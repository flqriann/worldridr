// Welt-Streaming: Satellitenbilder (Esri oder Google Map Tiles), Hoehen (Terrarium), Strassen/Gebaeude (OpenStreetMap)
#include "world.h"
#include "net.h"
#include "stb_image.h"
#include <nlohmann/json.hpp>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
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
        // letzte Kachel pro Thread merken: spart Sperre und Hash-Suche bei den vielen Nachbarzugriffen
        struct Last { const Elev* e = nullptr; int tx = -1, ty = -1; std::shared_ptr<ElevTile> t; };
        static thread_local Last last;
        int tx = ix >> 8, ty = iy >> 8;
        if (last.e != this || last.tx != tx || last.ty != ty || !last.t) {
            auto t = get(tx, ty, block);
            if (!t) return false;
            last.e = this; last.tx = tx; last.ty = ty; last.t = t;
        }
        h = last.t->h[(iy & 255) * 256 + (ix & 255)];
        return true;
    }
    // Glatte (bikubische Catmull-Rom) Hoehe an beliebiger Position in Hoehenpixeln (Zoom-20-Koordinaten)
    bool sample(double gx, double gy, float& h, bool block) {
        int ix = (int)std::floor(gx), iy = (int)std::floor(gy);
        float fx = (float)(gx - ix), fy = (float)(gy - iy);
        float P[4][4];
        bool have[4][4];
        for (int j = 0; j < 4; j++)
            for (int i = 0; i < 4; i++) have[j][i] = px(ix - 1 + i, iy - 1 + j, P[j][i], block);
        for (int j = 1; j <= 2; j++)
            for (int i = 1; i <= 2; i++) if (!have[j][i]) return false; // die vier inneren Pixel muessen da sein
        for (int j = 0; j < 4; j++)
            for (int i = 0; i < 4; i++)
                if (!have[j][i]) P[j][i] = P[clampv(j, 1, 2)][clampv(i, 1, 2)];
        auto w = [](float t, float* o) {
            float t2 = t * t, t3 = t2 * t;
            o[0] = 0.5f * (-t3 + 2 * t2 - t);
            o[1] = 0.5f * (3 * t3 - 5 * t2 + 2);
            o[2] = 0.5f * (-3 * t3 + 4 * t2 + t);
            o[3] = 0.5f * (t3 - t2);
        };
        float wx[4], wy[4];
        w(fx, wx); w(fy, wy);
        float s = 0;
        for (int j = 0; j < 4; j++) { float r = 0; for (int i = 0; i < 4; i++) r += P[j][i] * wx[i]; s += r * wy[j]; }
        h = s;
        return true;
    }
    // Hoehenmodell als glatte Flaeche, auf einem Gitter mit 1/4 Hoehenpixel (ca. 6 m) als Dreiecksnetz dargestellt.
    // Das Gelaendenetz verwendet dasselbe Gitter, damit Auto und Strassen exakt auf der Oberflaeche liegen.
    bool height(double mxx, double myy, float& h, bool block) {
        double gx = mxx * 1048576.0 * 4.0, gy = myy * 1048576.0 * 4.0;
        double ix = std::floor(gx), iy = std::floor(gy);
        float fx = (float)(gx - ix), fy = (float)(gy - iy);
        float h00, h10, h01, h11;
        if (!sample(ix * 0.25, iy * 0.25, h00, block) || !sample((ix + 1) * 0.25, iy * 0.25, h10, block) || !sample(ix * 0.25, (iy + 1) * 0.25, h01, block) ||
            !sample((ix + 1) * 0.25, (iy + 1) * 0.25, h11, block))
            return false;
        if (fx + fy <= 1.0f) h = h00 + fx * (h10 - h00) + fy * (h01 - h00);
        else h = h11 + (1 - fx) * (h01 - h11) + (1 - fy) * (h10 - h11);
        return true;
    }
};

struct Tile { int z, x, y; Mesh mesh; GLuint tex = 0; double touched = 0; Vec3 lo, hi; };
struct Cell { Mesh m; Vec3 lo, hi; };               // Teilnetz eines Chunks, einzeln sichtbarkeitsgeprueft
struct CellRes { std::vector<Vertex> v; Vec3 lo{1e9f, 1e9f, 1e9f}, hi{-1e9f, -1e9f, -1e9f}; };
static const int GC = 6;                              // Zellen pro Achse
struct TileRes { int gen, z, x, y; bool ok = false; std::vector<Vertex> v; std::vector<uint8_t> rgb; int w = 0, h = 0; };
struct Chunk {
    Mesh roads;                       // nur fuer selbst gebaute Strassen
    std::vector<Cell> sideCells, roadCells, bldCells;
    std::vector<RoadSeg> segs;
    std::vector<std::string> names;
    std::vector<Control> controls;
    std::vector<Building> buildings;
    std::vector<Poi> pois;
    float x0 = 0, z0 = 0, x1 = 0, z1 = 0;
    double touched = 0;
    bool ok = false;
};
struct ChunkRes {
    int gen, cx, cy;
    bool ok = false;
    std::vector<Vertex> roadV, bldV;
    std::vector<CellRes> sideCells, roadCells, bldCells;
    std::vector<RoadSeg> segs;
    std::vector<std::string> names;
    std::vector<Control> controls;
    std::vector<Building> buildings;
    std::vector<Poi> pois;
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
            int cells = std::min(span * 4, 32);  // nah: 1/4 Hoehenpixel (ca. 6 m) pro Masche, glatt interpoliert
            double stride = (double)span / cells;
            int n = cells + 1;
            std::vector<Vec3> p(n * n);
            for (int j = 0; j < n; j++)
                for (int i = 0; i < n; i++) {
                    double ix = (double)x * span + i * stride, iy = (double)y * span + j * stride;
                    float hh = 0;
                    d->elev.sample(ix, iy, hh, true);
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

// Dreiecke in ein Raster aus Zellen sortieren (Sichtbarkeitspruefung pro Zelle)
static void bucketCells(const std::vector<Vertex>& v, float x0, float z0, float x1, float z1, std::vector<CellRes>& out) {
    out.assign(GC * GC, CellRes{});
    float sx = (x1 - x0) / GC, sz = (z1 - z0) / GC;
    for (size_t i = 0; i + 2 < v.size(); i += 3) {
        float cx = (v[i].p[0] + v[i + 1].p[0] + v[i + 2].p[0]) / 3, cz = (v[i].p[2] + v[i + 1].p[2] + v[i + 2].p[2]) / 3;
        int gx = clampv((int)((cx - x0) / sx), 0, GC - 1), gz = clampv((int)((cz - z0) / sz), 0, GC - 1);
        CellRes& c = out[gz * GC + gx];
        for (int k = 0; k < 3; k++) {
            const Vertex& q = v[i + k];
            c.v.push_back(q);
            c.lo = {std::min(c.lo.x, q.p[0]), std::min(c.lo.y, q.p[1]), std::min(c.lo.z, q.p[2])};
            c.hi = {std::max(c.hi.x, q.p[0]), std::max(c.hi.y, q.p[1]), std::max(c.hi.z, q.p[2])};
        }
    }
}
static void uploadCells(std::vector<CellRes>& src, std::vector<Cell>& dst) {
    for (Cell& c : dst) c.m.destroy();
    dst.clear();
    for (CellRes& r : src) {
        if (r.v.empty()) continue;
        Cell c;
        c.m.create(r.v.data(), (int)r.v.size());
        c.lo = r.lo; c.hi = r.hi;
        dst.push_back(std::move(c));
    }
}
struct Frustum {
    float p[6][4];
    explicit Frustum(const Mat4& vp) {
        const float* m = vp.m;
        auto row = [&](int i, float* o) { for (int c = 0; c < 4; c++) o[c] = m[c * 4 + i]; };
        float r0[4], r1[4], r2[4], r3[4];
        row(0, r0); row(1, r1); row(2, r2); row(3, r3);
        for (int c = 0; c < 4; c++) {
            p[0][c] = r3[c] + r0[c]; p[1][c] = r3[c] - r0[c]; p[2][c] = r3[c] + r1[c];
            p[3][c] = r3[c] - r1[c]; p[4][c] = r3[c] + r2[c]; p[5][c] = r3[c] - r2[c];
        }
    }
    bool sees(Vec3 lo, Vec3 hi) const {
        for (int i = 0; i < 6; i++) {
            const float* q = p[i];
            Vec3 v{q[0] > 0 ? hi.x : lo.x, q[1] > 0 ? hi.y : lo.y, q[2] > 0 ? hi.z : lo.z};
            if (q[0] * v.x + q[1] * v.y + q[2] * v.z + q[3] < 0) return false;
        }
        return true;
    }
};
static float boxDist(Vec3 lo, Vec3 hi, Vec3 c) {
    float dx = std::max(std::max(lo.x - c.x, 0.0f), c.x - hi.x), dy = std::max(std::max(lo.y - c.y, 0.0f), c.y - hi.y), dz = std::max(std::max(lo.z - c.z, 0.0f), c.z - hi.z);
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// ---------- OpenStreetMap ----------
// ---------- Tankstellen und Supermaerkte (eigene, leichte Abfrage; getrennt vom Strassen-/Gebaeudecache) ----------
static bool fetchPoi(int cx, int cy, json& j) {
    char cache[128];
    snprintf(cache, sizeof cache, "cache/poi/14_%d_%d.json", cx, cy);
    std::vector<uint8_t> data;
    if (readFile(cache, data)) {
        j = json::parse(data.begin(), data.end(), nullptr, false);
        if (!j.is_discarded() && j.contains("elements")) return true;
    }
    double n = myToLat(cy / 16384.0), s = myToLat((cy + 1) / 16384.0), w = cx / 16384.0 * 360 - 180, e = (cx + 1) / 16384.0 * 360 - 180;
    char bb[128], q[768];
    snprintf(bb, sizeof bb, "(%.6f,%.6f,%.6f,%.6f)", s, w, n, e);
    snprintf(q, sizeof q, "[out:json][timeout:30];(node[\"amenity\"=\"fuel\"]%s;way[\"amenity\"=\"fuel\"]%s;node[\"shop\"=\"supermarket\"]%s;way[\"shop\"=\"supermarket\"]%s;);out center tags;", bb, bb, bb, bb);
    static const char* servers[] = {"https://overpass.openstreetmap.fr/api/interpreter", "https://overpass-api.de/api/interpreter", "https://overpass.private.coffee/api/interpreter"};
    for (int attempt = 0; attempt < 3; attempt++) {
        std::string url = std::string(servers[attempt % 3]) + "?data=" + urlEncode(q);
        int st = 0;
        if (httpRequest(url, data, st) && st == 200) {
            j = json::parse(data.begin(), data.end(), nullptr, false);
            if (!j.is_discarded() && j.contains("elements") && !j.contains("remark")) { writeFile(cache, data); return true; }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1200 * (attempt + 1)));
    }
    return false;
}

// Quader fuer Tankstellen-Modelle (Alpha 0 = ohne Fenstermuster)
static void pushBox(std::vector<Vertex>& out, Vec3 c, Vec3 h, float ux, float uz, Color col) {
    col.a = 0;
    Vec3 U{ux, 0, uz}, Vv{-uz, 0, ux}, Y{0, 1, 0};
    auto P = [&](float a, float b, float y) { return c + U * (a * h.x) + Vv * (b * h.z) + Y * (y * h.y); };
    auto quad = [&](Vec3 a, Vec3 b, Vec3 cc, Vec3 d, Vec3 n) {
        Vertex v[4];
        Vec3 pp[4] = {a, b, cc, d};
        for (int i = 0; i < 4; i++) v[i] = {{pp[i].x, pp[i].y, pp[i].z}, {n.x, n.y, n.z}, {0, 0}, col};
        out.push_back(v[0]); out.push_back(v[1]); out.push_back(v[2]);
        out.push_back(v[0]); out.push_back(v[2]); out.push_back(v[3]);
    };
    quad(P(-1, -1, 1), P(1, -1, 1), P(1, 1, 1), P(-1, 1, 1), Y);
    quad(P(-1, -1, -1), P(-1, 1, -1), P(1, 1, -1), P(1, -1, -1), -Y);
    quad(P(1, -1, -1), P(1, 1, -1), P(1, 1, 1), P(1, -1, 1), U);
    quad(P(-1, -1, -1), P(-1, -1, 1), P(-1, 1, 1), P(-1, 1, -1), -U);
    quad(P(-1, 1, -1), P(-1, 1, 1), P(1, 1, 1), P(1, 1, -1), Vv);
    quad(P(-1, -1, -1), P(1, -1, -1), P(1, -1, 1), P(-1, -1, 1), -Vv);
}
static Color brandColor(const std::string& nm, bool fuel) {
    std::string n;
    for (char c : nm) n += (char)tolower((unsigned char)c);
    auto has = [&](const char* w) { return n.find(w) != std::string::npos; };
    if (fuel) {
        if (has("omv")) return {0, 80, 170, 255};
        if (has("shell")) return {250, 200, 0, 255};
        if (has("bp")) return {0, 150, 70, 255};
        if (has("jet") || has("eni") || has("agip")) return {250, 210, 20, 255};
        if (has("avia")) return {20, 70, 160, 255};
        if (has("turm")) return {210, 30, 40, 255};
        return {210, 40, 40, 255};
    }
    if (has("billa")) return {250, 210, 0, 255};
    if (has("spar")) return {0, 140, 60, 255};
    if (has("hofer") || has("aldi")) return {0, 70, 160, 255};
    if (has("lidl")) return {0, 80, 170, 255};
    if (has("penny")) return {200, 30, 40, 255};
    if (has("merkur")) return {0, 130, 60, 255};
    if (has("mpreis")) return {200, 30, 40, 255};
    return {60, 110, 160, 255};
}

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

// ---------- Dachfarben aus dem Luftbild (Esri-Kacheln Zoom 16, werden mit den uebrigen Bildern zwischengespeichert) ----------
typedef std::unordered_map<uint32_t, std::vector<uint8_t>> RoofTiles;
static void loadRoofTiles(int cx, int cy, RoofTiles& out) {
    std::mutex m;
    std::vector<std::thread> th;
    for (int j = 0; j < 4; j++)
        for (int i = 0; i < 4; i++) {
            int tx = cx * 4 + i, ty = cy * 4 + j;
            th.emplace_back([&out, &m, tx, ty] {
                char url[256], cache[160];
                snprintf(url, sizeof url, "https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/16/%d/%d", ty, tx);
                snprintf(cache, sizeof cache, "cache/img/16/%d_%d.jpg", tx, ty);
                std::vector<uint8_t> data;
                if (!cachedGet(url, cache, data)) return;
                int w, h, c;
                uint8_t* px = stbi_load_from_memory(data.data(), (int)data.size(), &w, &h, &c, 3);
                if (!px) return;
                if (w == 256 && h == 256) {
                    std::lock_guard<std::mutex> lk(m);
                    out[((uint32_t)tx << 16) | (uint32_t)ty].assign(px, px + 256 * 256 * 3);
                }
                stbi_image_free(px);
            });
        }
    for (auto& t : th) t.join();
}
static bool pointInPoly(const std::vector<float>& p, float x, float z) {
    bool in = false;
    size_t n = p.size() / 2;
    for (size_t i = 0, k = n - 1; i < n; k = i++) {
        float xi = p[i * 2], zi = p[i * 2 + 1], xk = p[k * 2], zk = p[k * 2 + 1];
        if (((zi > z) != (zk > z)) && (x < (xk - xi) * (z - zi) / (zk - zi) + xi)) in = !in;
    }
    return in;
}
// Medianfarbe der Dachflaeche; false bei Vegetation / zu wenigen Stichproben
static bool sampleRoof(const RoofTiles& tiles, const GeoRef& g, const std::vector<float>& poly, float cx, float cz, Color& out) {
    std::vector<float> R, G, B;
    auto grab = [&](float x, float z) {
        if (!pointInPoly(poly, x, z)) return;
        double fx = (g.mx0 + x / g.S) * 65536.0, fy = (g.my0 + z / g.S) * 65536.0;
        int tx = (int)std::floor(fx), ty = (int)std::floor(fy);
        auto it = tiles.find(((uint32_t)tx << 16) | (uint32_t)ty);
        if (it == tiles.end()) return;
        int px = clampv((int)((fx - tx) * 256), 0, 255), py = clampv((int)((fy - ty) * 256), 0, 255);
        const uint8_t* q = &it->second[((size_t)py * 256 + px) * 3];
        R.push_back(q[0]); G.push_back(q[1]); B.push_back(q[2]);
    };
    grab(cx, cz);
    size_t n = poly.size() / 2;
    for (size_t i = 0; i < n; i += std::max<size_t>(1, n / 8)) {
        grab(cx + (poly[i * 2] - cx) * 0.45f, cz + (poly[i * 2 + 1] - cz) * 0.45f);
        grab(cx + (poly[i * 2] - cx) * 0.2f, cz + (poly[i * 2 + 1] - cz) * 0.2f);
    }
    if (R.size() < 3) return false;
    auto med = [](std::vector<float>& v) { std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end()); return v[v.size() / 2]; };
    float r = med(R), gg = med(G), b = med(B);
    if (gg > r * 1.12f && gg > b * 1.05f) return false; // Baeume / Gras
    out = Color{(uint8_t)clampv(r * 1.06f, 0.0f, 255.0f), (uint8_t)clampv(gg * 1.06f, 0.0f, 255.0f), (uint8_t)clampv(b * 1.06f, 0.0f, 255.0f), 255};
    return true;
}

// ---------- Gebaeude: Masse, Farben und Dachformen aus den OSM-Tags ----------
static bool parseColour(std::string s, Color& out) {
    if (s.empty()) return false;
    for (char& c : s) c = (char)tolower((unsigned char)c);
    if (s[0] == '#' && s.size() >= 7) {
        unsigned v = (unsigned)std::strtoul(s.c_str() + 1, nullptr, 16);
        if (s.size() == 4) { unsigned r = (v >> 8) & 15, g = (v >> 4) & 15, b = v & 15; v = ((r * 17) << 16) | ((g * 17) << 8) | (b * 17); }
        out = Color{(uint8_t)((v >> 16) & 255), (uint8_t)((v >> 8) & 255), (uint8_t)(v & 255), 255};
        return true;
    }
    static const struct { const char* n; Color c; } T[] = {
        {"white", {236, 233, 226, 255}}, {"black", {52, 52, 54, 255}}, {"grey", {150, 150, 152, 255}}, {"gray", {150, 150, 152, 255}},
        {"lightgrey", {190, 190, 192, 255}}, {"lightgray", {190, 190, 192, 255}}, {"darkgrey", {96, 96, 100, 255}}, {"darkgray", {96, 96, 100, 255}},
        {"silver", {192, 194, 198, 255}}, {"red", {170, 62, 52, 255}}, {"darkred", {128, 46, 40, 255}}, {"brown", {122, 86, 64, 255}},
        {"beige", {214, 198, 164, 255}}, {"yellow", {226, 200, 112, 255}}, {"orange", {214, 138, 70, 255}}, {"green", {110, 140, 96, 255}},
        {"blue", {100, 130, 170, 255}}, {"pink", {214, 160, 160, 255}}, {"cream", {232, 222, 190, 255}}, {"tan", {200, 170, 130, 255}},
        {"maroon", {120, 50, 50, 255}}, {"salmon", {224, 150, 130, 255}}, {"ochre", {204, 160, 70, 255}}, {"terracotta", {176, 92, 62, 255}}};
    for (auto& t : T) if (s == t.n) { out = t.c; return true; }
    return false;
}
static bool materialColour(std::string s, Color& out, bool& glass) {
    for (char& c : s) c = (char)tolower((unsigned char)c);
    glass = false;
    static const struct { const char* n; Color c; } T[] = {
        {"brick", {166, 92, 74, 255}}, {"concrete", {176, 174, 168, 255}}, {"stone", {186, 176, 160, 255}}, {"sandstone", {206, 184, 140, 255}},
        {"wood", {150, 112, 76, 255}}, {"timber_framing", {222, 212, 190, 255}}, {"plaster", {226, 216, 196, 255}}, {"render", {226, 216, 196, 255}},
        {"stucco", {226, 216, 196, 255}}, {"metal", {150, 154, 160, 255}}, {"steel", {150, 154, 160, 255}}, {"cement_block", {170, 168, 162, 255}},
        {"roof_tiles", {156, 78, 56, 255}}, {"tiles", {156, 78, 56, 255}}, {"clay", {156, 78, 56, 255}}, {"slate", {74, 78, 86, 255}},
        {"copper", {112, 152, 132, 255}}, {"tar_paper", {80, 80, 82, 255}}, {"gravel", {124, 120, 114, 255}}, {"eternit", {122, 124, 122, 255}},
        {"thatch", {150, 130, 86, 255}}, {"grass", {92, 130, 72, 255}}, {"glass", {150, 184, 204, 255}}, {"asphalt", {70, 70, 72, 255}}};
    for (auto& t : T) if (s == t.n) { out = t.c; glass = s == "glass"; return true; }
    return false;
}

struct HalfPlane { float nu, nv, k; }; // innen: nu*u + nv*v >= k
struct P2 { float x, z; };

static std::vector<P2> clipPoly(const std::vector<P2>& in, float nu, float nv, float k, const float ux, const float uz, const float cu, const float cv) {
    // in: Weltkoordinaten; geclippt wird in (u,v) relativ zur Mitte der Huellbox
    std::vector<P2> out;
    int n = (int)in.size();
    auto f = [&](const P2& p) {
        float u = p.x * ux + p.z * uz - cu, v = -p.x * uz + p.z * ux - cv;
        return nu * u + nv * v - k;
    };
    for (int i = 0; i < n; i++) {
        const P2& a = in[i];
        const P2& b = in[(i + 1) % n];
        float fa = f(a), fb = f(b);
        if (fa >= 0) out.push_back(a);
        if ((fa >= 0) != (fb >= 0)) { float t = fa / (fa - fb); out.push_back({a.x + (b.x - a.x) * t, a.z + (b.z - a.z) * t}); }
    }
    return out;
}

static void buildingMesh(std::vector<Vertex>& out, const std::vector<float>& poly, const json& e, int64_t id, float lo, float hi, float area, const Color* sampled) {
    int n = (int)poly.size() / 2;
    float rnd = hash2((int)(id & 0xFFFFFF), 17), rnd2 = hash2((int)(id & 0xFFFFFF), 91), rnd3 = hash2((int)(id & 0xFFFFFF), 53);
    std::string type = tagOf(e, "building"), shapeTag = tagOf(e, "roof:shape");
    float htTag = std::strtof(tagOf(e, "height").c_str(), nullptr);
    float levels = std::strtof(tagOf(e, "building:levels").c_str(), nullptr);
    float roofLv = std::strtof(tagOf(e, "roof:levels").c_str(), nullptr);
    float roofHTag = std::strtof(tagOf(e, "roof:height").c_str(), nullptr);
    bool house = type == "house" || type == "detached" || type == "semidetached_house" || type == "terrace" || type == "bungalow" || type == "villa" ||
                 type == "farm" || type == "cabin" || type == "residential";
    bool shed = type == "garage" || type == "garages" || type == "shed" || type == "carport" || type == "hut" || type == "roof" || type == "service" || type == "kiosk";
    bool industrial = type == "industrial" || type == "warehouse" || type == "factory" || type == "hangar";
    bool commercial = type == "commercial" || type == "retail" || type == "office" || type == "supermarket" || type == "hotel";
    bool church = type == "church" || type == "cathedral" || type == "chapel" || type == "mosque" || type == "temple";

    // ----- Huellbox (kleinste orientierte Box) -----
    float ux = 1, uz = 0, hu = 0, hv = 0, cu = 0, cv = 0;
    float bestA = 1e30f;
    for (int i = 0; i < n && n <= 64; i++) {
        int k = (i + 1) % n;
        float dx = poly[k * 2] - poly[i * 2], dz = poly[k * 2 + 1] - poly[i * 2 + 1], l = std::hypot(dx, dz);
        if (l < 0.3f) continue;
        float tx = dx / l, tz = dz / l, u0 = 1e9f, u1 = -1e9f, v0 = 1e9f, v1 = -1e9f;
        for (int q = 0; q < n; q++) {
            float u = poly[q * 2] * tx + poly[q * 2 + 1] * tz, v = -poly[q * 2] * tz + poly[q * 2 + 1] * tx;
            u0 = std::min(u0, u); u1 = std::max(u1, u); v0 = std::min(v0, v); v1 = std::max(v1, v);
        }
        float a = (u1 - u0) * (v1 - v0);
        if (a < bestA) {
            bestA = a;
            if (u1 - u0 >= v1 - v0) { ux = tx; uz = tz; hu = (u1 - u0) * 0.5f; hv = (v1 - v0) * 0.5f; cu = (u0 + u1) * 0.5f; cv = (v0 + v1) * 0.5f; }
            else { ux = -tz; uz = tx; hu = (v1 - v0) * 0.5f; hv = (u1 - u0) * 0.5f; cu = (v0 + v1) * 0.5f; cv = -(u0 + u1) * 0.5f; }
        }
    }
    // Achse (ux,uz) = lange Seite; v = (-uz, ux). Fuer den zweiten Fall: neue u-Achse = alte v-Achse.
    // cv/cu sind dann im neuen System: u' = alte v, v' = -alte u
    float rectness = bestA > 1 ? std::fabs(area) / bestA : 0;

    // ----- Dachform -----
    enum { FLAT, GABLED, HIPPED, PYRAMID, SKILLION, DOME } shape = FLAT;
    {
        std::string s = shapeTag;
        if (s == "gabled" || s == "gambrel" || s == "saltbox" || s == "round" || s == "half-hipped" || s == "side_hipped_half") shape = GABLED;
        else if (s == "hipped" || s == "mansard" || s == "half_hipped") shape = HIPPED;
        else if (s == "pyramidal" || s == "cone") shape = PYRAMID;
        else if (s == "skillion" || s == "lean_to") shape = SKILLION;
        else if (s == "dome" || s == "onion") shape = DOME;
        else if (s == "flat") shape = FLAT;
        else if (!shed && !industrial && !commercial && !church) {
            bool small = std::fabs(area) < 320 && (levels > 0 ? levels <= 4 : (htTag > 0 ? htTag <= 14 : true));
            bool houseish = house || (type == "yes" || type.empty()) || type == "apartments" || type == "dormitory";
            if (houseish && small && rectness > 0.7f) shape = rnd < 0.58f ? GABLED : (rnd < 0.9f ? HIPPED : FLAT);
            else if (houseish && std::fabs(area) < 800 && (levels > 0 ? levels <= 5 : true) && rectness > 0.85f && hv > 4.0f && hv < 11.0f && rnd < 0.4f) shape = rnd2 < 0.5f ? HIPPED : GABLED;
        } else if (shed && std::fabs(area) < 60) shape = rnd < 0.5f ? SKILLION : FLAT;
        else if (industrial && rectness > 0.85f && rnd < 0.4f) shape = GABLED;
        else if (church && rectness > 0.75f) shape = GABLED;
    }
    if ((shape == GABLED || shape == HIPPED) && (rectness < 0.62f || hv < 1.2f || n > 64)) shape = (shape == HIPPED && n <= 64 && hv > 0.8f) ? PYRAMID : FLAT;
    if ((shape == SKILLION) && (hv < 0.8f || n > 64)) shape = FLAT;
    if (shape == PYRAMID || shape == DOME) { // Spitzdach nur fuer halbwegs konvexe, kompakte Grundrisse
        bool convex = true;
        for (int i = 0; i < n && convex; i++) {
            int j = (i + 1) % n, k = (i + 2) % n;
            float c = (poly[j * 2] - poly[i * 2]) * (poly[k * 2 + 1] - poly[j * 2 + 1]) - (poly[j * 2 + 1] - poly[i * 2 + 1]) * (poly[k * 2] - poly[j * 2]);
            if (c < -0.05f) convex = false;
        }
        if (!convex || n > 40) shape = FLAT;
    }

    // ----- Hoehen -----
    float eave;
    float rh = 0;
    switch (shape) {
    case GABLED: rh = clampv(hv * (0.50f + 0.14f * rnd2), 1.0f, 5.5f); break;
    case HIPPED: rh = clampv(hv * (0.40f + 0.12f * rnd2), 0.8f, 4.5f); break;
    case PYRAMID: rh = clampv(std::min(hu, hv) * 0.65f, 1.0f, 8.0f); break;
    case DOME: rh = clampv(std::min(hu, hv) * 0.8f, 1.5f, 12.0f); break;
    case SKILLION: rh = clampv(hv * 0.35f, 0.7f, 3.0f); break;
    default: break;
    }
    if (roofHTag > 0 && shape != FLAT) rh = clampv(roofHTag, 0.5f, 25.0f);
    else if (roofLv > 0 && shape != FLAT) rh = clampv(roofLv * 2.2f, 0.8f, 12.0f);
    if (htTag > 0) eave = shape == FLAT ? htTag : std::max(htTag - rh, std::min(htTag, 2.5f));
    else if (levels > 0) eave = levels * 3.1f + 0.8f;
    else if (shed) eave = 2.6f + rnd * 0.8f;
    else if (house) eave = 5.6f + rnd * 1.4f;
    else if (church) eave = 11.0f + rnd * 4.0f;
    else if (industrial) eave = 7.0f + rnd * 3.5f;
    else if (commercial) eave = 9.0f + rnd * 6.0f;
    else if (std::fabs(area) < 40) eave = 3.0f + rnd * 1.2f;
    else eave = (std::fabs(area) > 600 ? 9.0f : 6.0f) + rnd * 6.0f;
    eave = clampv(eave, 2.2f, 400.0f);
    if (eave + rh > 30 && shape != FLAT && htTag <= 0) rh = std::min(rh, 3.0f);

    // ----- Farben -----
    // Putzfarben wie sie in Mitteleuropa haeufig sind (Creme, Ocker, Gelb, Lachs, Weiss, Hellgrau, Graublau, Hellgruen)
    static const Color pal[12] = {{236, 226, 200, 255}, {228, 204, 152, 255}, {232, 214, 150, 255}, {224, 178, 150, 255}, {242, 240, 234, 255}, {204, 204, 202, 255},
                                  {188, 198, 208, 255}, {204, 218, 192, 255}, {216, 170, 140, 255}, {222, 196, 176, 255}, {196, 186, 172, 255}, {214, 204, 186, 255}};
    static const Color pitched[5] = {{156, 78, 56, 255}, {132, 72, 54, 255}, {120, 62, 48, 255}, {84, 88, 96, 255}, {104, 70, 58, 255}};
    Color wall = pal[(int)(rnd * 11.99f)], roof;
    bool glass = false, gl2 = false;
    if (house) wall = pal[(int)(rnd3 * 11.99f)];
    if (sampled && !house) { // warmes Dach (Ziegel) -> eher warme Putzfarbe, graues Flachdach -> eher neutral
        float warm = (sampled->r - sampled->b) / 255.0f;
        if (warm < 0.12f && rnd2 < 0.6f) wall = pal[4 + (int)(rnd3 * 3.99f)];
    }
    if (church) wall = Color{206, 198, 184, 255};
    Color tmp;
    if (parseColour(tagOf(e, "building:colour"), tmp) || parseColour(tagOf(e, "colour"), tmp) || parseColour(tagOf(e, "wall:colour"), tmp) || parseColour(tagOf(e, "building:facade:colour"), tmp)) wall = tmp;
    else if (materialColour(tagOf(e, "building:material"), tmp, gl2) || materialColour(tagOf(e, "material"), tmp, gl2)) { wall = tmp; glass = gl2; }
    else if (industrial) wall = Color{176, 178, 176, 255};
    if (shape == FLAT) { uint8_t g = (uint8_t)(100 + rnd2 * 42); roof = Color{g, (uint8_t)(g - 4), (uint8_t)(g - 8), 255}; }
    else roof = pitched[(int)(rnd2 * 4.99f)];
    if (parseColour(tagOf(e, "roof:colour"), tmp)) roof = tmp;
    else if (materialColour(tagOf(e, "roof:material"), tmp, gl2)) roof = tmp;
    else if (sampled) roof = *sampled;
    float jit = 0.93f + 0.14f * rnd3;
    auto shade = [&](Color c, float f) { return Color{(uint8_t)std::min(255.0f, c.r * f), (uint8_t)std::min(255.0f, c.g * f), (uint8_t)std::min(255.0f, c.b * f), c.a}; };
    wall = shade(wall, jit);
    roof = shade(roof, 0.95f + 0.1f * rnd);
    if (!glass && (commercial || (eave > 18 && type != "residential" && type != "apartments")) && htTag <= 0 && rnd3 < 0.45f) glass = true; // Buerotuerme: Glasfassade
    wall.a = glass ? 170 : 255;
    roof.a = 0; // alpha 0 = Dach (keine Fenster)

    float base = lo - 1.0f, top = hi + eave;

    // ----- Dachflaechen: Regionen (Halbebenen in u/v) und Hoehenfunktion -----
    std::vector<std::vector<HalfPlane>> regions;
    std::vector<HalfPlane> lines;
    float k2 = hu - hv, slope = hv > 0.01f ? rh / hv : 0;
    auto toUV = [&](float x, float z, float& u, float& v) { u = x * ux + z * uz - cu; v = -x * uz + z * ux - cv; };
    auto height = [&](float x, float z) -> float {
        float u, v;
        toUV(x, z, u, v);
        switch (shape) {
        case GABLED: return rh * std::max(0.0f, 1.0f - std::fabs(v) / hv);
        case HIPPED: return slope * std::max(0.0f, std::min(hv - std::fabs(v), hu - std::fabs(u)));
        case SKILLION: return rh * clampv((v + hv) / (2 * hv), 0.0f, 1.0f);
        default: return 0.0f;
        }
    };
    if (shape == GABLED) { regions = {{{0, 1, 0}}, {{0, -1, 0}}}; lines = {{0, 1, 0}}; }
    else if (shape == HIPPED) {
        regions = {{{0, 1, 0}, {-1, 1, -k2}, {1, 1, -k2}}, {{0, -1, 0}, {-1, -1, -k2}, {1, -1, -k2}}, {{1, -1, k2}, {1, 1, k2}}, {{-1, -1, k2}, {-1, 1, k2}}};
        lines = {{0, 1, 0}, {-1, 1, -k2}, {1, 1, -k2}, {-1, -1, -k2}, {1, -1, -k2}, {1, -1, k2}, {1, 1, k2}, {-1, -1, k2}, {-1, 1, k2}};
    } else if (shape == SKILLION) regions = {{}};

    auto emit = [&](Vec3 a, Vec3 b, Vec3 c, Color col) {
        Vec3 nn = normalize(cross(b - a, c - a));
        if (nn.y < 0) nn = -nn;
        out.push_back({{a.x, a.y, a.z}, {nn.x, nn.y, nn.z}, {0, 0}, col});
        out.push_back({{b.x, b.y, b.z}, {nn.x, nn.y, nn.z}, {0, 0}, col});
        out.push_back({{c.x, c.y, c.z}, {nn.x, nn.y, nn.z}, {0, 0}, col});
    };
    // Waende: Oberkante folgt der Dachflaeche (ergibt Giebel), an Knicklinien wird unterteilt
    float u = 0;
    for (int i = 0; i < n; i++) {
        int k = (i + 1) % n;
        float ax = poly[i * 2], az = poly[i * 2 + 1], bx = poly[k * 2], bz = poly[k * 2 + 1];
        float len = std::hypot(bx - ax, bz - az);
        if (len < 0.01f) continue;
        Vec3 nn{(bz - az) / len, 0, -(bx - ax) / len};
        std::vector<float> ts{0.0f, 1.0f};
        float ua, va, ub, vb;
        toUV(ax, az, ua, va); toUV(bx, bz, ub, vb);
        for (const HalfPlane& h : lines) {
            float fa = h.nu * ua + h.nv * va - h.k, fb = h.nu * ub + h.nv * vb - h.k;
            if ((fa > 0) != (fb > 0) && std::fabs(fa - fb) > 1e-6f) { float t = fa / (fa - fb); if (t > 0.001f && t < 0.999f) ts.push_back(t); }
        }
        std::sort(ts.begin(), ts.end());
        for (size_t q = 0; q + 1 < ts.size(); q++) {
            float t0 = ts[q], t1 = ts[q + 1];
            float x0 = ax + (bx - ax) * t0, z0 = az + (bz - az) * t0, x1 = ax + (bx - ax) * t1, z1 = az + (bz - az) * t1;
            float y0 = top + height(x0, z0), y1 = top + height(x1, z1);
            float u0 = u + len * t0, u1 = u + len * t1;
            auto V = [&](float x, float y, float z, float uu) { out.push_back({{x, y, z}, {nn.x, nn.y, nn.z}, {uu, y - (hi + 0.3f)}, wall}); };
            V(x0, base, z0, u0); V(x1, base, z1, u1); V(x1, y1, z1, u1);
            V(x0, base, z0, u0); V(x1, y1, z1, u1); V(x0, y0, z0, u0);
        }
        u += len;
    }
    // Dach
    std::vector<P2> footprint(n);
    for (int i = 0; i < n; i++) footprint[i] = {poly[i * 2], poly[i * 2 + 1]};
    auto roofPoly = [&](const std::vector<P2>& pts) {
        if (pts.size() < 3) return;
        std::vector<float> flat;
        for (const P2& p : pts) { flat.push_back(p.x); flat.push_back(p.z); }
        std::vector<int> tri;
        triangulate(flat, tri);
        for (size_t t = 0; t + 2 < tri.size(); t += 3) {
            Vec3 v[3];
            for (int q = 0; q < 3; q++) { float x = flat[tri[t + q] * 2], z = flat[tri[t + q] * 2 + 1]; v[q] = {x, top + height(x, z), z}; }
            emit(v[0], v[1], v[2], roof);
        }
    };
    if (shape == PYRAMID || shape == DOME) {
        float mx = 0, mz = 0;
        for (int i = 0; i < n; i++) { mx += poly[i * 2]; mz += poly[i * 2 + 1]; }
        mx /= n; mz /= n;
        const int rings = shape == DOME ? 5 : 1;
        float sc[6], hh[6];
        for (int r = 0; r <= rings; r++) {
            float a = (shape == DOME ? 1.5708f : 1.0f) * r / rings;
            sc[r] = shape == DOME ? std::cos(a) : 1.0f - (float)r / rings;
            hh[r] = shape == DOME ? rh * std::sin(a) : rh * (float)r / rings;
        }
        for (int i = 0; i < n; i++) {
            int k = (i + 1) % n;
            for (int r = 0; r < rings; r++) {
                auto P = [&](int idx, int ring) { return Vec3{mx + (poly[idx * 2] - mx) * sc[ring], top + hh[ring], mz + (poly[idx * 2 + 1] - mz) * sc[ring]}; };
                Vec3 a = P(i, r), b = P(k, r), c = P(k, r + 1), d = P(i, r + 1);
                emit(a, b, c, roof);
                if (r + 1 < rings || shape == DOME) { if (length(c - d) > 1e-4f) emit(a, c, d, roof); }
            }
        }
    } else if (regions.empty()) roofPoly(footprint);
    else {
        for (const auto& reg : regions) {
            std::vector<P2> pts = footprint;
            for (const HalfPlane& h : reg) { pts = clipPoly(pts, h.nu, h.nv, h.k, ux, uz, cu, cv); if (pts.size() < 3) break; }
            roofPoly(pts);
        }
    }
}

// ---------- Kartenfragen: vom Spieler bestaetigte Korrekturen (dauerhaft in maps/map_overrides.json) ----------
struct Overrides {
    std::mutex m;
    std::unordered_map<int64_t, int> lanes;       // OSM-Weg-ID -> Spurzahl
    std::unordered_map<std::string, bool> resolved; // Schluessel "idA_idB" -> beantwortet
    std::vector<MapConflict> found;               // neu erkannte, noch nicht abgeholte Unklarheiten
    std::unordered_map<std::string, bool> seen;
    bool loaded = false;
} OV;
static const char* OV_FILE = "maps/map_overrides.json";
static void loadOverrides() {
    std::lock_guard<std::mutex> lk(OV.m);
    if (OV.loaded) return;
    OV.loaded = true;
    std::vector<uint8_t> data;
    if (!readFile(OV_FILE, data)) return;
    json j = json::parse(data.begin(), data.end(), nullptr, false);
    if (!j.is_object()) return;
    if (j.contains("lanes") && j["lanes"].is_object())
        for (auto it = j["lanes"].begin(); it != j["lanes"].end(); ++it) OV.lanes[std::strtoll(it.key().c_str(), nullptr, 10)] = it.value().get<int>();
    if (j.contains("resolved") && j["resolved"].is_array())
        for (const json& k : j["resolved"]) if (k.is_string()) OV.resolved[k.get<std::string>()] = true;
}
static void saveOverrides() { // Aufrufer haelt OV.m
    json j;
    j["lanes"] = json::object();
    for (auto& kv : OV.lanes) j["lanes"][std::to_string(kv.first)] = kv.second;
    j["resolved"] = json::array();
    for (auto& kv : OV.resolved) j["resolved"].push_back(kv.first);
    std::string s = j.dump(1);
    writeFile(OV_FILE, std::vector<uint8_t>(s.begin(), s.end()));
}
static bool laneOverride(int64_t id, int& lanes) {
    std::lock_guard<std::mutex> lk(OV.m);
    auto it = OV.lanes.find(id);
    if (it == OV.lanes.end()) return false;
    lanes = it->second;
    return true;
}

struct WayTmp {
    bool ring = false, rbt = false; // Kreisverkehr (junction=roundabout, auch mehrteilig); rbt = Tag gesetzt
    int ringGroup = -1;
    float rcx = 0, rcz = 0, rR = 0;
    uint8_t ringOuter = 1;        // 1: Aussenseite liegt rechts der Laufrichtung, 2: links
    int64_t id = 0;
    bool lanesExplicit = false;
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
        RoofTiles roofTiles;
        if (d->googleSession.empty()) loadRoofTiles(cx, cy, roofTiles);
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
                w.id = e.value("id", (int64_t)0);
                w.px = std::move(px); w.pz = std::move(pz);
                if (e.contains("nodes")) for (const json& id : e["nodes"]) w.nodes.push_back(id.get<int64_t>());
                std::string ow = tagOf(e, "oneway");
                w.oneway = (ow == "yes" || ow == "1" || ow == "true") ? 1 : (ow == "-1" ? -1 : 0);
                if (ow.empty() && (klass == 0 || tagOf(e, "junction") == "roundabout")) w.oneway = 1;
                int lanesTag = atoi(tagOf(e, "lanes").c_str());
                { int ov; if (laneOverride(w.id, ov)) lanesTag = ov; }
                w.lanesExplicit = lanesTag > 0;
                w.lanes = lanesTag > 0 ? std::min(lanesTag, 8) : (klass >= 6 ? 1 : (w.oneway && klass >= 2 ? 1 : 2));
                static const float laneW[8] = {3.75f, 3.5f, 3.5f, 3.25f, 3.25f, 2.9f, 3.4f, 2.8f};
                float width = w.lanes * laneW[klass] + (klass <= 1 ? 1.5f : 0.4f);
                float wt = std::strtof(tagOf(e, "width").c_str(), nullptr);
                if (wt >= 2.5f && wt <= 30) width = wt;
                w.hw = std::min(width * 0.5f, 25.0f);
                w.flags = ((klass <= 4 || lanesTag >= 2) && w.lanes >= 2 ? 1 : 0) | (klass <= 3 ? 2 : 0) | (w.oneway ? 4 : 0);
                float def = klass == 0 ? 130 : klass == 1 ? 100 : (klass >= 6 ? 30 : (hw == "living_street" ? 20 : 50));
                w.maxspeed = parseSpeed(tagOf(e, "maxspeed"), def);
                {   // Gehsteig: aus dem Tag, sonst in Ortsstrassen (Tempo <= 50) angenommen
                    std::string sw = tagOf(e, "sidewalk");
                    if (sw.empty()) sw = tagOf(e, "sidewalk:both");
                    if (sw.empty()) sw = (tagOf(e, "sidewalk:left").empty() && tagOf(e, "sidewalk:right").empty()) ? "" : "yes";
                    bool has = sw == "both" || sw == "left" || sw == "right" || sw == "yes" || sw == "separate";
                    if (sw.empty() && klass >= 2 && klass <= 5 && w.maxspeed <= 50) has = true;
                    if (has) w.flags |= 8;
                }
                std::string name = tagOf(e, "name");
                if (name.empty()) name = tagOf(e, "ref");
                auto it = nameIdx.find(name);
                if (it == nameIdx.end()) { w.name = (int)r.names.size(); nameIdx[name] = w.name; r.names.push_back(name); }
                else w.name = it->second;
                if (klass <= 6) for (int64_t id : w.nodes) junction[id]++;
w.rbt = klass <= 6 && tagOf(e, "junction") == "roundabout";
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
                float lo = 1e9f, hi = -1e9f;
                Building b;
                b.minx = b.minz = 1e9f; b.maxx = b.maxz = -1e9f;
                for (int i = 0; i < n; i++) {
                    float h = H(poly[i * 2], poly[i * 2 + 1]);
                    lo = std::min(lo, h); hi = std::max(hi, h);
                    b.minx = std::min(b.minx, poly[i * 2]); b.maxx = std::max(b.maxx, poly[i * 2]);
                    b.minz = std::min(b.minz, poly[i * 2 + 1]); b.maxz = std::max(b.maxz, poly[i * 2 + 1]);
                }
                Color roofSample;
                bool haveSample = !roofTiles.empty() && sampleRoof(roofTiles, g, poly, cxm, czm, roofSample);
                buildingMesh(r.bldV, poly, e, id, lo, hi, area, haveSample ? &roofSample : nullptr);
                b.poly = std::move(poly);
                r.buildings.push_back(std::move(b));
            }
        }
        // ----- Kreisverkehre erkennen: Wege mit junction=roundabout, die zusammenhaengen und einen Kreis bilden -----
        struct RingGroup { float cx = 0, cz = 0, R = 0, hw = 0; bool done = false; };
        std::vector<RingGroup> rgroups;
        {
            int nw = (int)ways.size();
            std::vector<int> par(nw);
            for (int i = 0; i < nw; i++) par[i] = i;
            std::function<int(int)> fnd = [&](int a) { return par[a] == a ? a : par[a] = fnd(par[a]); };
            std::unordered_map<int64_t, int> firstWay;
            for (int wi = 0; wi < nw; wi++) {
                if (!ways[wi].rbt || ways[wi].nodes.size() != ways[wi].px.size()) continue;
                for (int64_t id : ways[wi].nodes) {
                    auto it = firstWay.find(id);
                    if (it == firstWay.end()) firstWay[id] = wi; else par[fnd(wi)] = fnd(it->second);
                }
            }
            std::unordered_map<int, std::vector<int>> members;
            for (int wi = 0; wi < nw; wi++) if (ways[wi].rbt && ways[wi].nodes.size() == ways[wi].px.size()) members[fnd(wi)].push_back(wi);
            for (auto& kv : members) {
                std::unordered_map<int64_t, std::pair<float, float>> pts;
                float hwMax = 0;
                for (int wi : kv.second) { for (size_t i = 0; i < ways[wi].nodes.size(); i++) pts[ways[wi].nodes[i]] = {ways[wi].px[i], ways[wi].pz[i]}; hwMax = std::max(hwMax, ways[wi].hw); }
                if (pts.size() < 8) continue;
                float cx = 0, cz = 0;
                for (auto& p : pts) { cx += p.second.first; cz += p.second.second; }
                cx /= pts.size(); cz /= pts.size();
                float mr = 0, mn = 1e9f, mxr = 0;
                std::vector<float> ang;
                for (auto& p : pts) {
                    float d = std::hypot(p.second.first - cx, p.second.second - cz);
                    mr += d; mn = std::min(mn, d); mxr = std::max(mxr, d);
                    ang.push_back(std::atan2(p.second.second - cz, p.second.first - cx));
                }
                mr /= pts.size();
                std::sort(ang.begin(), ang.end());
                float gap = 2 * PI - (ang.back() - ang.front());
                for (size_t i = 0; i + 1 < ang.size(); i++) gap = std::max(gap, ang[i + 1] - ang[i]);
                if (mr < 4.0f || mr > 90.0f || mxr - mn > mr * 0.45f || gap > 1.7f) continue;
                int gid = (int)rgroups.size();
                RingGroup rg; rg.cx = cx; rg.cz = cz; rg.R = mr; rg.hw = hwMax;
                rgroups.push_back(rg);
                for (int wi : kv.second) {
                    WayTmp& w = ways[wi];
                    w.ring = true; w.ringGroup = gid; w.rcx = cx; w.rcz = cz; w.rR = mr;
                    float dx = w.px[1] - w.px[0], dz = w.pz[1] - w.pz[0];
                    float rxv = -dz, rzv = dx, mx = (w.px[0] + w.px[1]) * 0.5f - cx, mz = (w.pz[0] + w.pz[1]) * 0.5f - cz;
                    w.ringOuter = (rxv * mx + rzv * mz) > 0 ? 1 : 2;
                }
            }
        }
        { // Unklare Spurwechsel auf einer Strasse melden (der Spieler wird einmal gefragt, die Antwort bleibt gespeichert)
            std::unordered_map<int64_t, std::vector<int>> ends;
            for (int wi = 0; wi < (int)ways.size(); wi++) {
                const WayTmp& w = ways[wi];
                if (w.nodes.size() < 2 || w.nodes.size() != w.px.size() || w.klass > 5 || !w.lanesExplicit) continue;
                ends[w.nodes.front()].push_back(wi);
                ends[w.nodes.back()].push_back(wi);
            }
            for (auto& kv : ends) {
                if (kv.second.size() != 2 || junction[kv.first] != 2) continue;
                const WayTmp& A = ways[kv.second[0]];
                const WayTmp& B = ways[kv.second[1]];
                if (A.id == B.id || A.klass != B.klass || A.oneway != B.oneway || A.lanes == B.lanes || A.name != B.name || r.names[A.name].empty()) continue;
                float x = A.nodes.front() == kv.first ? A.px.front() : A.px.back(), z = A.nodes.front() == kv.first ? A.pz.front() : A.pz.back();
                if (!inside(x, z)) continue;
                MapConflict c;
                c.idA = std::min(A.id, B.id); c.idB = std::max(A.id, B.id);
                c.key = std::to_string(c.idA) + "_" + std::to_string(c.idB);
                c.lanesA = A.id == c.idA ? A.lanes : B.lanes; c.lanesB = A.id == c.idA ? B.lanes : A.lanes;
                c.name = r.names[A.name];
                c.lon = (g.mx0 + x / g.S) * 360.0 - 180.0;
                c.lat = myToLat(g.my0 + z / g.S);
                std::lock_guard<std::mutex> lk(OV.m);
                if (OV.resolved.count(c.key) || OV.seen.count(c.key)) continue;
                OV.seen[c.key] = true;
                OV.found.push_back(c);
            }
        }
        // ----- Strassen: kleine Strassen zuerst, grosse zuletzt (liegen beim Zeichnen oben) -----
        // Erst alle Gehsteige, dann aller Asphalt: so schneidet kein Gehsteig die Einmuendung einer Seitenstrasse ab.
        std::stable_sort(ways.begin(), ways.end(), [](const WayTmp& a, const WayTmp& b) { return a.klass > b.klass; });
        std::vector<Vertex> sideV, asphV;
        for (const WayTmp& w : ways) {
            int n = (int)w.px.size();
            // ----- Linie glaetten: Ecken werden mit quadratischen Kurven gerundet, Kreuzungsknoten bleiben exakt -----
            std::vector<float> sx, sz;
            if (w.ring) { // Kreisverkehr: Spline durch alle Knoten; an den Wegenden mit den Nachbarwegen des Rings fortgesetzt
                auto neighbour = [&](int64_t node, bool wantPrev, float& qx, float& qz) {
                    for (const WayTmp& o : ways) {
                        if (&o == &w || !o.ring || o.ringGroup != w.ringGroup || o.nodes.size() != o.px.size() || o.nodes.size() < 2) continue;
                        if (o.nodes.back() == node) { qx = o.px[o.px.size() - 2]; qz = o.pz[o.pz.size() - 2]; return true; }
                        if (o.nodes.front() == node) { qx = o.px[1]; qz = o.pz[1]; return true; }
                    }
                    (void)wantPrev;
                    return false;
                };
                float p0x = 2 * w.px[0] - w.px[1], p0z = 2 * w.pz[0] - w.pz[1];
                float p3x = 2 * w.px[n - 1] - w.px[n - 2], p3z = 2 * w.pz[n - 1] - w.pz[n - 2];
                if ((int)w.nodes.size() == n) { neighbour(w.nodes.front(), true, p0x, p0z); neighbour(w.nodes.back(), false, p3x, p3z); }
                auto PX = [&](int i) { return i < 0 ? p0x : (i >= n ? p3x : w.px[i]); };
                auto PZ = [&](int i) { return i < 0 ? p0z : (i >= n ? p3z : w.pz[i]); };
                for (int i = 0; i + 1 < n; i++) {
                    float len = std::hypot(PX(i + 1) - PX(i), PZ(i + 1) - PZ(i));
                    int steps = std::max(3, (int)std::ceil(len / 2.0f));
                    for (int s = 0; s < steps; s++) {
                        float t = (float)s / steps, t2 = t * t, t3 = t2 * t;
                        auto cr = [&](float q0, float q1, float q2, float q3) {
                            return 0.5f * ((2 * q1) + (-q0 + q2) * t + (2 * q0 - 5 * q1 + 4 * q2 - q3) * t2 + (-q0 + 3 * q1 - 3 * q2 + q3) * t3);
                        };
                        if (s == 0) { sx.push_back(PX(i)); sz.push_back(PZ(i)); }
                        else { sx.push_back(cr(PX(i - 1), PX(i), PX(i + 1), PX(i + 2))); sz.push_back(cr(PZ(i - 1), PZ(i), PZ(i + 1), PZ(i + 2))); }
                    }
                }
                sx.push_back(w.px[n - 1]); sz.push_back(w.pz[n - 1]);
            } else {
            float maxCut = w.klass <= 1 ? 60.0f : (w.klass <= 3 ? 30.0f : (w.klass <= 5 ? 18.0f : 10.0f));
            sx.push_back(w.px[0]); sz.push_back(w.pz[0]);
            for (int i = 1; i + 1 < n; i++) {
                bool shared = (int)w.nodes.size() == n && junction[w.nodes[i]] >= 2;
                float ax = w.px[i - 1], az = w.pz[i - 1], bx = w.px[i], bz = w.pz[i], cx = w.px[i + 1], cz = w.pz[i + 1];
                float l0 = std::hypot(bx - ax, bz - az), l1 = std::hypot(cx - bx, cz - bz);
                if (shared || l0 < 0.5f || l1 < 0.5f) { sx.push_back(bx); sz.push_back(bz); continue; }
                float d0x = (bx - ax) / l0, d0z = (bz - az) / l0, d1x = (cx - bx) / l1, d1z = (cz - bz) / l1;
                float cosA = clampv(d0x * d1x + d0z * d1z, -1.0f, 1.0f), ang = std::acos(cosA);
                if (ang < 0.035f) { sx.push_back(bx); sz.push_back(bz); continue; }
                float cut = std::min(maxCut, 0.5f * std::min(l0, l1));
                float p1x = bx - d0x * cut, p1z = bz - d0z * cut, p2x = bx + d1x * cut, p2z = bz + d1z * cut;
                int steps = clampv((int)std::ceil(ang / 0.13f), 2, 14);
                for (int s = 0; s <= steps; s++) {
                    float t = (float)s / steps, a0 = (1 - t) * (1 - t), a1 = 2 * t * (1 - t), a2 = t * t;
                    sx.push_back(a0 * p1x + a1 * bx + a2 * p2x);
                    sz.push_back(a0 * p1z + a1 * bz + a2 * p2z);
                }
            }
            sx.push_back(w.px[n - 1]); sz.push_back(w.pz[n - 1]);
            }
            int sn = (int)sx.size();
            for (int i = 0; i + 1 < sn; i++) {
                if (!inside((sx[i] + sx[i + 1]) * 0.5f, (sz[i] + sz[i + 1]) * 0.5f)) continue;
                r.segs.push_back({sx[i], sz[i], sx[i + 1], sz[i + 1], w.hw, w.maxspeed, (int8_t)w.oneway, (uint8_t)w.klass, (uint8_t)w.lanes, (uint8_t)(w.flags & 1), w.name, (uint8_t)(w.ring ? w.ringOuter : 0)});
            }
            std::vector<float> rx, rz, rd;
            float dist = 0;
            for (int i = 0; i + 1 < sn; i++) {
                float len = std::hypot(sx[i + 1] - sx[i], sz[i + 1] - sz[i]);
                int steps = std::max(1, (int)std::ceil(len / 5.0f));
                for (int s = 0; s < steps; s++) {
                    float t = (float)s / steps;
                    rx.push_back(sx[i] + (sx[i + 1] - sx[i]) * t);
                    rz.push_back(sz[i] + (sz[i + 1] - sz[i]) * t);
                    rd.push_back(dist + len * t);
                }
                dist += len;
            }
            rx.push_back(sx[sn - 1]); rz.push_back(sz[sn - 1]); rd.push_back(dist);
            int m = (int)rx.size();
            bool hasSw = (w.flags & 8) != 0;
            float hwq = std::min(255.0f, w.hw * 10.0f) / 10.0f;
            auto ribbon = [&](float half, int flags, std::vector<Vertex>& dst) {
                Color col{(uint8_t)w.lanes, (uint8_t)flags, (uint8_t)w.klass, (uint8_t)std::min(255.0f, w.hw * 10.0f)};
                auto edge = [&](int k, float side, Vertex& o) {
                    float n0x, n0z, n1x, n1z;
                    auto nrm = [&](int a, int b, float& nx, float& nz) {
                        float tx = rx[b] - rx[a], tz = rz[b] - rz[a], l = std::hypot(tx, tz);
                        if (l < 1e-4f) { nx = 0; nz = 0; return false; }
                        nx = -tz / l; nz = tx / l;
                        return true;
                    };
                    bool h0 = k > 0 && nrm(k - 1, k, n0x, n0z), h1 = k < m - 1 && nrm(k, k + 1, n1x, n1z);
                    if (!h0 && !h1) { n0x = 0; n0z = 1; n1x = 0; n1z = 1; }
                    else if (!h0) { n0x = n1x; n0z = n1z; }
                    else if (!h1) { n1x = n0x; n1z = n0z; }
                    float mx = n0x + n1x, mz = n0z + n1z, l = std::hypot(mx, mz);
                    if (l < 1e-3f) { mx = n1x; mz = n1z; l = 1; }
                    mx /= l; mz /= l;
                    float ch = std::max(0.6f, mx * n0x + mz * n0z); // Gehrung: Breite bleibt in Kurven gleich
                    float x = rx[k] + mx * half * side / ch, z = rz[k] + mz * half * side / ch;
                    o = {{x, H(x, z) + 0.03f, z}, {0, 1, 0}, {half * side, rd[k]}, col};
                };
                for (int k = 0; k + 1 < m; k++) {
                    if (!inside((rx[k] + rx[k + 1]) * 0.5f, (rz[k] + rz[k + 1]) * 0.5f)) continue;
                    Vertex L0, R0, L1, R1;
                    edge(k, -1, L0); edge(k, 1, R0); edge(k + 1, -1, L1); edge(k + 1, 1, R1);
                    dst.push_back(L0); dst.push_back(R0); dst.push_back(R1);
                    dst.push_back(L0); dst.push_back(R1); dst.push_back(L1);
                }
            };
            if (hasSw) ribbon(hwq + 1.9f, w.flags | 16, sideV);
            ribbon(hasSw ? hwq + 0.15f : hwq, w.flags, asphV);
            if (w.ring && !rgroups[w.ringGroup].done) { // Mittelinsel (einmal je Kreisverkehr): Rasen mit Bordstein, fuer Fahrzeuge ein Hindernis
                RingGroup& rg = rgroups[w.ringGroup];
                rg.done = true;
                float Ri = std::max(1.0f, rg.R - rg.hw - 0.2f);
                Color ic{(uint8_t)w.lanes, (uint8_t)w.flags, 8, 3};
                auto IV = [&](float x, float z, float lat) { asphV.push_back({{x, H(x, z) + 0.03f, z}, {0, 1, 0}, {lat, 0}, ic}); };
                const int N = 56;
                for (int i = 0; i < N; i++) {
                    float a0 = i * 2 * PI / N, a1 = (i + 1) * 2 * PI / N;
                    IV(rg.cx, rg.cz, -Ri);
                    IV(rg.cx + std::cos(a0) * Ri, rg.cz + std::sin(a0) * Ri, 0);
                    IV(rg.cx + std::cos(a1) * Ri, rg.cz + std::sin(a1) * Ri, 0);
                }
                Building ib;
                const int M = 24;
                ib.poly.resize(M * 2);
                ib.minx = ib.minz = 1e9f; ib.maxx = ib.maxz = -1e9f;
                for (int i = 0; i < M; i++) {
                    float x = rg.cx + std::cos(i * 2 * PI / M) * (Ri - 0.4f), z = rg.cz + std::sin(i * 2 * PI / M) * (Ri - 0.4f);
                    ib.poly[i * 2] = x; ib.poly[i * 2 + 1] = z;
                    ib.minx = std::min(ib.minx, x); ib.maxx = std::max(ib.maxx, x); ib.minz = std::min(ib.minz, z); ib.maxz = std::max(ib.maxz, z);
                }
                if (inside(rg.cx, rg.cz)) r.buildings.push_back(std::move(ib));
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
        { // Tankstellen und Supermaerkte: Eintraege fuer das Spiel und einfache Modelle (Dach mit Zapfsaeulen, Werbesaeule)
            json pj;
            if (fetchPoi(cx, cy, pj)) {
                for (const json& pe : pj["elements"]) {
                    double plat = 0, plon = 0;
                    if (pe.contains("lat") && pe.contains("lon")) { plat = pe["lat"]; plon = pe["lon"]; }
                    else if (pe.contains("center")) { plat = pe["center"].value("lat", 0.0); plon = pe["center"].value("lon", 0.0); }
                    else continue;
                    float px = (float)((lonToMx(plon) - g.mx0) * g.S), pz = (float)((latToMy(plat) - g.my0) * g.S);
                    if (!inside(px, pz)) continue;
                    bool fuel = tagOf(pe, "amenity") == "fuel";
                    Poi po;
                    po.kind = fuel ? 0 : 1;
                    po.x = px; po.z = pz;
                    po.name = tagOf(pe, "brand"); if (po.name.empty()) po.name = tagOf(pe, "name");
                    po.price = 1.55f + 0.3f * hash2((int)(px * 3), (int)(pz * 3));
                    // Ausrichtung an der naechsten Strasse
                    float bd = 1e9f, ux = 1, uz = 0, side = 1;
                    for (const RoadSeg& s : r.segs) {
                        float vx = s.bx - s.ax, vz = s.bz - s.az, l2 = vx * vx + vz * vz;
                        if (l2 < 1e-4f) continue;
                        float t = clampv(((px - s.ax) * vx + (pz - s.az) * vz) / l2, 0.0f, 1.0f);
                        float dd = std::hypot(px - (s.ax + vx * t), pz - (s.az + vz * t));
                        if (dd < bd) { bd = dd; float l = std::sqrt(l2); ux = vx / l; uz = vz / l; side = ((px - s.ax) * (-vz) + (pz - s.az) * vx) >= 0 ? 1.0f : -1.0f; }
                    }
                    po.ux = ux; po.uz = uz;
                    float gh = 0;
                    d->elev.height(g.mx0 + px / g.S, g.my0 + pz / g.S, gh, true);
                    Color brand = brandColor(po.name, fuel);
                    Vec3 c{px, gh, pz};
                    if (fuel) {
                        float hy = 2.6f;
                        pushBox(r.bldV, c + Vec3{0, 5.1f, 0}, {7.0f, 0.28f, 4.0f}, ux, uz, Color{236, 236, 238, 255});          // Dach
                        pushBox(r.bldV, c + Vec3{0, 4.75f, 0}, {7.05f, 0.18f, 4.05f}, ux, uz, brand);                           // Farbband
                        for (float a : {-5.4f, 5.4f}) for (float b : {-2.6f, 2.6f})
                            pushBox(r.bldV, c + Vec3{ux * a - uz * b, hy, uz * a + ux * b}, {0.18f, hy, 0.18f}, ux, uz, Color{220, 220, 224, 255}); // Stuetzen
                        for (float a : {-2.6f, 2.6f}) { // Zapfsaeulen auf Inseln
                            pushBox(r.bldV, c + Vec3{ux * a, 0.08f, uz * a}, {0.9f, 0.08f, 2.6f}, ux, uz, Color{170, 170, 172, 255});
                            for (float b : {-1.2f, 1.2f}) pushBox(r.bldV, c + Vec3{ux * a - uz * b, 0.95f, uz * a + ux * b}, {0.28f, 0.95f, 0.28f}, ux, uz, brand);
                        }
                        Vec3 pylon = c + Vec3{-ux * 9.5f, 0, -uz * 9.5f} + Vec3{-uz * 5.0f * side, 0, ux * 5.0f * side};
                        pushBox(r.bldV, pylon + Vec3{0, 3.2f, 0}, {0.12f, 3.2f, 0.12f}, ux, uz, Color{90, 90, 96, 255});
                        pushBox(r.bldV, pylon + Vec3{0, 6.3f, 0}, {0.18f, 1.0f, 1.0f}, ux, uz, brand);
                        pushBox(r.bldV, pylon + Vec3{0, 5.4f, 0}, {0.2f, 0.45f, 0.95f}, ux, uz, Color{245, 245, 245, 255});
                    } else {
                        Vec3 pylon = c + Vec3{-uz * 9.0f * side, 0, ux * 9.0f * side};
                        pushBox(r.bldV, pylon + Vec3{0, 3.5f, 0}, {0.14f, 3.5f, 0.14f}, ux, uz, Color{90, 90, 96, 255});
                        pushBox(r.bldV, pylon + Vec3{0, 7.4f, 0}, {0.2f, 1.1f, 2.0f}, ux, uz, brand);
                        pushBox(r.bldV, pylon + Vec3{0, 7.4f, 0}, {0.22f, 0.55f, 1.5f}, ux, uz, Color{245, 245, 245, 255});
                    }
                    r.pois.push_back(po);
                }
            }
        }
        bucketCells(sideV, r.x0, r.z0, r.x1, r.z1, r.sideCells);
        bucketCells(asphV, r.x0, r.z0, r.x1, r.z1, r.roadCells);
        bucketCells(r.bldV, r.x0, r.z0, r.x1, r.z1, r.bldCells);
        r.bldV.clear(); r.bldV.shrink_to_fit();
        r.ok = true;
    }
    std::lock_guard<std::mutex> lk(d->rm);
    d->chunkRes.push_back(std::move(r));
}

// ---------- World ----------
void World::init(const Config& cfg) {
    d = new Impl;
    loadOverrides();
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
    for (auto& c : d->chunks) { for (Cell& q : c.second.sideCells) q.m.destroy(); for (Cell& q : c.second.roadCells) q.m.destroy(); for (Cell& q : c.second.bldCells) q.m.destroy(); }
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
        t.lo = {1e9f, 1e9f, 1e9f}; t.hi = {-1e9f, -1e9f, -1e9f};
        for (const Vertex& q : r.v) { t.lo = {std::min(t.lo.x, q.p[0]), std::min(t.lo.y, q.p[1]), std::min(t.lo.z, q.p[2])}; t.hi = {std::max(t.hi.x, q.p[0]), std::max(t.hi.y, q.p[1]), std::max(t.hi.z, q.p[2])}; }
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
        uploadCells(r.sideCells, c.sideCells);
        uploadCells(r.roadCells, c.roadCells);
        uploadCells(r.bldCells, c.bldCells);
        c.segs = std::move(r.segs); c.names = std::move(r.names);
        c.controls = std::move(r.controls); c.buildings = std::move(r.buildings); c.pois = std::move(r.pois);
        if (r.ok) logMsg("OSM-Kachel %d/%d: %d Strassenabschnitte, %d Gebaeude, %d Ampeln/Schilder, %d Tankstellen/Supermärkte", r.cx, r.cy, (int)c.segs.size(), (int)c.buildings.size(), (int)c.controls.size(), (int)c.pois.size());
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
            for (Cell& q : it->second.sideCells) q.m.destroy(); for (Cell& q : it->second.roadCells) q.m.destroy(); for (Cell& q : it->second.bldCells) q.m.destroy();
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
void World::forSegmentsNear(float x, float z, float r, const std::function<void(const RoadSeg&)>& fn) const {
    auto scan = [&](const Chunk& c) {
        if (x + r < c.x0 || x - r > c.x1 || z + r < c.z0 || z - r > c.z1) return;
        for (const RoadSeg& s : c.segs) {
            if (std::max(s.ax, s.bx) < x - r || std::min(s.ax, s.bx) > x + r || std::max(s.az, s.bz) < z - r || std::min(s.az, s.bz) > z + r) continue;
            fn(s);
        }
    };
    scan(d->custom);
    for (auto& kv : d->chunks) scan(kv.second);
}
void World::forBuildingsNear(float x, float z, float r, const std::function<void(const Building&)>& fn) const {
    for (auto& kv : d->chunks) {
        const Chunk& c = kv.second;
        if (x + r < c.x0 || x - r > c.x1 || z + r < c.z0 || z - r > c.z1) continue;
        for (const Building& b : c.buildings)
            if (b.maxx >= x - r && b.minx <= x + r && b.maxz >= z - r && b.minz <= z + r) fn(b);
    }
}

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

RoadHit World::nearestRoad(float x, float z, float maxDist, int maxKlass) {
    RoadHit best;
    float bestScore = 1e9f;
    std::vector<Chunk*> list{&d->custom};
    for (auto& kv : d->chunks) list.push_back(&kv.second);
    for (Chunk* cp : list) {
        Chunk& c = *cp;
        if (x < c.x0 - 120 || x > c.x1 + 120 || z < c.z0 - 120 || z > c.z1 + 120) continue;
        for (const RoadSeg& s : c.segs) {
            if (s.klass > maxKlass) continue;
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


void World::setQuality(int q) {
    static const int r1[3] = {2, 3, 3}, r2[3] = {2, 3, 4};
    q = clampv(q, 0, 2);
    d->radius[1] = r1[q];
    d->radius[2] = r2[q];
}
void World::drawTerrain(int level, const Mat4& vp) {
    int z = d->zoom[level];
    Frustum fr(vp);
    for (auto& kv : d->tiles) {
        if (kv.second.z != z || !fr.sees(kv.second.lo, kv.second.hi)) continue;
        gfxBindTexture(kv.second.tex);
        kv.second.mesh.draw();
    }
}
void World::drawRoads(const Mat4& vp, Vec3 cam, float maxDist) {
    Frustum fr(vp);
    for (int pass = 0; pass < 2; pass++) // erst alle Gehsteige, dann aller Asphalt
        for (auto& kv : d->chunks)
            for (const Cell& c : pass == 0 ? kv.second.sideCells : kv.second.roadCells)
                if (boxDist(c.lo, c.hi, cam) < maxDist && fr.sees(c.lo, c.hi)) c.m.draw();
}
void World::drawBuildings(const Mat4& vp, Vec3 cam, float maxDist) {
    Frustum fr(vp);
    for (auto& kv : d->chunks)
        for (const Cell& c : kv.second.bldCells)
            if (boxDist(c.lo, c.hi, cam) < maxDist && fr.sees(c.lo, c.hi)) c.m.draw();
}

// ---------- Kartenfragen ----------
std::vector<MapConflict> World::takeConflicts() {
    std::lock_guard<std::mutex> lk(OV.m);
    std::vector<MapConflict> out = std::move(OV.found);
    OV.found.clear();
    return out;
}
void World::resolveConflict(const MapConflict& c, int lanes) {
    {
        std::lock_guard<std::mutex> lk(OV.m);
        if (lanes > 0) { OV.lanes[c.idA] = lanes; OV.lanes[c.idB] = lanes; }
        OV.resolved[c.key] = true;
        std::error_code ec;
        std::filesystem::create_directories("maps", ec);
        saveOverrides();
    }
    if (lanes > 0) { // Strassen mit der neuen Angabe neu aufbauen (aus dem Zwischenspeicher, ohne neuen Download)
        for (auto& ch : d->chunks) { for (Cell& q : ch.second.sideCells) q.m.destroy(); for (Cell& q : ch.second.roadCells) q.m.destroy(); for (Cell& q : ch.second.bldCells) q.m.destroy(); }
        d->chunks.clear();
        d->chunkReq.clear();
        d->version++;
    }
}

void World::forPoisNear(float x, float z, float r, const std::function<void(const Poi&)>& fn) const {
    for (auto& kv : d->chunks)
        for (const Poi& p : kv.second.pois)
            if (std::fabs(p.x - x) < r && std::fabs(p.z - z) < r) fn(p);
}
