// Welt-Streaming: Satellitenbilder (Esri oder Google Map Tiles), Hoehen (Terrarium), Strassen/Gebaeude (OpenStreetMap)
#include "world.h"
#include "net.h"
#include "stb_image.h"
#include <nlohmann/json.hpp>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

using json = nlohmann::json;
static const double DPI = 3.14159265358979323846;
static const double EARTH = 40075016.686;
static double lonToMx(double lon) { return (lon + 180.0) / 360.0; }
static double latToMy(double lat) { return (1.0 - std::asinh(std::tan(lat * DPI / 180.0)) / DPI) / 2.0; }
static double myToLat(double my) { return std::atan(std::sinh(DPI * (1.0 - 2.0 * my))) * 180.0 / DPI; }

struct Pool {
    std::deque<std::function<void()>> q;
    std::mutex m;
    std::condition_variable cv;
    void start(int n) {
        for (int i = 0; i < n; i++)
            std::thread([this] {
                for (;;) {
                    std::function<void()> f;
                    {
                        std::unique_lock<std::mutex> lk(m);
                        cv.wait(lk, [&] { return !q.empty(); });
                        f = std::move(q.front());
                        q.pop_front();
                    }
                    f();
                }
            }).detach();
    }
    void push(std::function<void()> f) {
        { std::lock_guard<std::mutex> lk(m); q.push_back(std::move(f)); }
        cv.notify_one();
    }
    void clear() { std::lock_guard<std::mutex> lk(m); q.clear(); }
};

// ---------- Hoehenmodell: Terrarium-Kacheln Zoom 12, als Dreiecksnetz interpoliert ----------
struct ElevTile { std::vector<float> h; };
struct Elev {
    std::unordered_map<uint32_t, std::shared_ptr<ElevTile>> m;
    std::mutex mx, loadMx;
    std::shared_ptr<ElevTile> get(int tx, int ty, bool block) {
        uint32_t key = ((uint32_t)tx << 16) | (uint32_t)ty;
        {
            std::lock_guard<std::mutex> lk(mx);
            auto it = m.find(key);
            if (it != m.end()) return it->second;
        }
        if (!block) return nullptr;
        std::lock_guard<std::mutex> ll(loadMx);
        {
            std::lock_guard<std::mutex> lk(mx);
            auto it = m.find(key);
            if (it != m.end()) return it->second;
        }
        auto t = std::make_shared<ElevTile>();
        t->h.assign(65536, 0.0f);
        char url[256], cache[128];
        snprintf(url, sizeof url, "https://s3.amazonaws.com/elevation-tiles-prod/terrarium/12/%d/%d.png", tx, ty);
        snprintf(cache, sizeof cache, "cache/elev/12_%d_%d.png", tx, ty);
        std::vector<uint8_t> data;
        bool ok = false;
        for (int attempt = 0; attempt < 3 && !ok; attempt++) {
            if (!cachedGet(url, cache, data)) continue;
            int w, h, c;
            uint8_t* px = stbi_load_from_memory(data.data(), (int)data.size(), &w, &h, &c, 3);
            if (px && w == 256 && h == 256) {
                for (int i = 0; i < 65536; i++) t->h[i] = px[i * 3] * 256.0f + px[i * 3 + 1] + px[i * 3 + 2] / 256.0f - 32768.0f;
                ok = true;
            }
            if (px) stbi_image_free(px);
        }
        if (!ok) logMsg("Hoehendaten %d/%d nicht ladbar - verwende flaches Gelaende", tx, ty);
        std::lock_guard<std::mutex> lk(mx);
        m[key] = t;
        return t;
    }
    bool px(int ix, int iy, float& h, bool block) {
        ix = clampv(ix, 0, (1 << 20) - 1); iy = clampv(iy, 0, (1 << 20) - 1);
        auto t = get(ix >> 8, iy >> 8, block);
        if (!t) return false;
        h = t->h[(iy & 255) * 256 + (ix & 255)];
        return true;
    }
    // Gleiche Dreiecksaufteilung wie das Gelaendenetz, damit Auto und Strassen exakt auf der Oberflaeche liegen
    bool height(double mxx, double myy, float& h, bool block) {
        double gx = mxx * 1048576.0, gy = myy * 1048576.0;
        int ix = (int)std::floor(gx), iy = (int)std::floor(gy);
        float fx = (float)(gx - ix), fy = (float)(gy - iy);
        float h00, h10, h01, h11;
        if (!px(ix, iy, h00, block) || !px(ix + 1, iy, h10, block) || !px(ix, iy + 1, h01, block) || !px(ix + 1, iy + 1, h11, block)) return false;
        if (fx + fy <= 1.0f) h = h00 + fx * (h10 - h00) + fy * (h01 - h00);
        else h = h11 + (1 - fx) * (h01 - h11) + (1 - fy) * (h10 - h11);
        return true;
    }
};

struct Tile { int z, x, y; Mesh mesh; GLuint tex = 0; double touched = 0; };
struct TileRes { int gen, z, x, y; bool ok = false; std::vector<Vertex> v; std::vector<uint8_t> rgb; int w = 0, h = 0; };
struct Chunk {
    Mesh roads, blds;
    std::vector<RoadSeg> segs;
    std::vector<std::string> names;
    std::vector<Control> controls;
    std::vector<Building> buildings;
    float x0 = 0, z0 = 0, x1 = 0, z1 = 0;
    double touched = 0;
    bool ok = false;
};
struct ChunkRes {
    int gen, cx, cy;
    bool ok = false;
    std::vector<Vertex> roadV, bldV;
    std::vector<RoadSeg> segs;
    std::vector<std::string> names;
    std::vector<Control> controls;
    std::vector<Building> buildings;
    float x0, z0, x1, z1;
};

struct World::Impl {
    Pool tilePool, osmPool;
    Elev elev;
    std::mutex rm;
    std::vector<TileRes> tileRes;
    std::vector<ChunkRes> chunkRes;
    std::unordered_map<uint64_t, Tile> tiles;
    std::unordered_map<uint64_t, double> requested; // 0 = unterwegs, sonst fruehester Neuversuch
    std::unordered_map<uint64_t, Chunk> chunks;
    std::unordered_map<uint64_t, double> chunkReq;
    Chunk custom;
    int version = 0;
    std::string googleKey, googleSession;
    int zoom[3] = {12, 15, 18};
    int radius[3] = {2, 3, 4};
    double originTime = -1;
    std::atomic<int> inflight{0};
};

static uint64_t tkey(int z, int x, int y) { return ((uint64_t)z << 56) | ((uint64_t)(uint32_t)x << 28) | (uint64_t)(uint32_t)y; }

// ---------- Gelaendekachel ----------
static void buildTile(World::Impl* d, GeoRef g, int z, int x, int y) {
    TileRes r;
    r.gen = g.gen; r.z = z; r.x = x; r.y = y;
    char url[512], cache[160] = "";
    if (!d->googleSession.empty())
        snprintf(url, sizeof url, "https://tile.googleapis.com/v1/2dtiles/%d/%d/%d?session=%s&key=%s", z, x, y, d->googleSession.c_str(), d->googleKey.c_str());
    else {
        snprintf(url, sizeof url, "https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/%d/%d/%d", z, y, x);
        snprintf(cache, sizeof cache, "cache/img/%d/%d_%d.jpg", z, x, y);
    }
    std::vector<uint8_t> data;
    if (cachedGet(url, cache, data)) {
        int w, h, c;
        uint8_t* px = stbi_load_from_memory(data.data(), (int)data.size(), &w, &h, &c, 3);
        if (px) {
            r.rgb.assign(px, px + (size_t)w * h * 3);
            r.w = w; r.h = h;
            stbi_image_free(px);
            int span = 1 << (20 - z);          // Hoehenpixel pro Kachelkante
            int cells = std::min(span, 32), stride = span / cells;
            int n = cells + 1;
            std::vector<Vec3> p(n * n);
            for (int j = 0; j < n; j++)
                for (int i = 0; i < n; i++) {
                    int ix = x * span + i * stride, iy = y * span + j * stride;
                    float hh = 0;
                    d->elev.px(ix, iy, hh, true);
                    p[j * n + i] = {(float)((ix / 1048576.0 - g.mx0) * g.S), hh, (float)((iy / 1048576.0 - g.my0) * g.S)};
                }
            r.v.reserve(cells * cells * 6);
            auto V = [&](int i, int j) {
                Vec3 q = p[j * n + i];
                r.v.push_back({{q.x, q.y, q.z}, {0, 1, 0}, {(float)i / cells, (float)j / cells}, Color{}});
            };
            for (int j = 0; j < cells; j++)
                for (int i = 0; i < cells; i++) { V(i, j); V(i, j + 1); V(i + 1, j); V(i + 1, j); V(i, j + 1); V(i + 1, j + 1); }
            r.ok = true;
        }
    }
    std::lock_guard<std::mutex> lk(d->rm);
    d->tileRes.push_back(std::move(r));
}

// ---------- OpenStreetMap ----------
static bool fetchOsm(int cx, int cy, json& j) {
    char cache[128];
    snprintf(cache, sizeof cache, "cache/osm/14_%d_%d.json", cx, cy);
    std::vector<uint8_t> data;
    if (readFile(cache, data)) {
        j = json::parse(data.begin(), data.end(), nullptr, false);
        if (!j.is_discarded() && j.contains("elements")) return true;
    }
    double n = myToLat(cy / 16384.0), s = myToLat((cy + 1) / 16384.0), w = cx / 16384.0 * 360 - 180, e = (cx + 1) / 16384.0 * 360 - 180;
    char bb[128], q[1024];
    snprintf(bb, sizeof bb, "(%.6f,%.6f,%.6f,%.6f)", s, w, n, e);
    snprintf(q, sizeof q, "[out:json][timeout:40];(way[\"highway\"]%s;way[\"building\"]%s;node[\"highway\"~\"^(traffic_signals|stop|give_way)$\"]%s;);out body geom qt;", bb, bb, bb);
    static const char* servers[] = {"https://overpass.openstreetmap.fr/api/interpreter", "https://overpass-api.de/api/interpreter",
                                    "https://overpass.private.coffee/api/interpreter"};
    for (int attempt = 0; attempt < 6; attempt++) {
        std::string url = std::string(servers[attempt % 3]) + "?data=" + urlEncode(q);
        int st = 0;
        if (httpRequest(url, data, st) && st == 200) {
            j = json::parse(data.begin(), data.end(), nullptr, false);
            if (!j.is_discarded() && j.contains("elements") && !j.contains("remark")) {
                writeFile(cache, data);
                return true;
            }
        }
        logMsg("OSM-Abruf %d/%d fehlgeschlagen (HTTP %d), neuer Versuch ...", cx, cy, st);
        std::this_thread::sleep_for(std::chrono::milliseconds(1500 * (attempt + 1)));
    }
    return false;
}

static std::string tagOf(const json& e, const char* k) {
    auto t = e.find("tags");
    if (t == e.end()) return "";
    auto v = t->find(k);
    if (v == t->end() || !v->is_string()) return "";
    return v->get<std::string>();
}
static float hash2(int a, int b) {
    uint32_t h = (uint32_t)a * 374761393u + (uint32_t)b * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return ((h ^ (h >> 16)) & 0xFFFF) / 65535.0f;
}
static int roadClass(const std::string& h) {
    if (h == "motorway" || h == "motorway_link") return 0;
    if (h == "trunk" || h == "trunk_link") return 1;
    if (h == "primary" || h == "primary_link") return 2;
    if (h == "secondary" || h == "secondary_link") return 3;
    if (h == "tertiary" || h == "tertiary_link") return 4;
    if (h == "residential" || h == "unclassified" || h == "living_street" || h == "road") return 5;
    if (h == "service") return 6;
    if (h == "track") return 7;
    return -1;
}
static float parseSpeed(const std::string& s, float def) {
    if (s.empty()) return def;
    if (isdigit((unsigned char)s[0])) {
        float v = std::strtof(s.c_str(), nullptr);
        if (s.find("mph") != std::string::npos) v *= 1.609f;
        return v > 1 ? v : def;
    }
    if (s.find("living_street") != std::string::npos) return 20;
    if (s.find("urban") != std::string::npos) return 50;
    if (s.find("rural") != std::string::npos) return 100;
    if (s.find("motorway") != std::string::npos) return 130;
    if (s == "walk") return 7;
    return def;
}

// Ohrenschnitt-Triangulierung fuer Dachflaechen (Polygon mit positiver Flaeche)
static void triangulate(const std::vector<float>& p, std::vector<int>& out) {
    int n = (int)p.size() / 2;
    std::vector<int> idx(n);
    for (int i = 0; i < n; i++) idx[i] = i;
    auto cr = [&](int a, int b, int c) { return (p[b * 2] - p[a * 2]) * (p[c * 2 + 1] - p[a * 2 + 1]) - (p[b * 2 + 1] - p[a * 2 + 1]) * (p[c * 2] - p[a * 2]); };
    int guard = 0;
    while (idx.size() > 3 && guard++ < n * 4) {
        bool found = false;
        int m = (int)idx.size();
        for (int i = 0; i < m && !found; i++) {
            int a = idx[(i + m - 1) % m], b = idx[i], c = idx[(i + 1) % m];
            if (cr(a, b, c) <= 1e-6f) continue;
            bool ok = true;
            for (int k : idx) {
                if (k == a || k == b || k == c) continue;
                if (cr(a, b, k) >= 0 && cr(b, c, k) >= 0 && cr(c, a, k) >= 0) { ok = false; break; }
            }
            if (ok) { out.push_back(a); out.push_back(b); out.push_back(c); idx.erase(idx.begin() + i); found = true; }
        }
        if (!found) break;
    }
    for (size_t k = 1; k + 1 < idx.size(); k++) { out.push_back(idx[0]); out.push_back(idx[k]); out.push_back(idx[k + 1]); }
}

struct WayTmp {
    int klass, lanes, flags, oneway, name;
    float hw, maxspeed;
    std::vector<float> px, pz;
    std::vector<int64_t> nodes;
};

static void buildChunk(World::Impl* d, GeoRef g, int cx, int cy) {
    ChunkRes r;
    r.gen = g.gen; r.cx = cx; r.cy = cy;
    r.x0 = (float)((cx / 16384.0 - g.mx0) * g.S); r.x1 = (float)(((cx + 1) / 16384.0 - g.mx0) * g.S);
    r.z0 = (float)((cy / 16384.0 - g.my0) * g.S); r.z1 = (float)(((cy + 1) / 16384.0 - g.my0) * g.S);
    json j;
    if (fetchOsm(cx, cy, j)) {
        auto H = [&](float x, float z) { float h = 0; d->elev.height(g.mx0 + x / g.S, g.my0 + z / g.S, h, true); return h; };
        auto inside = [&](float x, float z) { return x >= r.x0 && x < r.x1 && z >= r.z0 && z < r.z1; };
        struct NodeCtl { int type; int dir; };
        std::unordered_map<int64_t, NodeCtl> ctl;
        std::unordered_map<int64_t, int> junction;
        std::vector<WayTmp> ways;
        std::unordered_map<std::string, int> nameIdx;
        const json& els = j["elements"];
        for (const json& e : els) {
            std::string type = e.value("type", "");
            if (type == "node") {
                std::string h = tagOf(e, "highway");
                NodeCtl c{h == "traffic_signals" ? 0 : (h == "stop" ? 1 : 2), 0};
                std::string dir = tagOf(e, "direction");
                if (dir.empty()) dir = tagOf(e, "traffic_signals:direction");
                if (dir == "forward") c.dir = 1; else if (dir == "backward") c.dir = -1;
                ctl[e.value("id", (int64_t)0)] = c;
                continue;
            }
            if (type != "way" || !e.contains("geometry")) continue;
            const json& geom = e["geometry"];
            int n = (int)geom.size();
            if (n < 2) continue;
            std::vector<float> px(n), pz(n);
            for (int i = 0; i < n; i++) {
                double lat = geom[i].value("lat", 0.0), lon = geom[i].value("lon", 0.0);
                px[i] = (float)((lonToMx(lon) - g.mx0) * g.S);
                pz[i] = (float)((latToMy(lat) - g.my0) * g.S);
            }
            std::string hw = tagOf(e, "highway");
            if (!hw.empty()) {
                int klass = roadClass(hw);
                if (klass < 0 || tagOf(e, "area") == "yes") continue;
                WayTmp w;
                w.klass = klass;
                w.px = std::move(px); w.pz = std::move(pz);
                if (e.contains("nodes")) for (const json& id : e["nodes"]) w.nodes.push_back(id.get<int64_t>());
                std::string ow = tagOf(e, "oneway");
                w.oneway = (ow == "yes" || ow == "1" || ow == "true") ? 1 : (ow == "-1" ? -1 : 0);
                if (ow.empty() && (klass == 0 || tagOf(e, "junction") == "roundabout")) w.oneway = 1;
                int lanesTag = atoi(tagOf(e, "lanes").c_str());
                w.lanes = lanesTag > 0 ? std::min(lanesTag, 8) : (klass >= 6 ? 1 : (w.oneway && klass >= 2 ? 1 : 2));
                static const float laneW[8] = {3.75f, 3.5f, 3.5f, 3.25f, 3.25f, 2.9f, 3.4f, 2.8f};
                float width = w.lanes * laneW[klass] + (klass <= 1 ? 1.5f : 0.4f);
                float wt = std::strtof(tagOf(e, "width").c_str(), nullptr);
                if (wt >= 2.5f && wt <= 30) width = wt;
                w.hw = std::min(width * 0.5f, 25.0f);
                w.flags = ((klass <= 4 || lanesTag >= 2) && w.lanes >= 2 ? 1 : 0) | (klass <= 3 ? 2 : 0) | (w.oneway ? 4 : 0);
                float def = klass == 0 ? 130 : klass == 1 ? 100 : (klass >= 6 ? 30 : (hw == "living_street" ? 20 : 50));
                w.maxspeed = parseSpeed(tagOf(e, "maxspeed"), def);
                std::string name = tagOf(e, "name");
                if (name.empty()) name = tagOf(e, "ref");
                auto it = nameIdx.find(name);
                if (it == nameIdx.end()) { w.name = (int)r.names.size(); nameIdx[name] = w.name; r.names.push_back(name); }
                else w.name = it->second;
                if (klass <= 6) for (int64_t id : w.nodes) junction[id]++;
                ways.push_back(std::move(w));
            } else if (e.contains("tags") && e["tags"].contains("building") && n >= 4 && n <= 300) {
                // ----- Gebaeude: Grundriss extrudieren -----
                if (px[0] == px[n - 1] && pz[0] == pz[n - 1]) n--;
                float cxm = 0, czm = 0, area = 0;
                for (int i = 0; i < n; i++) {
                    int k = (i + 1) % n;
                    cxm += px[i]; czm += pz[i];
                    area += px[i] * pz[k] - px[k] * pz[i];
                }
                cxm /= n; czm /= n; area *= 0.5f;
                if (std::fabs(area) < 6 || !inside(cxm, czm)) continue;
                std::vector<float> poly(n * 2);
                for (int i = 0; i < n; i++) {
                    int s = area > 0 ? i : n - 1 - i;
                    poly[i * 2] = px[s]; poly[i * 2 + 1] = pz[s];
                }
                int64_t id = e.value("id", (int64_t)0);
                float rnd = hash2((int)(id & 0xFFFFFF), 17), rnd2 = hash2((int)(id & 0xFFFFFF), 91);
                float ht = std::strtof(tagOf(e, "height").c_str(), nullptr);
                if (ht <= 0) { float lv = std::strtof(tagOf(e, "building:levels").c_str(), nullptr); if (lv > 0) ht = lv * 3.1f + 1.0f; }
                if (ht <= 0) ht = (std::fabs(area) > 600 ? 9.0f : 6.0f) + rnd * 6.0f;
                ht = std::min(ht, 400.0f);
                float lo = 1e9f, hi = -1e9f;
                Building b;
                b.minx = b.minz = 1e9f; b.maxx = b.maxz = -1e9f;
                for (int i = 0; i < n; i++) {
                    float h = H(poly[i * 2], poly[i * 2 + 1]);
                    lo = std::min(lo, h); hi = std::max(hi, h);
                    b.minx = std::min(b.minx, poly[i * 2]); b.maxx = std::max(b.maxx, poly[i * 2]);
                    b.minz = std::min(b.minz, poly[i * 2 + 1]); b.maxz = std::max(b.maxz, poly[i * 2 + 1]);
                }
                float base = lo - 1.0f, top = hi + ht;
                static const Color pal[6] = {{214, 204, 186, 255}, {196, 186, 172, 255}, {222, 214, 204, 255}, {180, 170, 160, 255}, {205, 190, 165, 255}, {190, 196, 200, 255}};
                Color wall = pal[(int)(rnd * 5.99f)];
                uint8_t rg = (uint8_t)(95 + rnd2 * 50);
                Color roof{rg, (uint8_t)(rg - 8), (uint8_t)(rg - 12), 0};
                float u = 0;
                for (int i = 0; i < n; i++) {
                    int k = (i + 1) % n;
                    float ax = poly[i * 2], az = poly[i * 2 + 1], bx = poly[k * 2], bz = poly[k * 2 + 1];
                    float len = std::hypot(bx - ax, bz - az);
                    if (len < 0.01f) continue;
                    Vec3 nn{(bz - az) / len, 0, -(bx - ax) / len};
                    auto V = [&](float x, float y, float z, float uu) { r.bldV.push_back({{x, y, z}, {nn.x, nn.y, nn.z}, {uu, y - (hi + 0.3f)}, wall}); };
                    V(ax, base, az, u); V(bx, base, bz, u + len); V(bx, top, bz, u + len);
                    V(ax, base, az, u); V(bx, top, bz, u + len); V(ax, top, az, u);
                    u += len;
                }
                std::vector<int> tri;
                triangulate(poly, tri);
                for (int t : tri) r.bldV.push_back({{poly[t * 2], top, poly[t * 2 + 1]}, {0, 1, 0}, {0, 0}, roof});
                b.poly = std::move(poly);
                r.buildings.push_back(std::move(b));
            }
        }
        // ----- Strassen: kleine Strassen zuerst, grosse zuletzt (liegen beim Zeichnen oben) -----
        std::stable_sort(ways.begin(), ways.end(), [](const WayTmp& a, const WayTmp& b) { return a.klass > b.klass; });
        for (const WayTmp& w : ways) {
            int n = (int)w.px.size();
            for (int i = 0; i + 1 < n; i++) {
                if (!inside((w.px[i] + w.px[i + 1]) * 0.5f, (w.pz[i] + w.pz[i + 1]) * 0.5f)) continue;
                r.segs.push_back({w.px[i], w.pz[i], w.px[i + 1], w.pz[i + 1], w.hw, w.maxspeed, (int8_t)w.oneway, (uint8_t)w.klass, (uint8_t)w.lanes, (uint8_t)(w.flags & 1), w.name});
            }
            std::vector<float> rx, rz, rd;
            float dist = 0;
            for (int i = 0; i + 1 < n; i++) {
                float len = std::hypot(w.px[i + 1] - w.px[i], w.pz[i + 1] - w.pz[i]);
                int steps = std::max(1, (int)std::ceil(len / 6.0f));
                for (int s = 0; s < steps; s++) {
                    float t = (float)s / steps;
                    rx.push_back(w.px[i] + (w.px[i + 1] - w.px[i]) * t);
                    rz.push_back(w.pz[i] + (w.pz[i + 1] - w.pz[i]) * t);
                    rd.push_back(dist + len * t);
                }
                dist += len;
            }
            rx.push_back(w.px[n - 1]); rz.push_back(w.pz[n - 1]); rd.push_back(dist);
            int m = (int)rx.size();
            Color col{(uint8_t)w.lanes, (uint8_t)w.flags, (uint8_t)w.klass, (uint8_t)std::min(255.0f, w.hw * 10.0f)};
            float hwq = col.a / 10.0f;
            auto edge = [&](int k, float side, Vertex& out) {
                int a = std::max(k - 1, 0), b = std::min(k + 1, m - 1);
                float tx = rx[b] - rx[a], tz = rz[b] - rz[a], l = std::hypot(tx, tz);
                if (l < 1e-4f) { tx = 1; tz = 0; l = 1; }
                float x = rx[k] + (-tz / l) * hwq * side, z = rz[k] + (tx / l) * hwq * side;
                out = {{x, H(x, z) + 0.03f, z}, {0, 1, 0}, {hwq * side, rd[k]}, col};
            };
            for (int k = 0; k + 1 < m; k++) {
                if (!inside((rx[k] + rx[k + 1]) * 0.5f, (rz[k] + rz[k + 1]) * 0.5f)) continue;
                Vertex L0, R0, L1, R1;
                edge(k, -1, L0); edge(k, 1, R0); edge(k + 1, -1, L1); edge(k + 1, 1, R1);
                r.roadV.push_back(L0); r.roadV.push_back(R0); r.roadV.push_back(R1);
                r.roadV.push_back(L0); r.roadV.push_back(R1); r.roadV.push_back(L1);
            }
            // ----- Ampeln und Schilder entlang dieses Wegs -----
            if (w.klass > 6 || (int)w.nodes.size() != n) continue;
            for (int i = 0; i < n; i++) {
                auto it = ctl.find(w.nodes[i]);
                if (it == ctl.end() || !inside(w.px[i], w.pz[i])) continue;
                int type = it->second.type, jn = junction[w.nodes[i]];
                if (type != 0 && jn >= 2) continue; // Schild direkt auf Kreuzungsknoten: Richtung unklar
                auto add = [&](int sign) {
                    int k = i - sign;
                    if (k < 0 || k >= n) return;
                    if ((w.oneway == 1 && sign < 0) || (w.oneway == -1 && sign > 0)) return;
                    float dx = w.px[i] - w.px[k], dz = w.pz[i] - w.pz[k], len = std::hypot(dx, dz);
                    if (len < 0.5f) return;
                    dx /= len; dz /= len;
                    float off = std::min(jn >= 2 ? 7.0f : 0.0f, len * 0.8f);
                    Control c;
                    c.pos = {w.px[i] - dx * off, 0, w.pz[i] - dz * off};
                    c.pos.y = H(c.pos.x, c.pos.z);
                    c.dx = dx; c.dz = dz; c.hw = w.hw; c.type = type; c.oneway = w.oneway != 0;
                    float ang = std::atan2(dz, dx);
                    if (ang < 0) ang += PI;
                    c.group = (ang > PI / 4 && ang < 3 * PI / 4) ? 1 : 0;
                    c.phase = hash2((int)std::floor(w.px[i] / 90.0f), (int)std::floor(w.pz[i] / 90.0f));
                    r.controls.push_back(c);
                };
                int dir = it->second.dir;
                if (dir != 0) add(dir);
                else if (type == 0) { add(1); add(-1); }
                else if (i == 0) add(-1);
                else if (i == n - 1) add(1);
                else add(i < n - 1 - i ? -1 : 1);
            }
        }
        r.ok = true;
    }
    std::lock_guard<std::mutex> lk(d->rm);
    d->chunkRes.push_back(std::move(r));
}

// ---------- World ----------
void World::init(const Config& cfg) {
    d = new Impl;
    d->zoom[2] = clampv(cfg.nearZoom, 16, 19);
    d->tilePool.start(8);
    d->osmPool.start(1);
    if (!cfg.googleKey.empty()) {
        std::vector<uint8_t> out;
        int st = 0;
        std::string url = "https://tile.googleapis.com/v1/createSession?key=" + cfg.googleKey;
        if (httpRequest(url, out, st, "{\"mapType\":\"satellite\",\"language\":\"de-DE\",\"region\":\"AT\"}") && st == 200) {
            json j = json::parse(out.begin(), out.end(), nullptr, false);
            if (!j.is_discarded() && j.contains("session")) {
                d->googleKey = cfg.googleKey;
                d->googleSession = j["session"].get<std::string>();
                logMsg("Google Map Tiles aktiv");
            }
        }
        if (d->googleSession.empty()) logMsg("Google-Sitzung fehlgeschlagen (HTTP %d) - verwende Esri-Satellitenbilder", st);
    }
}

std::string World::attribution() const {
    return std::string("Bilder: ") + (d->googleSession.empty() ? "Esri, Maxar, Earthstar Geographics" : "Google") +
           "  |  Karte: \xC2\xA9 OpenStreetMap-Mitwirkende  |  H\xC3\xB6hen: Mapzen/AWS Terrain Tiles";
}

void World::setOrigin(double lat, double lon) {
    g.gen++;
    g.lat0 = lat; g.lon0 = lon;
    g.mx0 = lonToMx(lon); g.my0 = latToMy(lat);
    g.S = EARTH * std::cos(lat * DPI / 180.0);
    d->tilePool.clear();
    d->osmPool.clear();
    for (auto& t : d->tiles) { t.second.mesh.destroy(); if (t.second.tex) glDeleteTextures(1, &t.second.tex); }
    for (auto& c : d->chunks) { c.second.roads.destroy(); c.second.blds.destroy(); }
    d->tiles.clear(); d->chunks.clear(); d->requested.clear(); d->chunkReq.clear();
    d->custom.segs.clear();
    d->version++;
    d->originTime = -1;
    logMsg("Neuer Ursprung: %.5f, %.5f", lat, lon);
}

void World::toLatLon(float x, float z, double& lat, double& lon) const {
    lon = (g.mx0 + x / g.S) * 360.0 - 180.0;
    lat = myToLat(g.my0 + z / g.S);
}

void World::toLocal(double lat, double lon, float& x, float& z) const {
    x = (float)((lonToMx(lon) - g.mx0) * g.S);
    z = (float)((latToMy(lat) - g.my0) * g.S);
}

int World::pendingJobs() const {
    int n = 0;
    for (auto& r : d->requested) if (r.second == 0) n++;
    for (auto& r : d->chunkReq) if (r.second == 0) n++;
    return n;
}

void World::update(Vec3 p, double now) {
    Impl& D = *d;
    if (D.originTime < 0) D.originTime = now;
    std::vector<TileRes> tr;
    std::vector<ChunkRes> cr;
    {
        std::lock_guard<std::mutex> lk(D.rm);
        size_t take = std::min<size_t>(D.tileRes.size(), 10);
        tr.assign(std::make_move_iterator(D.tileRes.begin()), std::make_move_iterator(D.tileRes.begin() + take));
        D.tileRes.erase(D.tileRes.begin(), D.tileRes.begin() + take);
        if (!D.chunkRes.empty()) { cr.push_back(std::move(D.chunkRes.front())); D.chunkRes.erase(D.chunkRes.begin()); }
    }
    for (TileRes& r : tr) {
        if (r.gen != g.gen) continue;
        uint64_t k = tkey(r.z, r.x, r.y);
        if (!r.ok) { D.requested[k] = now + 10; continue; }
        D.requested.erase(k);
        Tile& t = D.tiles[k];
        t.z = r.z; t.x = r.x; t.y = r.y; t.touched = now;
        t.mesh.create(r.v.data(), (int)r.v.size());
        t.tex = gfxTexture(r.rgb.data(), r.w, r.h, 3, true);
    }
    for (ChunkRes& r : cr) {
        if (r.gen != g.gen) continue;
        uint64_t k = tkey(14, r.cx, r.cy);
        if (!r.ok) { D.chunkReq[k] = now + 30; logMsg("OSM-Kachel %d/%d nicht ladbar", r.cx, r.cy); }
        else D.chunkReq.erase(k);
        Chunk& c = D.chunks[k];
        D.version++;
        c.ok = r.ok; c.touched = now;
        c.x0 = r.x0; c.x1 = r.x1; c.z0 = r.z0; c.z1 = r.z1;
        c.roads.create(r.roadV.data(), (int)r.roadV.size());
        c.blds.create(r.bldV.data(), (int)r.bldV.size());
        c.segs = std::move(r.segs); c.names = std::move(r.names);
        c.controls = std::move(r.controls); c.buildings = std::move(r.buildings);
        if (r.ok) logMsg("OSM-Kachel %d/%d: %d Strassenabschnitte, %d Gebaeude, %d Ampeln/Schilder", r.cx, r.cy, (int)c.segs.size(), (int)c.buildings.size(), (int)c.controls.size());
    }

    double mx = g.mx0 + p.x / g.S, my = g.my0 + p.z / g.S;
    struct Want { int z, x, y; float dist; };
    std::vector<Want> want;
    for (int L = 2; L >= 0; L--) {
        int z = D.zoom[L], R = D.radius[L];
        double n = (double)(1 << z);
        int cx = (int)std::floor(mx * n), cy = (int)std::floor(my * n);
        for (int dy = -R; dy <= R; dy++)
            for (int dx = -R; dx <= R; dx++) {
                int x = cx + dx, y = cy + dy;
                if (x < 0 || y < 0 || x >= (1 << z) || y >= (1 << z)) continue;
                uint64_t k = tkey(z, x, y);
                auto it = D.tiles.find(k);
                if (it != D.tiles.end()) { it->second.touched = now; continue; }
                auto rq = D.requested.find(k);
                if (rq != D.requested.end() && (rq->second == 0 || now < rq->second)) continue;
                want.push_back({z, x, y, (float)(dx * dx + dy * dy) + (2 - L) * 3.0f});
            }
    }
    std::sort(want.begin(), want.end(), [](const Want& a, const Want& b) { return a.dist < b.dist; });
    for (const Want& w : want) {
        D.requested[tkey(w.z, w.x, w.y)] = 0;
        GeoRef gg = g;
        Impl* dp = d;
        int z = w.z, x = w.x, y = w.y;
        D.tilePool.push([dp, gg, z, x, y] { buildTile(dp, gg, z, x, y); });
    }
    for (auto it = D.tiles.begin(); it != D.tiles.end();) {
        if (it->second.touched < now - 15) {
            it->second.mesh.destroy();
            if (it->second.tex) glDeleteTextures(1, &it->second.tex);
            it = D.tiles.erase(it);
        } else ++it;
    }
    int ccx = (int)std::floor(mx * 16384.0), ccy = (int)std::floor(my * 16384.0);
    for (int ring = 0; ring <= 1; ring++)
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++) {
                if ((ring == 0) != (dx == 0 && dy == 0)) continue;
                uint64_t k = tkey(14, ccx + dx, ccy + dy);
                auto it = D.chunks.find(k);
                auto rq = D.chunkReq.find(k);
                bool retry = rq != D.chunkReq.end() && rq->second != 0 && now >= rq->second;
                if (it != D.chunks.end()) { it->second.touched = now; if (!retry) continue; }
                if (rq != D.chunkReq.end() && !retry) continue;
                D.chunkReq[k] = 0;
                GeoRef gg = g;
                Impl* dp = d;
                int x = ccx + dx, y = ccy + dy;
                D.osmPool.push([dp, gg, x, y] { buildChunk(dp, gg, x, y); });
            }
    for (auto it = D.chunks.begin(); it != D.chunks.end();) {
        if (it->second.touched < now - 30) {
            it->second.roads.destroy(); it->second.blds.destroy();
            it = D.chunks.erase(it);
            D.version++;
        } else ++it;
    }
}

bool World::ready(Vec3 p, double now) {
    float h;
    if (!height(p.x, p.z, h)) return false;
    double mx = g.mx0 + p.x / g.S, my = g.my0 + p.z / g.S;
    if (d->originTime >= 0 && now - d->originTime > 45) return true;
    double n = (double)(1 << d->zoom[2]);
    if (!d->tiles.count(tkey(d->zoom[2], (int)std::floor(mx * n), (int)std::floor(my * n)))) return false;
    return d->chunks.count(tkey(14, (int)std::floor(mx * 16384.0), (int)std::floor(my * 16384.0))) > 0;
}

bool World::height(float x, float z, float& h) { return d->elev.height(g.mx0 + x / g.S, g.my0 + z / g.S, h, false); }

Vec3 World::normal(float x, float z) {
    float a, b, c, e;
    const float s = 0.7f;
    if (!height(x - s, z, a) || !height(x + s, z, b) || !height(x, z - s, c) || !height(x, z + s, e)) return {0, 1, 0};
    return normalize({a - b, 2 * s, c - e});
}

void World::setCustomRoads(const std::vector<Vertex>& verts, const std::vector<RoadSeg>& segs) {
    Chunk& c = d->custom;
    c.roads.create(verts.data(), (int)verts.size());
    c.segs = segs;
    c.names = {"Eigene Stra\xC3\x9F" "e"};
    c.x0 = c.z0 = -1e9f; c.x1 = c.z1 = 1e9f;
    d->version++;
}
int World::roadVersion() const { return d->version; }
void World::collectSegs(std::vector<RoadSeg>& out) const {
    out = d->custom.segs;
    for (auto& kv : d->chunks) out.insert(out.end(), kv.second.segs.begin(), kv.second.segs.end());
}
void World::drawCustomRoads() { d->custom.roads.draw(); }

bool World::surface(float x, float z, float yRef, float& h) {
    if (!height(x, z, h)) return false;
    float hh;
    if (surfaceHook && surfaceHook(x, z, yRef, hh) && hh > h) h = hh;
    return true;
}
Vec3 World::surfaceNormal(float x, float z, float yRef) {
    float a, b, c, e;
    const float s = 0.5f;
    if (!surface(x - s, z, yRef, a) || !surface(x + s, z, yRef, b) || !surface(x, z - s, yRef, c) || !surface(x, z + s, yRef, e)) return {0, 1, 0};
    Vec3 n = normalize({a - b, 2 * s, c - e});
    return n.y < 0.5f ? Vec3{0, 1, 0} : n; // Kanten von Objekten nicht als Steilhang werten
}

RoadHit World::nearestRoad(float x, float z, float maxDist) {
    RoadHit best;
    float bestScore = 1e9f;
    std::vector<Chunk*> list{&d->custom};
    for (auto& kv : d->chunks) list.push_back(&kv.second);
    for (Chunk* cp : list) {
        Chunk& c = *cp;
        if (x < c.x0 - 120 || x > c.x1 + 120 || z < c.z0 - 120 || z > c.z1 + 120) continue;
        for (const RoadSeg& s : c.segs) {
            float vx = s.bx - s.ax, vz = s.bz - s.az, l2 = vx * vx + vz * vz;
            if (l2 < 1e-6f) continue;
            float t = clampv(((x - s.ax) * vx + (z - s.az) * vz) / l2, 0.0f, 1.0f);
            float qx = s.ax + vx * t, qz = s.az + vz * t;
            float dd = std::hypot(x - qx, z - qz);
            float score = dd - s.hw * 0.5f - (cp == &d->custom ? 3.0f : 0.0f); // eigene Strassen haben Vorrang
            if (dd > maxDist || score >= bestScore) continue;
            bestScore = score;
            float l = std::sqrt(l2);
            best.found = true; best.dist = dd; best.seg = &s;
            best.dx = vx / l; best.dz = vz / l; best.px = qx; best.pz = qz;
            best.side = (x - s.ax) * (-best.dz) + (z - s.az) * best.dx;
            best.name = c.names[s.name];
        }
    }
    return best;
}

bool World::insideBuilding(float x, float z) {
    for (auto& kv : d->chunks)
        for (const Building& b : kv.second.buildings) {
            if (x < b.minx || x > b.maxx || z < b.minz || z > b.maxz) continue;
            bool in = false;
            size_t n = b.poly.size() / 2;
            for (size_t i = 0, k = n - 1; i < n; k = i++) {
                float xi = b.poly[i * 2], zi = b.poly[i * 2 + 1], xk = b.poly[k * 2], zk = b.poly[k * 2 + 1];
                if (((zi > z) != (zk > z)) && (x < (xk - xi) * (z - zi) / (zk - zi) + xi)) in = !in;
            }
            if (in) return true;
        }
    return false;
}

void World::forControls(Vec3 p, float radius, const std::function<void(const Control&)>& fn) {
    for (auto& kv : d->chunks)
        for (const Control& c : kv.second.controls)
            if (std::fabs(c.pos.x - p.x) < radius && std::fabs(c.pos.z - p.z) < radius) fn(c);
}

int World::signalState(const Control& c, double t) {
    double tt = std::fmod(t + c.phase * 60.0 + (c.group ? 30.0 : 0.0), 60.0);
    if (tt < 24) return 2;
    if (tt < 27) return 3;
    if (tt < 59) return 0;
    return 1;
}

void World::drawTerrain(int level) {
    int z = d->zoom[level];
    for (auto& kv : d->tiles) {
        if (kv.second.z != z) continue;
        gfxBindTexture(kv.second.tex);
        kv.second.mesh.draw();
    }
}
void World::drawRoads() { for (auto& kv : d->chunks) kv.second.roads.draw(); }
void World::drawBuildings() { for (auto& kv : d->chunks) kv.second.blds.draw(); }
