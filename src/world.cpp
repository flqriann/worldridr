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

struct Tile { int z, x, y; Mesh mesh; GLuint tex = 0; double touched = 0; Vec3 lo, hi; std::vector<uint8_t> forest; int fw = 0, fh = 0; };
struct Cell { Mesh m; Vec3 lo, hi; };               // Teilnetz eines Chunks, einzeln sichtbarkeitsgeprueft
struct CellRes { std::vector<Vertex> v; Vec3 lo{1e9f, 1e9f, 1e9f}, hi{-1e9f, -1e9f, -1e9f}; };
static const int GC = 6;                              // Zellen pro Achse
struct TileRes { int gen, z, x, y; bool ok = false; std::vector<Vertex> v; std::vector<uint8_t> rgb; int w = 0, h = 0; std::vector<uint8_t> forest; };
struct CarveSeg { float ax, az, ay, bx, bz, by, hw; };   // Einschnitt: Strassenstueck unter dem Gelaende
struct RoadPatch {
    std::vector<float> px, pz, py;
    float hw = 3;
    float x0 = 1e9f, z0 = 1e9f, x1 = -1e9f, z1 = -1e9f;
};
struct Chunk {
    std::vector<RoadPatch> patches;   // ueberschriebene Strassenhoehen (Fahrflaeche unter der Gelaendehoehe)
    Mesh roads;                       // nur fuer selbst gebaute Strassen
    std::vector<Cell> sideCells, roadCells, bldCells, waterCells, deckCells;   // deckCells: Fahrbahn auf Bruecken (wird nach den Bauwerken mit Tiefe gezeichnet)
    std::vector<RoadSeg> segs;
    std::vector<std::string> names;
    std::vector<Control> controls;
    std::vector<Building> buildings;
    std::vector<Poi> pois;
    std::vector<TreePos> trees;
    std::vector<SignDef> signs;
    std::vector<CarveSeg> carves;
    float x0 = 0, z0 = 0, x1 = 0, z1 = 0;
    double touched = 0;
    bool ok = false;
};
struct ChunkRes {
    std::vector<RoadPatch> patches;
    int gen, cx, cy;
    bool ok = false;
    std::vector<Vertex> roadV, bldV;
    std::vector<CellRes> sideCells, roadCells, bldCells, waterCells, deckCells;
    std::vector<RoadSeg> segs;
    std::vector<std::string> names;
    std::vector<Control> controls;
    std::vector<Building> buildings;
    std::vector<Poi> pois;
    std::vector<TreePos> trees;
    std::vector<SignDef> signs;
    std::vector<CarveSeg> carves;
    float x0, z0, x1, z1;
};

struct World::Impl {
    Pool tilePool, osmPool;
    Elev elev;
    std::mutex rm;
    std::vector<TileRes> tileRes;
    std::vector<ChunkRes> chunkRes;
    std::mutex carveM;
    std::vector<CarveSeg> carveAll;                 // alle Einschnitte der geladenen Kacheln (fuer buildTile)
    std::unordered_map<uint64_t, bool> rebuildTiles; // Gelaendekacheln, die wegen neuer Einschnitte neu gebaut werden
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
            if (z == d->zoom[1]) { // Wald = dunkle, gruene Flaechen (Wasser ist blau, Daecher/Asphalt grau und werden nicht erfasst)
                r.forest.assign((size_t)w * h, 0);
                for (size_t i = 0; i < (size_t)w * h; i++) {
                    int R = px[i * 3], G = px[i * 3 + 1], B = px[i * 3 + 2];
                    int lum = (R * 3 + G * 6 + B) / 10;
                    r.forest[i] = (lum < 92 && G > R + 4 && G > B + 8) ? 1 : 0;
                }
            }
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
            if (z == d->zoom[2]) { // Einschnitte (Unterfuehrungen): Gelaende entlang abgesenkter Strassen ausschneiden
                std::vector<CarveSeg> cs;
                {
                    std::lock_guard<std::mutex> lk(d->carveM);
                    float x0 = p[0].x - 30, x1 = p[n * n - 1].x + 30, z0 = p[0].z - 30, z1 = p[n * n - 1].z + 30;
                    for (const CarveSeg& c : d->carveAll)
                        if (std::max(c.ax, c.bx) > x0 && std::min(c.ax, c.bx) < x1 && std::max(c.az, c.bz) > z0 && std::min(c.az, c.bz) < z1) cs.push_back(c);
                }
                if (!cs.empty())
                    for (Vec3& q : p)
                        for (const CarveSeg& c : cs) {
                            float vx = c.bx - c.ax, vz = c.bz - c.az, l2 = vx * vx + vz * vz;
                            float t = l2 > 1e-6f ? clampv(((q.x - c.ax) * vx + (q.z - c.az) * vz) / l2, 0.0f, 1.0f) : 0.0f;
                            float dist = std::hypot(q.x - (c.ax + vx * t), q.z - (c.az + vz * t));
                            float floorY = c.ay + (c.by - c.ay) * t - 0.35f;
                            if (q.y <= floorY) continue;
                            float full = c.hw + 5.0f, sl = std::max(4.0f, (q.y - floorY) * 0.8f);   // Sohle (breiter als die Maschenweite), dann Boeschung
                            if (dist < full) q.y = floorY;
                            else if (dist < full + sl) q.y = std::min(q.y, floorY + (q.y - floorY) * (dist - full) / sl);
                        }
            }
            r.v.reserve(cells * cells * 6);
            auto V = [&](int i, int j) {
                Vec3 q = p[j * n + i];
                // Gelaendeneigung aus dem Hoehenmodell: Normale fuer die Schummerung (Hanglicht), sichtbar als Relief
                Vec3 a = p[j * n + std::max(i - 1, 0)], b = p[j * n + std::min(i + 1, n - 1)], c0 = p[std::max(j - 1, 0) * n + i], d0 = p[std::min(j + 1, n - 1) * n + i];
                float cx = std::max(0.5f, b.x - a.x), cz = std::max(0.5f, d0.z - c0.z);
                Vec3 nn = normalize({-cz * (b.y - a.y), cz * cx, -(d0.y - c0.y) * cx});
                r.v.push_back({{q.x, q.y, q.z}, {nn.x, nn.y, nn.z}, {(float)i / cells, (float)j / cells}, Color{}});
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
// Baum (Stamm + Krone aus zwei Oktaedern) und Strassenlaterne als einfache Geometrie
static void pushOcta(std::vector<Vertex>& out, Vec3 c, float rx, float ry, float rz, Color col) {
    col.a = 0;
    Vec3 P[6] = {c + Vec3{rx, 0, 0}, c + Vec3{-rx, 0, 0}, c + Vec3{0, ry, 0}, c + Vec3{0, -ry, 0}, c + Vec3{0, 0, rz}, c + Vec3{0, 0, -rz}};
    static const int F[8][3] = {{0, 2, 4}, {4, 2, 1}, {1, 2, 5}, {5, 2, 0}, {4, 3, 0}, {1, 3, 4}, {5, 3, 1}, {0, 3, 5}};
    for (auto& f : F) {
        Vec3 n = normalize(cross(P[f[1]] - P[f[0]], P[f[2]] - P[f[0]]));
        if (dot(n, P[f[0]] - c) < 0) n = -n;
        for (int k = 0; k < 3; k++) out.push_back({{P[f[k]].x, P[f[k]].y, P[f[k]].z}, {n.x, n.y, n.z}, {0, 0}, col});
    }
}
static void pushTree(std::vector<Vertex>& out, float x, float gy, float z, float rnd) {
    static const Color greens[5] = {{58, 108, 50, 255}, {74, 128, 54, 255}, {50, 94, 46, 255}, {96, 138, 58, 255}, {66, 118, 60, 255}};
    float h = 5.0f + rnd * 4.0f, cr = 1.5f + rnd * 1.1f;
    pushBox(out, {x, gy + h * 0.28f, z}, {0.11f, h * 0.28f, 0.11f}, 1, 0, Color{84, 64, 46, 255});
    Color g = greens[(int)(rnd * 4.99f)];
    pushOcta(out, {x, gy + h * 0.72f, z}, cr, h * 0.30f, cr, g);
    pushOcta(out, {x, gy + h * 0.92f, z}, cr * 0.7f, h * 0.20f, cr * 0.7f, Color{(uint8_t)(g.r + 10), (uint8_t)(g.g + 12), (uint8_t)(g.b + 6), 255});
}
static void pushLamp(std::vector<Vertex>& out, float x, float gy, float z, float dx, float dz) {
    pushBox(out, {x, gy + 2.6f, z}, {0.05f, 2.6f, 0.05f}, 1, 0, Color{92, 94, 100, 255});
    pushBox(out, {x + dx * 0.55f, gy + 5.15f, z + dz * 0.55f}, {0.62f, 0.05f, 0.05f}, dx, dz, Color{92, 94, 100, 255});
    pushBox(out, {x + dx * 1.1f, gy + 5.05f, z + dz * 1.1f}, {0.22f, 0.06f, 0.12f}, dx, dz, Color{232, 228, 200, 255});
}

// Beliebige Flaeche (Boeschung): Normale wird zur Aussenseite ausgerichtet
static void pushQuadN(std::vector<Vertex>& out, Vec3 a, Vec3 b, Vec3 c, Vec3 d, Vec3 outward, Color col) {
    col.a = 0;
    Vec3 n = normalize(cross(b - a, d - a));
    if (dot(n, outward) < 0) n = -n;
    Vec3 pp[4] = {a, b, c, d};
    Vertex v[4];
    for (int i = 0; i < 4; i++) v[i] = {{pp[i].x, pp[i].y, pp[i].z}, {n.x, n.y, n.z}, {0, 0}, col};
    out.push_back(v[0]); out.push_back(v[1]); out.push_back(v[2]);
    out.push_back(v[0]); out.push_back(v[2]); out.push_back(v[3]);
}
// Strassenmarkierungen als flache Dreiecke im Strassenpass (Klasse 9 = weisse Farbe, siehe Shader)
static void markTri(std::vector<Vertex>& out, float x0, float z0, float x1, float z1, float x2, float z2, float y) {
    Color c{0, 0, 9, 0};
    out.push_back({{x0, y, z0}, {0, 1, 0}, {0, 0}, c}); out.push_back({{x1, y, z1}, {0, 1, 0}, {0, 0}, c}); out.push_back({{x2, y, z2}, {0, 1, 0}, {0, 0}, c});
}
static void markBar(std::vector<Vertex>& out, float ax, float az, float bx, float bz, float half, float y) {
    float dx = bx - ax, dz = bz - az, l = std::hypot(dx, dz);
    if (l < 1e-4f) return;
    float nx = -dz / l * half, nz = dx / l * half;
    markTri(out, ax - nx, az - nz, ax + nx, az + nz, bx + nx, bz + nz, y);
    markTri(out, ax - nx, az - nz, bx + nx, bz + nz, bx - nx, bz - nz, y);
}
static void markHead(std::vector<Vertex>& out, float tx, float tz, float dx, float dz, float len, float half, float y) {
    float bx = tx - dx * len, bz = tz - dz * len;
    markTri(out, tx, tz, bx - dz * half, bz + dx * half, bx + dz * half, bz - dx * half, y);
}
// Fahrspurpfeil in Fahrtrichtung (fx,fz): geradeaus, links und/oder rechts; sc = Groesse (1 = Spurbreite ca. 3,3 m)
static void arrowGlyph(std::vector<Vertex>& out, float cx, float cz, float y, float fx, float fz, bool thru, bool left, bool right, float sc) {
    float rxv = -fz, rzv = fx;
    auto P = [&](float u, float v, float& x, float& z) { x = cx + (fx * u + rxv * v) * sc; z = cz + (fz * u + rzv * v) * sc; };
    float ax, az, bx, bz;
    if (thru) {
        P(-1.3f, 0, ax, az); P(0.5f, 0, bx, bz); markBar(out, ax, az, bx, bz, 0.10f * sc, y);
        P(1.4f, 0, ax, az); markHead(out, ax, az, fx, fz, 0.9f * sc, 0.42f * sc, y);
    }
    for (int side = -1; side <= 1; side += 2) {
        if ((side < 0 && !left) || (side > 0 && !right)) continue;
        float u0 = thru ? -0.2f : 0.0f;
        if (!thru) { P(-1.3f, 0, ax, az); P(u0, 0, bx, bz); markBar(out, ax, az, bx, bz, 0.10f * sc, y); }
        float ex, ez, tx, tz;
        float ddx = (fx * 0.72f + rxv * 0.69f * side), ddz = (fz * 0.72f + rzv * 0.69f * side);   // 45 Grad zur Seite
        P(u0, 0, ax, az);
        ex = ax + ddx * 1.15f * sc; ez = az + ddz * 1.15f * sc;
        markBar(out, ax, az, ex, ez, 0.10f * sc, y);
        tx = ex + ddx * 0.55f * sc; tz = ez + ddz * 0.55f * sc;
        markHead(out, tx, tz, ddx, ddz, 0.85f * sc, 0.40f * sc, y);
    }
}
// Schnitt zweier Strecken (ta/tb = Parameter 0..1)
static bool segCross(float ax, float az, float bx, float bz, float cx, float cz, float dx, float dz, float& ta, float& tb) {
    float rx = bx - ax, rz = bz - az, sx = dx - cx, sz = dz - cz;
    float den = rx * sz - rz * sx;
    if (std::fabs(den) < 1e-6f) return false;
    float qx = cx - ax, qz = cz - az;
    ta = (qx * sz - qz * sx) / den;
    tb = (qx * rz - qz * rx) / den;
    return ta >= 0 && ta <= 1 && tb >= 0 && tb <= 1;
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
    // osm2: Abfrage enthaelt zusaetzlich Fussgaengeruebergaenge und Anschlussstellen (Namen fuer Wegweiser)
    snprintf(cache, sizeof cache, "cache/osm2/14_%d_%d.json", cx, cy);
    std::vector<uint8_t> data;
    if (readFile(cache, data)) {
        j = json::parse(data.begin(), data.end(), nullptr, false);
        if (!j.is_discarded() && j.contains("elements")) return true;
    }
    double n = myToLat(cy / 16384.0), s = myToLat((cy + 1) / 16384.0), w = cx / 16384.0 * 360 - 180, e = (cx + 1) / 16384.0 * 360 - 180;
    // 450 m Rand: die Hoehenfuehrung (Bruecken, Rampen, Unterfuehrungen) haengt von den Wegen der Umgebung ab. Ohne Rand rechnete jede Kachel
    // mit anderen Nachbarn, und an der Kachelgrenze entstand eine Stufe in der Fahrbahn.
    { double mLat = 450.0 / 111320.0, mLon = mLat / std::max(0.2, std::cos((n + s) * 0.5 * 3.14159265358979 / 180.0)); n += mLat; s -= mLat; w -= mLon; e += mLon; }
    char bb[128], q[1024];
    snprintf(bb, sizeof bb, "(%.6f,%.6f,%.6f,%.6f)", s, w, n, e);
    snprintf(q, sizeof q, "[out:json][timeout:40];(way[\"highway\"]%s;way[\"building\"]%s;way[\"railway\"][\"bridge\"]%s;node[\"highway\"~\"^(traffic_signals|stop|give_way|crossing|motorway_junction)$\"]%s;);out body geom qt;", bb, bb, bb, bb);
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
    // ohne Netz: aelteren Zwischenspeicher verwenden (ohne Zebrastreifen und Knotennamen)
    snprintf(cache, sizeof cache, "cache/osm/14_%d_%d.json", cx, cy);
    if (readFile(cache, data)) {
        j = json::parse(data.begin(), data.end(), nullptr, false);
        if (!j.is_discarded() && j.contains("elements")) return true;
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
    std::unordered_map<int64_t, std::pair<int, float>> hov; // OSM-Weg-ID -> (Modus, Versatz)
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
    if (j.contains("height") && j["height"].is_object())
        for (auto it = j["height"].begin(); it != j["height"].end(); ++it)
            if (it.value().is_array() && it.value().size() == 2) OV.hov[std::strtoll(it.key().c_str(), nullptr, 10)] = {it.value()[0].get<int>(), it.value()[1].get<float>()};
    if (j.contains("resolved") && j["resolved"].is_array())
        for (const json& k : j["resolved"]) if (k.is_string()) OV.resolved[k.get<std::string>()] = true;
}
static void saveOverrides() { // Aufrufer haelt OV.m
    json j;
    j["lanes"] = json::object();
    for (auto& kv : OV.lanes) j["lanes"][std::to_string(kv.first)] = kv.second;
    j["height"] = json::object();
    for (auto& kv : OV.hov) j["height"][std::to_string(kv.first)] = json::array({kv.second.first, kv.second.second});
    j["resolved"] = json::array();
    for (auto& kv : OV.resolved) j["resolved"].push_back(kv.first);
    std::string s = j.dump(1);
    writeFile(OV_FILE, std::vector<uint8_t>(s.begin(), s.end()));
}
static bool heightOverride(int64_t id, int& mode, float& delta) {
    std::lock_guard<std::mutex> lk(OV.m);
    auto it = OV.hov.find(id);
    if (it == OV.hov.end()) return false;
    mode = it->second.first; delta = it->second.second;
    return true;
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
    bool bridge = false, tunnel = false;   // bridge=*, tunnel=yes
    bool under = false, covered = false;   // Unterfuehrung/Einschnitt (layer < 0 oder covered=yes ohne Tunnel): Fahrbahn wird abgesenkt, Gelaende ausgeschnitten
    bool inferred = false;                 // Spurzahl stammt von einem Nachbarabschnitt (kein eigener lanes-Tag)
    float widthTag = 0;                    // width=* in Metern, 0 = nicht angegeben
    bool speedTag = false, link = false;   // maxspeed explizit gesetzt, *_link-Strasse (Auf-/Abfahrt)
    std::string dest, ref, jref;           // destination, ref, junction:ref (Wegweiser)
    int layer = 0;                         // layer=*
    int lanesBack = 0;                     // Spuren links der Mittellinie (Gegenrichtung); 0 = symmetrisch
    std::string turnF, turnB;              // turn:lanes in Fahrtrichtung / Gegenrichtung (links nach rechts)
    int klass, lanes, flags, oneway, name;
    float hw, maxspeed;
    std::vector<float> px, pz;
    std::vector<int64_t> nodes;
};

// ---------- Gewaesser (OpenStreetMap: natural=water, Flussufer, Fluesse/Kanaele/Baeche) ----------
// Die Umrisse werden auf ein 4-m-Raster gezeichnet (Scanline, gerade-ungerade-Regel: Inseln bleiben frei),
// zusammenhaengende Zellen einer Zeile werden zu einem Rechteck auf Wasserhoehe.
static bool fetchWater(int cx, int cy, json& j) {
    char cache[128];
    snprintf(cache, sizeof cache, "cache/osm/water_14_%d_%d.json", cx, cy);
    std::vector<uint8_t> data;
    if (readFile(cache, data)) {
        j = json::parse(data.begin(), data.end(), nullptr, false);
        if (!j.is_discarded() && j.contains("elements")) return true;
    }
    double n = myToLat(cy / 16384.0), s = myToLat((cy + 1) / 16384.0), w = cx / 16384.0 * 360 - 180, e = (cx + 1) / 16384.0 * 360 - 180;
    char bb[128], q[1024];
    snprintf(bb, sizeof bb, "(%.6f,%.6f,%.6f,%.6f)", s, w, n, e);
    snprintf(q, sizeof q, "[out:json][timeout:60];(way[\"natural\"=\"water\"]%s;way[\"waterway\"=\"riverbank\"]%s;way[\"waterway\"~\"^(river|canal|stream)$\"]%s;relation[\"natural\"=\"water\"]%s;relation[\"waterway\"=\"riverbank\"]%s;);out geom;", bb, bb, bb, bb, bb);
    static const char* servers[] = {"https://overpass.openstreetmap.fr/api/interpreter", "https://overpass-api.de/api/interpreter",
                                    "https://overpass.private.coffee/api/interpreter"};
    for (int attempt = 0; attempt < 4; attempt++) {
        std::string url = std::string(servers[attempt % 3]) + "?data=" + urlEncode(q);
        int st = 0;
        if (httpRequest(url, data, st) && st == 200) {
            j = json::parse(data.begin(), data.end(), nullptr, false);
            if (!j.is_discarded() && j.contains("elements") && !j.contains("remark")) { writeFile(cache, data); return true; }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1500 * (attempt + 1)));
    }
    return false;
}

// roadMask: gleiche 4-m-Zellen wie unten (nx * nz); dort liegt eine Strasse auf dem Boden, das Wasser wird ausgespart
static void buildWater(World::Impl* d, const GeoRef& g, int cx, int cy, float x0, float z0, float x1, float z1, std::vector<Vertex>& out, const std::vector<uint8_t>* roadMask = nullptr) {
    json j;
    if (!fetchWater(cx, cy, j)) return;
    const float CS = 4.0f;
    int nx = (int)std::ceil((x1 - x0) / CS), nz = (int)std::ceil((z1 - z0) / CS);
    if (nx < 1 || nz < 1 || nx > 1000 || nz > 1000) return;
    std::vector<uint8_t> mask((size_t)nx * nz, 0);
    bool any = false;
    auto toXZ = [&](const json& pt, float& x, float& z) {
        x = (float)((lonToMx(pt.value("lon", 0.0)) - g.mx0) * g.S);
        z = (float)((latToMy(pt.value("lat", 0.0)) - g.my0) * g.S);
    };
    // Jede Flaeche wird fuer sich gefuellt (gerade/ungerade je Rasterzeile). Zeilen mit ungerader Zahl von Schnittpunkten
    // (offener Umriss, fehlende Teile einer Relation) werden ausgelassen - frueher entstanden daraus Wasserflaechen quer durchs Gelaende.
    struct Edge { float ax, az, bx, bz; };
    auto fill = [&](const std::vector<Edge>& edges) {
        if (edges.size() < 3) return;
        float zmin = 1e18f, zmax = -1e18f;
        for (const Edge& e : edges) { zmin = std::min(zmin, std::min(e.az, e.bz)); zmax = std::max(zmax, std::max(e.az, e.bz)); }
        int r0 = std::max(0, (int)std::floor((zmin - z0) / CS)), r1 = std::min(nz - 1, (int)std::ceil((zmax - z0) / CS));
        std::vector<float> cr;
        for (int r = r0; r <= r1; r++) {
            float zc = z0 + (r + 0.5f) * CS;
            cr.clear();
            for (const Edge& e : edges) {
                float az = e.az, bz = e.bz, ax = e.ax, bx = e.bx;
                if (az == bz) continue;
                if (az > bz) { std::swap(ax, bx); std::swap(az, bz); }
                if (zc < az || zc >= bz) continue;
                cr.push_back(ax + (bx - ax) * (zc - az) / (bz - az));
            }
            if (cr.size() < 2 || (cr.size() & 1)) continue;
            std::sort(cr.begin(), cr.end());
            for (size_t k = 0; k + 1 < cr.size(); k += 2) {
                int c0 = std::max(0, (int)std::ceil((cr[k] - x0) / CS - 0.5f)), c1 = std::min(nx - 1, (int)std::floor((cr[k + 1] - x0) / CS - 0.5f));
                for (int c = c0; c <= c1; c++) { mask[(size_t)r * nx + c] = 1; any = true; }
            }
        }
    };
    auto stroke = [&](const std::vector<float>& px, const std::vector<float>& pz, float hw) {
        for (size_t i = 0; i + 1 < px.size(); i++) {
            float dx = px[i + 1] - px[i], dz = pz[i + 1] - pz[i], len = std::hypot(dx, dz);
            int steps = std::max(1, (int)(len / 2.0f));
            for (int s = 0; s <= steps; s++) {
                float x = px[i] + dx * s / steps, z = pz[i] + dz * s / steps;
                int c0 = (int)std::floor((x - hw - x0) / CS + 0.5f), c1 = (int)std::floor((x + hw - x0) / CS - 0.5f);
                int r0 = (int)std::floor((z - hw - z0) / CS + 0.5f), r1 = (int)std::floor((z + hw - z0) / CS - 0.5f);
                for (int r = std::max(0, r0); r <= std::min(nz - 1, r1); r++)
                    for (int c = std::max(0, c0); c <= std::min(nx - 1, c1); c++) { mask[(size_t)r * nx + c] = 1; any = true; }
            }
        }
    };
    auto hidden = [&](const json& e) { // verrohrt, unterirdisch oder nur zeitweise Wasser: nicht zeichnen
        std::string t = tagOf(e, "tunnel"), cv = tagOf(e, "covered"), im = tagOf(e, "intermittent"), ly = tagOf(e, "layer");
        return (!t.empty() && t != "no") || cv == "yes" || im == "yes" || (!ly.empty() && atoi(ly.c_str()) < 0);
    };
    std::unordered_map<int64_t, bool> inRel;
    for (const json& e : j["elements"])
        if (e.value("type", "") == "relation" && e.contains("members"))
            for (const json& m : e["members"]) if (m.value("type", "") == "way") inRel[m.value("ref", (int64_t)0)] = true;
    for (const json& e : j["elements"]) {
        std::string type = e.value("type", "");
        if (hidden(e)) continue;
        if (type == "relation" && e.contains("members")) {
            std::vector<Edge> edges;
            for (const json& m : e["members"]) {
                if (m.value("type", "") != "way" || !m.contains("geometry")) continue;
                const json& gm = m["geometry"];
                for (size_t i = 0; i + 1 < gm.size(); i++) { Edge ed; toXZ(gm[i], ed.ax, ed.az); toXZ(gm[i + 1], ed.bx, ed.bz); edges.push_back(ed); }
            }
            fill(edges);
        } else if (type == "way" && e.contains("geometry")) {
            if (inRel.count(e.value("id", (int64_t)0))) continue;
            const json& gm = e["geometry"];
            std::string ww = tagOf(e, "waterway");
            if (ww == "stream") continue;   // Baeche und Graeben: im Hoehenmodell nicht aufgeloest, wuerden als blaue Stufen im Gelaende liegen
            if (ww == "river" || ww == "canal") {
                std::vector<float> px, pz;
                for (const json& pt : gm) { float x, z; toXZ(pt, x, z); px.push_back(x); pz.push_back(z); }
                float w = tagOf(e, "width").empty() ? (ww == "river" ? 14.0f : 8.0f) : std::strtof(tagOf(e, "width").c_str(), nullptr);
                stroke(px, pz, std::max(6.0f, std::min(w, 80.0f)) * 0.5f);
            } else if (gm.size() >= 4) { // geschlossener Umriss
                std::vector<Edge> edges;
                for (size_t i = 0; i + 1 < gm.size(); i++) { Edge ed; toXZ(gm[i], ed.ax, ed.az); toXZ(gm[i + 1], ed.bx, ed.bz); edges.push_back(ed); }
                if (std::hypot(edges.front().ax - edges.back().bx, edges.front().az - edges.back().bz) > 1.0f) continue;   // offen
                fill(edges);
            }
        }
    }
    if (!any) return;
    if (roadMask && roadMask->size() == mask.size()) for (size_t i = 0; i < mask.size(); i++) if ((*roadMask)[i]) mask[i] = 0;   // keine Wasserflaeche ueber Strassen
    auto H = [&](float x, float z) { float h = 0; d->elev.height(g.mx0 + x / g.S, g.my0 + z / g.S, h, true); return h; };
    // Das Wasser liegt als Tuch auf dem Gelaende (Eckhoehen aus dem Hoehenmodell): Seen sind dort ohnehin eben, Fluesse folgen dem Gefaelle.
    // Eine waagrechte Flaeche auf mittlerer Hoehe ragte frueher am Hang aus dem Boden.
    std::vector<float> ch((size_t)(nx + 1) * (nz + 1), -1e9f);
    auto CH = [&](int c, int r) -> float {
        float& v = ch[(size_t)r * (nx + 1) + c];
        if (v < -1e8f) v = H(x0 + c * CS, z0 + r * CS) + 0.12f;
        return v;
    };
    Color col{44, 96, 128, 238};
    for (int r = 0; r < nz; r++)
        for (int c = 0; c < nx;) {
            if (!mask[(size_t)r * nx + c]) { c++; continue; }
            int e = c + 1;
            // Lauf verlaengern, solange die Eckhoehen linear zwischen Anfang und Ende liegen (ebene Seen: lange Streifen)
            while (e < nx && mask[(size_t)r * nx + e] && e - c < 24) {
                bool lin = true;
                float t0 = CH(c, r), t1 = CH(e + 1, r), b0 = CH(c, r + 1), b1 = CH(e + 1, r + 1);
                for (int q = c + 1; q <= e && lin; q++) {
                    float f = (float)(q - c) / (float)(e + 1 - c);
                    if (std::fabs(CH(q, r) - (t0 + (t1 - t0) * f)) > 0.06f || std::fabs(CH(q, r + 1) - (b0 + (b1 - b0) * f)) > 0.06f) lin = false;
                }
                if (!lin) break;
                e++;
            }
            float xa = x0 + c * CS, xb = x0 + e * CS, za = z0 + r * CS, zb = za + CS;
            Vec3 a{xa, CH(c, r), za}, b{xb, CH(e, r), za}, cc{xb, CH(e, r + 1), zb}, dd{xa, CH(c, r + 1), zb};
            Vec3 n{0, 1, 0};
            for (const Vec3& q : {a, dd, cc, a, cc, b}) out.push_back({{q.x, q.y, q.z}, {n.x, n.y, n.z}, {0, 0}, col});
            c = e;
        }
}

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
        std::vector<std::pair<std::vector<float>, std::vector<float>>> railBr;   // Bahnbruecken (Linienzug x, z)
        struct Deck { float x, z, tx, tz, len, top; };
        std::vector<Deck> decks;                                                 // Brueckenplatten der Bahn ueber Unterfuehrungen
        std::unordered_map<int64_t, NodeCtl> ctl;
        std::unordered_map<int64_t, bool> crossings;                                   // Fussgaengeruebergang: markiert (Zebrastreifen)?
        std::unordered_map<int64_t, std::pair<std::string, std::string>> jnodes;       // Anschlussstelle: Name, Nummer
        std::unordered_map<int64_t, int> junction;
        std::vector<WayTmp> ways;
        std::unordered_map<std::string, int> nameIdx;
        const json& els = j["elements"];
        for (const json& e : els) {
            std::string type = e.value("type", "");
            if (type == "node") {
                std::string h = tagOf(e, "highway");
                if (h == "crossing") {
                    std::string cr = tagOf(e, "crossing"), mk = tagOf(e, "crossing:markings"), cref = tagOf(e, "crossing_ref");
                    bool marked = cr == "zebra" || cr == "marked" || cr == "uncontrolled" || cref == "zebra" || (!mk.empty() && mk != "no");
                    crossings[e.value("id", (int64_t)0)] = marked;
                    continue;
                }
                if (h == "motorway_junction") { jnodes[e.value("id", (int64_t)0)] = {tagOf(e, "name"), tagOf(e, "ref")}; continue; }
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
            if (hw.empty() && !tagOf(e, "railway").empty()) {
                std::string bt = tagOf(e, "bridge");
                if (!bt.empty() && bt != "no") railBr.push_back({px, pz});
                continue;
            }
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
                int lanesFwd = atoi(tagOf(e, "lanes:forward").c_str()), lanesBwd = atoi(tagOf(e, "lanes:backward").c_str());
                if (lanesTag <= 0 && (lanesFwd > 0 || lanesBwd > 0)) lanesTag = std::min(8, lanesFwd + lanesBwd);
                { int ov; if (laneOverride(w.id, ov)) { lanesTag = ov; lanesFwd = lanesBwd = 0; } }
                w.lanesExplicit = lanesTag > 0;
                w.lanes = lanesTag > 0 ? std::min(lanesTag, 8) : (klass >= 6 ? 1 : (w.oneway && klass >= 2 ? 1 : 2));
                static const float laneW[8] = {3.75f, 3.5f, 3.5f, 3.25f, 3.25f, 2.9f, 3.4f, 2.8f};
                float width = w.lanes * laneW[klass] + (klass <= 1 ? 1.5f : 0.4f);
                float wt = std::strtof(tagOf(e, "width").c_str(), nullptr);
                if (wt >= 2.5f && wt <= 30) { width = wt; w.widthTag = wt; }
                w.hw = std::min(width * 0.5f, 25.0f);
                w.flags = ((klass <= 4 || lanesTag >= 2) && w.lanes >= 2 ? 1 : 0) | (klass <= 3 ? 2 : 0) | (w.oneway ? 4 : 0);
                // Brueckenlage, Tunnel und Stockwerk (layer)
                w.layer = clampv(atoi(tagOf(e, "layer").c_str()), -5, 5);
                { std::string bt = tagOf(e, "bridge"), tt = tagOf(e, "tunnel"); w.bridge = !bt.empty() && bt != "no"; w.tunnel = tt == "yes" || tt == "avalanche_protector";
                  std::string cv = tagOf(e, "covered"); int ly = atoi(tagOf(e, "layer").c_str());
                  w.covered = cv == "yes" || tt == "building_passage";
                  w.under = !w.bridge && !w.tunnel && klass <= 6 && (w.covered || ly < 0); }
                // Spuren je Richtung: die Mittellinie liegt nach lanes:backward (links der Wegrichtung) statt in der Mitte
                if (w.oneway == 0 && (w.flags & 1) && lanesTag > 0) {
                    int bk = lanesBwd > 0 ? lanesBwd : (lanesFwd > 0 ? w.lanes - lanesFwd : 0);
                    if (bk > 0 && bk < w.lanes && bk * 2 != w.lanes) { w.lanesBack = std::min(bk, 7); w.flags |= w.lanesBack << 5; }
                }
                // Abbiegepfeile (turn:lanes): Einbahn = alle Spuren, sonst je Richtung
                if (w.oneway == 1) w.turnF = tagOf(e, "turn:lanes");
                else if (w.oneway == -1) w.turnB = tagOf(e, "turn:lanes");
                else { w.turnF = tagOf(e, "turn:lanes:forward"); w.turnB = tagOf(e, "turn:lanes:backward"); }
                float def = klass == 0 ? 130 : klass == 1 ? 100 : (klass >= 6 ? 30 : (hw == "living_street" ? 20 : 50));
                w.maxspeed = parseSpeed(tagOf(e, "maxspeed"), def);
                w.speedTag = !tagOf(e, "maxspeed").empty();
                w.link = hw.size() > 5 && hw.compare(hw.size() - 5, 5, "_link") == 0;
                w.dest = tagOf(e, "destination"); w.ref = tagOf(e, "ref"); w.jref = tagOf(e, "junction:ref");
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
        { // Fehlende Spurzahlen von Nachbarabschnitten derselben Strasse uebernehmen (statt Klassenstandard: sonst wechselt die Breite zufaellig)
            static const float laneW[8] = {3.75f, 3.5f, 3.5f, 3.25f, 3.25f, 2.9f, 3.4f, 2.8f};
            std::unordered_map<int64_t, std::vector<int>> at;
            for (int wi = 0; wi < (int)ways.size(); wi++) {
                const WayTmp& w = ways[wi];
                if (w.nodes.size() < 2 || w.nodes.size() != w.px.size() || w.klass > 5 || w.ring) continue;
                at[w.nodes.front()].push_back(wi);
                at[w.nodes.back()].push_back(wi);
            }
            for (int iter = 0; iter < 4; iter++) { // ueber mehrere Abschnitte hinweg weitergeben
                bool any = false;
                for (int wi = 0; wi < (int)ways.size(); wi++) {
                    WayTmp& w = ways[wi];
                    if (w.lanesExplicit || w.inferred || w.nodes.size() != w.px.size() || w.nodes.size() < 2 || w.klass > 5 || w.ring || r.names[w.name].empty()) continue;
                    int found = 0;
                    for (int64_t node : {w.nodes.front(), w.nodes.back()}) {
                        auto it = at.find(node);
                        if (it == at.end()) continue;
                        int cand = 0, cnt = 0;
                        for (int oi : it->second) {
                            const WayTmp& o = ways[oi];
                            if (oi == wi || o.klass != w.klass || o.name != w.name || o.oneway != w.oneway) continue;
                            cnt++;
                            if (o.lanesExplicit || o.inferred) cand = o.lanes;
                        }
                        if (cnt == 1 && cand > 0) found = found == 0 ? cand : std::min(found, cand);   // nur eindeutige Fortsetzungen, nie an Abzweigen raten
                    }
                    if (found <= 0) continue;
                    w.inferred = true; any = true;
                    if (found != w.lanes) {
                        w.lanes = found;
                        float width = w.widthTag > 0 ? w.widthTag : w.lanes * laneW[w.klass] + (w.klass <= 1 ? 1.5f : 0.4f);
                        w.hw = std::min(width * 0.5f, 25.0f);
                        if (w.lanes >= 2 && (w.klass <= 4 || found >= 2)) w.flags |= 1; else w.flags &= ~1;
                    }
                }
                if (!any) break;
            }
        }
        // ----- Strassen: kleine Strassen zuerst, grosse zuletzt (liegen beim Zeichnen oben) -----
        // Erst alle Gehsteige, dann aller Asphalt: so schneidet kein Gehsteig die Einmuendung einer Seitenstrasse ab.
        std::stable_sort(ways.begin(), ways.end(), [](const WayTmp& a, const WayTmp& b) { return a.klass > b.klass; });
        std::vector<Vertex> sideV, asphV, deckV;
        int treeCount = 0, lampCount = 0;
        // Gegenstaende (Baeume, Laternen) nie auf einer Fahrbahn: Abstand zu allen Wegen der Kachel pruefen
        auto onAnyRoad = [&](float x, float z, float margin) {
            for (const WayTmp& o : ways) {
                float reach = o.hw + margin;
                for (size_t i = 0; i + 1 < o.px.size(); i++) {
                    float ax = o.px[i], az = o.pz[i], bx = o.px[i + 1], bz = o.pz[i + 1];
                    if (x < std::min(ax, bx) - reach || x > std::max(ax, bx) + reach || z < std::min(az, bz) - reach || z > std::max(az, bz) + reach) continue;
                    float vx = bx - ax, vz = bz - az, l2 = vx * vx + vz * vz;
                    float t = l2 > 1e-6f ? clampv(((x - ax) * vx + (z - az) * vz) / l2, 0.0f, 1.0f) : 0.0f;
                    if (std::hypot(x - (ax + vx * t), z - (az + vz * t)) < reach) return true;
                }
            }
            return false;
        };
        struct JN { float x, z, hw; float y = -1e9f; };   // y: Fahrbahnhoehe (Bruecke/Rampe), sonst Gelaende
        std::unordered_map<int64_t, JN> jmap;
        // ===== Strassengeometrie in drei Schritten: (1) glaetten + teilen, (2) Hoehenfuehrung, (3) Netze bauen =====
        struct WayGeo { std::vector<float> sx, sz, rx, rz, rd, ry, cumS, wedge, gy; bool bridge = false, tunnel = false, cut = false; };   // gy: Gelaendehoehe, cut: liegt im Einschnitt   // wedge: erlaubte Halbbreite je Stuetzpunkt (Rampenkeil)
        std::vector<WayGeo> wg(ways.size());
        for (size_t wi = 0; wi < ways.size(); wi++) { // (1) Linie glaetten (Kreuzungsknoten bleiben exakt), in ~5-m-Schritte teilen
            const WayTmp& w = ways[wi];
            WayGeo& G = wg[wi];
            int n = (int)w.px.size();
            std::vector<float>& sx = G.sx;
            std::vector<float>& sz = G.sz;
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
            G.cumS.assign(sn, 0.0f);
            float dist = 0;
            for (int i = 0; i + 1 < sn; i++) {
                float len = std::hypot(sx[i + 1] - sx[i], sz[i + 1] - sz[i]);
                G.cumS[i] = dist;
                int steps = std::max(1, (int)std::ceil(len / 5.0f));
                for (int s = 0; s < steps; s++) {
                    float t = (float)s / steps;
                    G.rx.push_back(sx[i] + (sx[i + 1] - sx[i]) * t);
                    G.rz.push_back(sz[i] + (sz[i + 1] - sz[i]) * t);
                    G.rd.push_back(dist + len * t);
                }
                dist += len;
            }
            G.cumS[sn - 1] = dist;
            G.rx.push_back(sx[sn - 1]); G.rz.push_back(sz[sn - 1]); G.rd.push_back(dist);
        }
        { // (2) Hoehenfuehrung. Basis = Gelaende; Bruecke = Gerade zwischen den Enden (ueber Taelern frei schwebend), Tunnel = unter dem Gelaende.
          // Bruecken muessen ueber kreuzenden Strassen mit niedrigerem Stockwerk 5,6 m Abstand haben; die Strassen steigen davor mit
          // begrenzter Steigung an (ueber gemeinsame Knoten hinweg), so entstehen Rampen/Daemme statt unmoeglicher Spruenge.
            const size_t NW = ways.size();
            std::vector<size_t> off(NW + 1, 0);
            for (size_t wi = 0; wi < NW; wi++) off[wi + 1] = off[wi] + wg[wi].rx.size();
            const size_t TOT = off[NW];
            std::vector<float> B(TOT), E(TOT, 0.0f), LB(TOT, 0.0f), SM(NW), bx0(NW), bx1(NW), bz0(NW), bz1(NW);
            bool anyStruct = false;
            for (size_t wi = 0; wi < NW; wi++) {
                const WayTmp& w = ways[wi];
                WayGeo& G = wg[wi];
                int m = (int)G.rx.size();
                G.bridge = w.bridge && m >= 2;
                G.tunnel = w.tunnel && !w.bridge && m >= 2;
                anyStruct = anyStruct || G.bridge || G.tunnel || (w.under && m >= 2) || !railBr.empty();
                SM[wi] = w.klass <= 1 ? 0.055f : (w.klass <= 5 ? 0.075f : 0.10f);
                bx0[wi] = bz0[wi] = 1e9f; bx1[wi] = bz1[wi] = -1e9f;
                for (int k = 0; k < m; k++) {
                    B[off[wi] + k] = H(G.rx[k], G.rz[k]);
                    bx0[wi] = std::min(bx0[wi], G.rx[k]); bx1[wi] = std::max(bx1[wi], G.rx[k]);
                    bz0[wi] = std::min(bz0[wi], G.rz[k]); bz1[wi] = std::max(bz1[wi], G.rz[k]);
                }
            }
            if (anyStruct) {
                const std::vector<float> gH(B); // Gelaendehoehe
                for (size_t wi = 0; wi < NW; wi++) {
                    WayGeo& G = wg[wi];
                    int m = (int)G.rx.size();
                    if (!G.bridge && !G.tunnel) continue;
                    float L = std::max(0.1f, G.rd[m - 1]), h0 = gH[off[wi]], h1 = gH[off[wi] + m - 1];
                    for (int k = 0; k < m; k++) {
                        size_t gi = off[wi] + k;
                        if (G.bridge) B[gi] = std::max(h0 + (h1 - h0) * (G.rd[k] / L), gH[gi] + 0.1f);
                        else B[gi] = gH[gi] - std::min(7.0f, 0.085f * std::min(G.rd[k], L - G.rd[k]));
                    }
                }
                // Stuetzpunkte an derselben Position (gemeinsame OSM-Knoten) bilden eine Gruppe mit gleicher Hoehe
                std::vector<std::pair<int64_t, size_t>> kv;
                kv.reserve(TOT);
                for (size_t wi = 0; wi < NW; wi++)
                    for (size_t k = 0; k < wg[wi].rx.size(); k++) {
                        int64_t key = ((int64_t)(int32_t)std::llround((double)wg[wi].rx[k] * 20.0) << 32) | (uint32_t)(int32_t)std::llround((double)wg[wi].rz[k] * 20.0);
                        kv.push_back({key, off[wi] + k});
                    }
                std::sort(kv.begin(), kv.end());
                std::vector<std::vector<size_t>> members;
                for (size_t i = 0; i < kv.size();) {
                    size_t j = i;
                    while (j < kv.size() && kv[j].first == kv[i].first) j++;
                    if (j - i >= 2) { members.emplace_back(); for (size_t q = i; q < j; q++) members.back().push_back(kv[q].second); }
                    i = j;
                }
                // ---- Unterfuehrungen: Das Hoehenmodell enthaelt den Bahndamm/die Bruecke darueber, die Strasse wuerde als Buckel darueber fuehren.
                //      Zielhoehe = Gerade zwischen den tiefsten Gelaendepunkten vor und hinter dem Abschnitt (bis 90 m in Verlaengerung);
                //      die Tiefe D >= 0 klingt auf den Anschlussstrassen mit der Steigungsgrenze ab (gleiche Rechnung wie der Aufschlag E, nur nach unten).
                std::vector<float> Dp(TOT, 0.0f);
                bool anyUnder = false;
                for (size_t wi = 0; wi < NW; wi++) {
                    const WayTmp& w = ways[wi];
                    const WayGeo& G = wg[wi];
                    int m = (int)G.rx.size();
                    if (!w.under || m < 2) continue;
                    auto lowBeyond = [&](int k0, int k1) { // vom Punkt k0 in Richtung weg von k1
                        float dx = G.rx[k0] - G.rx[k1], dz = G.rz[k0] - G.rz[k1], l = std::max(1e-3f, std::hypot(dx, dz));
                        dx /= l; dz /= l;
                        float lo = gH[off[wi] + k0];
                        for (float s2 = 12.0f; s2 <= 90.0f; s2 += 13.0f) lo = std::min(lo, H(G.rx[k0] + dx * s2, G.rz[k0] + dz * s2));
                        return lo;
                    };
                    float lA = lowBeyond(0, 1), lB = lowBeyond(m - 1, m - 2), L = std::max(0.1f, G.rd[m - 1]);
                    float mxD = 0;
                    for (int k = 0; k < m; k++) mxD = std::max(mxD, gH[off[wi] + k] - (lA + (lB - lA) * (G.rd[k] / L)));
                    if (mxD < 0.9f) continue;   // kein Damm im Hoehenmodell: nichts zu tun
                    for (int k = 0; k < m; k++) Dp[off[wi] + k] = clampv(gH[off[wi] + k] - (lA + (lB - lA) * (G.rd[k] / L)), 0.0f, 9.0f);
                    anyUnder = true;
                }
                // Strasse unter einer Bahnbruecke: mindestens 4,8 m unter der Oberkante (Gleise). Liegt die Bahn im Hoehenmodell auf einem Damm,
                // bleibt die Strasse auf dem Niveau davor/dahinter; liegt die Bahn ebenerdig, taucht die Strasse als Trog darunter durch.
                for (const auto& rb : railBr) {
                    const std::vector<float>& bxs = rb.first;
                    const std::vector<float>& bzs = rb.second;
                    for (size_t i = 0; i + 1 < bxs.size(); i++) {
                        float ax = bxs[i], az = bzs[i], bx = bxs[i + 1], bz = bzs[i + 1];
                        float mnx = std::min(ax, bx), mxx = std::max(ax, bx), mnz = std::min(az, bz), mxz = std::max(az, bz);
                        for (size_t wi = 0; wi < NW; wi++) {
                            const WayTmp& w = ways[wi];
                            const WayGeo& G = wg[wi];
                            int m = (int)G.rx.size();
                            if (G.bridge || G.tunnel || m < 2 || w.klass > 6 || w.layer > 0) continue;
                            if (bx0[wi] > mxx || bx1[wi] < mnx || bz0[wi] > mxz || bz1[wi] < mnz) continue;
                            for (int k = 0; k + 1 < m; k++) {
                                float ta, tb;
                                if (!segCross(ax, az, bx, bz, G.rx[k], G.rz[k], G.rx[k + 1], G.rz[k + 1], ta, tb)) continue;
                                float px = ax + (bx - ax) * ta, pz = az + (bz - az) * ta;
                                float dx = G.rx[k + 1] - G.rx[k], dz = G.rz[k + 1] - G.rz[k], dl = std::max(1e-3f, std::hypot(dx, dz));
                                dx /= dl; dz /= dl;
                                float top = H(px, pz), lo = top;
                                for (float s2 : {25.0f, 40.0f, 55.0f}) { lo = std::min(lo, H(px + dx * s2, pz + dz * s2)); lo = std::min(lo, H(px - dx * s2, pz - dz * s2)); }
                                float need = clampv(std::max(top - lo, 4.8f), 0.0f, 8.0f);
                                float rc = G.rd[k] + tb * (G.rd[k + 1] - G.rd[k]);
                                for (int q = 0; q < m; q++) if (std::fabs(G.rd[q] - rc) < 6.0f) { size_t gq = off[wi] + q; Dp[gq] = std::max(Dp[gq], std::max(0.0f, gH[gq] - (top - need))); }
                                anyUnder = true;
                                float rl = std::max(1e-3f, std::hypot(bx - ax, bz - az));
                                if (inside(px, pz) && decks.size() < 400) decks.push_back({px, pz, (bx - ax) / rl, (bz - az) / rl, w.hw + ((w.flags & 8) ? 1.9f : 0.0f) + 9.5f, top});
                            }
                        }
                    }
                }
                if (anyUnder) {
                    for (int round = 0; round < 80; round++) {
                        bool changed = false;
                        for (size_t wi = 0; wi < NW; wi++) {
                            const WayGeo& G = wg[wi];
                            if (G.bridge || G.tunnel) continue;
                            int m = (int)G.rx.size();
                            size_t o0 = off[wi];
                            float sm = std::max(0.07f, SM[wi]);
                            // Hoehe h = gH - D darf je Meter hoechstens um sm steigen/fallen
                            for (int k = 1; k < m; k++) { float hPrev = gH[o0 + k - 1] - Dp[o0 + k - 1], lim = hPrev + sm * (G.rd[k] - G.rd[k - 1]); float need = gH[o0 + k] - lim; if (need > Dp[o0 + k] + 1e-3f) { Dp[o0 + k] = need; changed = true; } }
                            for (int k = m - 2; k >= 0; k--) { float hNext = gH[o0 + k + 1] - Dp[o0 + k + 1], lim = hNext + sm * (G.rd[k + 1] - G.rd[k]); float need = gH[o0 + k] - lim; if (need > Dp[o0 + k] + 1e-3f) { Dp[o0 + k] = need; changed = true; } }
                        }
                        for (const auto& mem : members) {
                            float low = 1e9f;
                            bool any = false;
                            for (size_t gi : mem) if (Dp[gi] > 0.02f) { low = std::min(low, gH[gi] - Dp[gi]); any = true; }
                            if (!any) continue;
                            for (size_t gi : mem) { float d2 = gH[gi] - low; if (d2 > Dp[gi] + 1e-3f) { Dp[gi] = d2; changed = true; } }
                        }
                        if (!changed) break;
                    }
                    for (size_t wi = 0; wi < NW; wi++) {
                        WayGeo& G = wg[wi];
                        if (G.bridge || G.tunnel) continue;
                        int m = (int)G.rx.size();
                        float mxD = 0;
                        for (int k = 0; k < m; k++) { size_t gi = off[wi] + k; mxD = std::max(mxD, Dp[gi]); B[gi] = gH[gi] - Dp[gi]; }
                        if (mxD > 0.25f) { G.cut = true; G.gy.assign(gH.begin() + off[wi], gH.begin() + off[wi] + m); }
                    }
                }
                const float CLEAR = 5.6f;
                auto computeLB = [&]() { // geforderter Aufschlag der Bruecken ueber kreuzenden, tiefer liegenden Strassen
                    for (size_t wi = 0; wi < NW; wi++) {
                        if (!wg[wi].bridge) continue;
                        const WayTmp& w = ways[wi];
                        const WayGeo& G = wg[wi];
                        int la = std::max(1, w.layer), m = (int)G.rx.size();
                        for (int k = 0; k + 1 < m; k++) {
                            float ax = G.rx[k], az = G.rz[k], bx = G.rx[k + 1], bz = G.rz[k + 1];
                            float mnx = std::min(ax, bx) - 1, mxx = std::max(ax, bx) + 1, mnz = std::min(az, bz) - 1, mxz = std::max(az, bz) + 1;
                            for (size_t wj = 0; wj < NW; wj++) {
                                if (wj == wi) continue;
                                const WayTmp& o = ways[wj];
                                int lo = o.bridge ? std::max(1, o.layer) : o.layer;
                                if (lo >= la) continue;
                                if (bx0[wj] - o.hw > mxx || bx1[wj] + o.hw < mnx || bz0[wj] - o.hw > mxz || bz1[wj] + o.hw < mnz) continue;
                                const WayGeo& O = wg[wj];
                                int mo = (int)O.rx.size();
                                for (int j = 0; j + 1 < mo; j++) {
                                    float ta, tb;
                                    if (!segCross(ax, az, bx, bz, O.rx[j], O.rz[j], O.rx[j + 1], O.rz[j + 1], ta, tb)) continue;
                                    if ((ta < 0.01f || ta > 0.99f) && (tb < 0.01f || tb > 0.99f)) continue; // gemeinsamer Endpunkt, keine Kreuzung
                                    float px = ax + (bx - ax) * ta, pz = az + (bz - az) * ta;
                                    size_t gj = off[wj] + j;
                                    float yo = (B[gj] + E[gj]) * (1 - tb) + (B[gj + 1] + E[gj + 1]) * tb;
                                    float need = yo + CLEAR, reach = o.hw + 2.5f;
                                    for (int q = std::max(0, k - 2); q <= std::min(m - 1, k + 3); q++) {
                                        if (std::hypot(G.rx[q] - px, G.rz[q] - pz) > reach) continue;
                                        size_t gq = off[wi] + q;
                                        LB[gq] = std::max(LB[gq], need - B[gq]);
                                    }
                                }
                            }
                        }
                    }
                };
                for (int outer = 0; outer < 3; outer++) { // 3 Durchlaeufe: uebereinanderliegende Stockwerke
                    computeLB();
                    for (size_t gi = 0; gi < TOT; gi++) E[gi] = std::max(E[gi], LB[gi]);
                    for (int round = 0; round < 80; round++) { // Aufschlag klingt mit der Steigungsgrenze ab, an gemeinsamen Knoten gleich
                        bool changed = false;
                        for (size_t wi = 0; wi < NW; wi++) {
                            const WayGeo& G = wg[wi];
                            int m = (int)G.rx.size();
                            size_t o0 = off[wi];
                            for (int k = 1; k < m; k++) { float need = E[o0 + k - 1] - SM[wi] * (G.rd[k] - G.rd[k - 1]); if (need > E[o0 + k] + 1e-4f) { E[o0 + k] = need; changed = true; } }
                            for (int k = m - 2; k >= 0; k--) { float need = E[o0 + k + 1] - SM[wi] * (G.rd[k + 1] - G.rd[k]); if (need > E[o0 + k] + 1e-4f) { E[o0 + k] = need; changed = true; } }
                        }
                        for (const auto& mem : members) {
                            float top = -1e9f;
                            for (size_t gi : mem) top = std::max(top, B[gi] + E[gi]);
                            for (size_t gi : mem) { float e2 = top - B[gi]; if (e2 > E[gi] + 1e-4f) { E[gi] = e2; changed = true; } }
                        }
                        if (!changed) break;
                    }
                }
                for (size_t wi = 0; wi < NW; wi++) { // Fahrbahnhoehe nur dort setzen, wo sie vom Gelaende abweicht
                    WayGeo& G = wg[wi];
                    int m = (int)G.rx.size();
                    float mx = 0;
                    for (int k = 0; k < m; k++) mx = std::max(mx, E[off[wi] + k]);
                    if (!(G.bridge || G.tunnel || G.cut || mx > 0.12f)) continue;
                    G.ry.resize(m);
                    for (int k = 0; k < m; k++) G.ry[k] = B[off[wi] + k] + E[off[wi] + k];
                    for (int pass = 0; pass < 3; pass++) // Knicke an den Rampenfuessen ausrunden (nur nach oben, Mindesthoehen bleiben erhalten)
                        for (int k = 1; k + 1 < m; k++) { float avg = 0.25f * G.ry[k - 1] + 0.5f * G.ry[k] + 0.25f * G.ry[k + 1]; if (avg > G.ry[k]) G.ry[k] = avg; }
                    if (G.cut) { // Gelaende entlang des Einschnitts ausschneiden (wird beim Bau der Gelaendekacheln beruecksichtigt)
                        const WayTmp& w = ways[wi];
                        float chw = w.hw + ((w.flags & 8) ? 1.9f : 0.0f);
                        for (int k = 0; k + 1 < m; k++) {
                            if (G.gy[k] - G.ry[k] < 0.25f && G.gy[k + 1] - G.ry[k + 1] < 0.25f) continue;
                            if (!inside((G.rx[k] + G.rx[k + 1]) * 0.5f, (G.rz[k] + G.rz[k + 1]) * 0.5f)) continue;
                            r.carves.push_back({G.rx[k], G.rz[k], G.ry[k], G.rx[k + 1], G.rz[k + 1], G.ry[k + 1], chw});
                        }
                    }
                }
            }
        }
        { // Auf- und Abfahrten: die OSM-Linie einer Rampe (*_link) beginnt auf der Mittellinie der Hauptfahrbahn und wuerde sie ueberlagern.
          // Mathematisch: der seitliche Abstand d(s) der Rampenlinie von der Hauptfahrbahn muss mindestens
          //   req(s) = hwHaupt + 0,4 m + hwRampe * w(s),  w(s) = smoothstep(s / 90 m)
          // betragen (s = Strecke vom Abzweig). Wo d kleiner ist, wird die Linie um genau die Differenz nach aussen geschoben;
          // gleichzeitig waechst die Rampenbreite keilfoermig mit w(s). Ergebnis: die Rampe loest sich sauber vom Fahrbahnrand.
          // Die Navigations-/Verkehrslinie (sx/sz) behaelt ihren Anfangspunkt im Knoten, damit das Strassennetz verbunden bleibt.
            auto sq = [](float v) { return v * v; };
            for (size_t li = 0; li < ways.size(); li++) {
                const WayTmp& L = ways[li];
                if (!L.link || L.klass > 2 || L.nodes.size() != L.px.size() || L.nodes.size() < 2) continue;
                WayGeo& GL = wg[li];
                int ml = (int)GL.rx.size(), sn2 = (int)GL.sx.size();
                if (ml < 3 || sn2 < 3) continue;
                float Lw = GL.rd[ml - 1];
                if ((int)GL.wedge.size() != ml) GL.wedge.assign(ml, 1e9f);
                for (int end = 0; end < 2; end++) {
                    int64_t N = end == 0 ? L.nodes.front() : L.nodes.back();
                    int mi = -1, idx = -1;
                    for (size_t q = 0; q < ways.size() && mi < 0; q++) { // Hauptfahrbahn: Richtungsfahrbahn, die den Knoten enthaelt
                        const WayTmp& M = ways[q];
                        if (q == li || M.link || M.klass > 3 || M.oneway == 0 || M.nodes.size() != M.px.size()) continue;
                        for (size_t t = 0; t < M.nodes.size(); t++) if (M.nodes[t] == N) { mi = (int)q; idx = (int)t; break; }
                    }
                    if (mi < 0) continue;
                    const WayTmp& M = ways[mi];
                    const WayGeo& GM = wg[mi];
                    int mm = (int)GM.rx.size();
                    if (mm < 2) continue;
                    float npx = M.px[idx], npz = M.pz[idx];
                    int kk = 0;
                    float bd = 1e18f;
                    for (int k = 0; k < mm; k++) { float dd = sq(GM.rx[k] - npx) + sq(GM.rz[k] - npz); if (dd < bd) { bd = dd; kk = k; } }
                    int lo = kk, hi = kk;
                    while (lo > 0 && GM.rd[kk] - GM.rd[lo] < 350.0f) lo--;
                    while (hi < mm - 1 && GM.rd[hi] - GM.rd[kk] < 350.0f) hi++;
                    int k2 = std::min(kk + 1, mm - 1), k1 = std::max(kk - 1, 0);
                    float tx = GM.rx[k2] - GM.rx[k1], tz = GM.rz[k2] - GM.rz[k1], tn = std::hypot(tx, tz);
                    if (tn < 1e-4f) continue;
                    float lnx = -tz / tn, lnz = tx / tn;   // rechte Normale der Hauptfahrbahn
                    float sideSign = 1.0f;                 // Rampe liegt rechts (+1) oder links (-1) der Fahrtrichtung
                    for (int k = 0; k < ml; k++) {
                        float s = end == 0 ? GL.rd[k] : Lw - GL.rd[k];
                        if (s >= 40.0f) { sideSign = ((GL.rx[k] - npx) * lnx + (GL.rz[k] - npz) * lnz) >= 0 ? 1.0f : -1.0f; break; }
                    }
                    auto shiftPt = [&](float& x, float& z, float s) {
                        if (s > 230.0f) return;
                        float u = clampv(s / 90.0f, 0.0f, 1.0f), wv = u * u * (3.0f - 2.0f * u);
                        float req = M.hw + 0.4f + L.hw * wv;
                        float bestD = 1e18f, qx = 0, qz = 0;
                        for (int j = lo; j < hi; j++) {
                            float vx = GM.rx[j + 1] - GM.rx[j], vz = GM.rz[j + 1] - GM.rz[j], l2 = vx * vx + vz * vz;
                            float t = l2 > 1e-6f ? clampv(((x - GM.rx[j]) * vx + (z - GM.rz[j]) * vz) / l2, 0.0f, 1.0f) : 0.0f;
                            float px = GM.rx[j] + vx * t, pz = GM.rz[j] + vz * t, dd = sq(x - px) + sq(z - pz);
                            if (dd < bestD) { bestD = dd; qx = px; qz = pz; }
                        }
                        float d = std::sqrt(bestD), ux, uz;
                        if (d < 0.05f) { ux = lnx * sideSign; uz = lnz * sideSign; } else { ux = (x - qx) / d; uz = (z - qz) / d; }
                        if (d < req) { x += ux * (req - d); z += uz * (req - d); }
                    };
                    for (int k = 0; k < ml; k++) { // Fahrbahnband (dicht): verschieben und Keilbreite setzen
                        float s = end == 0 ? GL.rd[k] : Lw - GL.rd[k];
                        shiftPt(GL.rx[k], GL.rz[k], s);
                        float u = clampv(s / 90.0f, 0.0f, 1.0f), wv = u * u * (3.0f - 2.0f * u);
                        GL.wedge[k] = std::min(GL.wedge[k], std::max(0.6f, L.hw * wv));
                    }
                    for (int i = 1; i + 1 < sn2; i++) { // Stuetzpunkte fuer Segmente/Verkehr: Endpunkte bleiben im Knoten
                        float s = end == 0 ? GL.cumS[i] : GL.cumS[sn2 - 1] - GL.cumS[i];
                        shiftPt(GL.sx[i], GL.sz[i], s);
                    }
                }
            }
        }
        // Wegenden je Knoten (fuer Spurverengungen und Schilder)
        std::unordered_map<int64_t, std::vector<int>> endsAt;
        for (int wi = 0; wi < (int)ways.size(); wi++) {
            const WayTmp& w = ways[wi];
            if (w.nodes.size() < 2 || w.nodes.size() != w.px.size()) continue;
            endsAt[w.nodes.front()].push_back(wi);
            endsAt[w.nodes.back()].push_back(wi);
        }
        for (size_t wi = 0; wi < ways.size(); wi++) { // (3) Segmente, Fahrbahn-/Gehsteignetze, Bauwerke, Schilder
            const WayTmp& w = ways[wi];
            WayGeo& G = wg[wi];
            int n = (int)w.px.size();
            const std::vector<float>& sx = G.sx;
            const std::vector<float>& sz = G.sz;
            const std::vector<float>& rx = G.rx;
            const std::vector<float>& rz = G.rz;
            const std::vector<float>& rd = G.rd;
            std::vector<float>& ry = G.ry;      // Fahrbahnhoehe, leer = folgt dem Gelaende
            int sn = (int)sx.size(), m = (int)rx.size();
            {   // Hoehen-Override aus dem Studio (Taste 5) ersetzt die automatische Hoehenfuehrung
                int hm = 0; float hd = 0;
                if (m >= 2 && heightOverride(w.id, hm, hd) && hm != 0) {
                    ry.resize(m);
                    if (hm == 1) { // Boden: Rampe zwischen den Hoehen kurz vor Anfang und kurz hinter Ende (Unterfuehrungen)
                        float d0x = rx[1] - rx[0], d0z = rz[1] - rz[0], l0 = std::max(0.01f, std::hypot(d0x, d0z));
                        float d1x = rx[m - 1] - rx[m - 2], d1z = rz[m - 1] - rz[m - 2], l1 = std::max(0.01f, std::hypot(d1x, d1z));
                        float hA = H(rx[0] - d0x / l0 * 6.0f, rz[0] - d0z / l0 * 6.0f), hB = H(rx[m - 1] + d1x / l1 * 6.0f, rz[m - 1] + d1z / l1 * 6.0f);
                        float tot = std::max(0.01f, rd[m - 1]);
                        for (int k = 0; k < m; k++) ry[k] = hA + (hB - hA) * (rd[k] / tot);
                    } else for (int k = 0; k < m; k++) ry[k] = H(rx[k], rz[k]) + hd;
                }
            }
            auto yAtDist = [&](float dd) { // Fahrbahnhoehe bei Bogenlaenge dd
                int lo = 0, hi = m - 1;
                while (hi - lo > 1) { int mid = (lo + hi) / 2; if (rd[mid] <= dd) lo = mid; else hi = mid; }
                float t = (dd - rd[lo]) / std::max(1e-4f, rd[hi] - rd[lo]);
                return ry[lo] + (ry[hi] - ry[lo]) * clampv(t, 0.0f, 1.0f);
            };
            for (int i = 0; i + 1 < sn; i++) {
                if (!inside((sx[i] + sx[i + 1]) * 0.5f, (sz[i] + sz[i + 1]) * 0.5f)) continue;
                RoadSeg sg{sx[i], sz[i], sx[i + 1], sz[i + 1], w.hw, w.maxspeed, (int8_t)w.oneway, (uint8_t)w.klass, (uint8_t)w.lanes, (uint8_t)(w.flags & 1), w.name, (uint8_t)(w.ring ? w.ringOuter : 0), w.id};
                if (!ry.empty()) { sg.ya = yAtDist(G.cumS[i]); sg.yb = yAtDist(G.cumS[i + 1]); }
                r.segs.push_back(sg);
            }
            bool hasSw = (w.flags & 8) != 0;
            if (!ry.empty()) { // Fahrflaeche fuer die Physik (Bruecke, Tunnel, Rampe)
                RoadPatch pt;
                pt.px = rx; pt.pz = rz; pt.py = ry; pt.hw = w.hw + (hasSw ? 1.9f : 0.0f);
                for (int k = 0; k < m; k++) { pt.x0 = std::min(pt.x0, rx[k]); pt.x1 = std::max(pt.x1, rx[k]); pt.z0 = std::min(pt.z0, rz[k]); pt.z1 = std::max(pt.z1, rz[k]); }
                r.patches.push_back(std::move(pt));
            }
            if ((int)w.nodes.size() == n) for (int i = 0; i < n; i++) if (junction[w.nodes[i]] >= 3 && !w.ring && w.klass > 1) {   // Autobahn-/Schnellstrassenknoten bekommen keine runde Asphaltscheibe (Rampen werden getrennt gefuehrt)
                JN& jn = jmap[w.nodes[i]]; jn.x = w.px[i]; jn.z = w.pz[i]; jn.hw = std::max(jn.hw, w.hw);
                if (!ry.empty()) { float bd = 1e18f; for (int k = 0; k < m; k++) { float dd = (rx[k] - jn.x) * (rx[k] - jn.x) + (rz[k] - jn.z) * (rz[k] - jn.z); if (dd < bd) { bd = dd; jn.y = ry[k]; } } }
            }
            float hwq = std::min(255.0f, w.hw * 10.0f) / 10.0f;
            // Spurverengung/-erweiterung: der breitere Abschnitt einer Strasse verjuengt sich zum Knoten hin auf die Breite der Fortsetzung
            // (Keil mit weichem Verlauf, 28 m innerorts, 50 m, 90 m auf Schnellstrassen) statt eines Sprungs an der Wegegrenze
            std::vector<float> hwv(m, hwq);
            if ((int)G.wedge.size() == m) for (int k = 0; k < m; k++) hwv[k] = std::min(hwv[k], std::min(255.0f, G.wedge[k] * 10.0f) / 10.0f);   // Rampenkeil
            if (!w.ring && (int)w.nodes.size() == n && w.klass <= 5 && m >= 2) {
                float Lw = rd[m - 1], taperLen = w.klass <= 1 ? 90.0f : (w.klass <= 3 ? 50.0f : 28.0f);
                for (int side = 0; side < 2; side++) {
                    int64_t node = side == 0 ? w.nodes.front() : w.nodes.back();
                    auto it = endsAt.find(node);
                    if (it == endsAt.end()) continue;
                    float tx = side == 0 ? rx[1] - rx[0] : rx[m - 2] - rx[m - 1], tz = side == 0 ? rz[1] - rz[0] : rz[m - 2] - rz[m - 1];
                    float tn = std::hypot(tx, tz);
                    if (tn < 1e-4f) continue;
                    tx /= tn; tz /= tn;
                    float bestHw = -1;
                    for (int oi : it->second) {
                        const WayTmp& o = ways[oi];
                        if (oi == (int)wi || o.klass != w.klass || o.name != w.name || r.names[w.name].empty() || o.oneway != w.oneway || o.ring) continue;
                        const WayGeo& OG = wg[oi];
                        int om = (int)OG.rx.size();
                        if (om < 2) continue;
                        bool oStart = o.nodes.front() == node;
                        float ox = oStart ? OG.rx[1] - OG.rx[0] : OG.rx[om - 2] - OG.rx[om - 1], oz = oStart ? OG.rz[1] - OG.rz[0] : OG.rz[om - 2] - OG.rz[om - 1];
                        float ol = std::hypot(ox, oz);
                        if (ol < 1e-4f) continue;
                        ox /= ol; oz /= ol;
                        if (tx * ox + tz * oz > -0.6f) continue;   // laeuft nicht weiter, sondern zweigt ab
                        bestHw = bestHw < 0 ? o.hw : std::min(bestHw, o.hw);
                    }
                    if (bestHw < 0 || bestHw >= w.hw - 0.3f) continue;   // nur der breitere Abschnitt verjuengt sich
                    float len = std::min(taperLen, Lw * 0.45f);
                    for (int k = 0; k < m; k++) {
                        float s = side == 0 ? rd[k] : Lw - rd[k];
                        if (s >= len) continue;
                        float u = s / len, sm = u * u * (3 - 2 * u);
                        hwv[k] = std::min(hwv[k], std::min(255.0f, (bestHw + (w.hw - bestHw) * sm) * 10.0f) / 10.0f);
                    }
                }
            }
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
                    float hk = half + (hwv[k] - hwq);   // Breite an diesem Punkt (Keil bei Spurwechsel)
                    float x = rx[k] + mx * hk * side / ch, z = rz[k] + mz * hk * side / ch;
                    // Bruecke/Tunnel: ebene Fahrbahn exakt auf Projekthoehe; Rampe/Damm: nie unter dem Gelaende (Haenge)
                    // Auf dem Boden: nie unter dem Gelaende der Fahrbahnmitte, sonst schaut ein Hoecker durch den Asphalt
                    float yy = ry.empty() ? std::max(H(x, z), H(rx[k], rz[k])) : ((G.bridge || G.tunnel || (G.cut && G.gy[k] - ry[k] > 0.2f)) ? ry[k] : std::max(ry[k], H(x, z)));
                    Color ck = col;
                    ck.a = (uint8_t)std::min(255.0f, hwv[k] * 10.0f);
                    o = {{x, yy + 0.03f, z}, {0, 1, 0}, {hk * side, rd[k]}, ck};
                };
                for (int k = 0; k + 1 < m; k++) {
                    if (!inside((rx[k] + rx[k + 1]) * 0.5f, (rz[k] + rz[k + 1]) * 0.5f)) continue;
                    Vertex L0, R0, L1, R1;
                    edge(k, -1, L0); edge(k, 1, R0); edge(k + 1, -1, L1); edge(k + 1, 1, R1);
                    dst.push_back(L0); dst.push_back(R0); dst.push_back(R1);
                    dst.push_back(L0); dst.push_back(R1); dst.push_back(L1);
                }
            };
            // Bruecken: Die Fahrbahn wird ohne Tiefenwerte gezeichnet (liegt sonst direkt auf dem Gelaende). Die Unterseite der Brueckenplatte
            // kam danach und ueberdeckte die Fahrbahn samt Markierungen. Fahrbahnen auf Bruecken kommen deshalb in einen eigenen Durchgang.
            if (hasSw) ribbon(hwq + 1.9f, w.flags | 16, G.bridge ? deckV : sideV);
            ribbon(hasSw ? hwq + 0.15f : hwq, w.flags, G.bridge ? deckV : asphV);
            // ----- Bauwerke: Bruecke (geneigte Platte, Gelaender, Pfeiler), Tunnelroehre, Damm/Boeschung bei erhoehter Fahrbahn -----
            // Alle Flaechen folgen der Fahrbahnneigung (ry je Stuetzpunkt) und den gleichen Rand-Punkten wie das Asphaltband; waagerechte Quader
            // wuerden auf Rampen Stufen bilden und die Fahrbahn samt Markierungen verdecken.
            if (G.cut && w.covered && m >= 2 && !ry.empty()) {
                const Color conc{150, 150, 146, 0}, concD{104, 104, 100, 0};
                float top = -1e9f;
                for (int k = 0; k < m; k++) top = std::max(top, G.gy[k]);
                for (int k = 0; k + 1 < m; k++) {
                    float x0 = rx[k], z0 = rz[k], x1 = rx[k + 1], z1 = rz[k + 1], tx = x1 - x0, tz = z1 - z0, tl = std::hypot(tx, tz);
                    if (tl < 0.05f || !inside((x0 + x1) * 0.5f, (z0 + z1) * 0.5f)) continue;
                    tx /= tl; tz /= tl;
                    float yr = std::max(ry[k], ry[k + 1]);
                    if (top - 0.9f - yr < 3.4f) continue;   // zu wenig Durchfahrtshoehe: offen lassen
                    float halfW = hwq + (hasSw ? 1.9f : 0.0f) + 0.6f;
                    Vec3 c{(x0 + x1) * 0.5f, 0, (z0 + z1) * 0.5f};
                    pushBox(r.bldV, {c.x, top - 0.40f, c.z}, {tl * 0.5f + 0.05f, 0.45f, halfW + 1.2f}, tx, tz, conc);
                    for (float sd : {-1.0f, 1.0f}) { // Widerlagerwaende
                        Vec3 wc{c.x - tz * sd * (halfW + 0.5f), (top + yr) * 0.5f - 0.4f, c.z + tx * sd * (halfW + 0.5f)};
                        pushBox(r.bldV, wc, {tl * 0.5f + 0.05f, (top - yr) * 0.5f + 0.2f, 0.5f}, tx, tz, concD);
                    }
                }
            }
            if (!ry.empty() && m >= 2) {
                const float Wd = hwq + (hasSw ? 1.9f : 0.0f);   // halbe Breite inkl. Gehsteig = Rand der Fahrbahnplatte
                const Color conc{158, 158, 152, 0}, concD{116, 116, 112, 0}, tunC{104, 104, 100, 0}, fillC{92, 106, 68, 0};
                auto edgeXZ = [&](int k, float side, float half, float& x, float& z) { // gleiche Gehrung wie das Asphaltband
                    float n0x = 0, n0z = 1, n1x = 0, n1z = 1;
                    bool h0 = false, h1 = false;
                    if (k > 0) { float tx = rx[k] - rx[k - 1], tz = rz[k] - rz[k - 1], l = std::hypot(tx, tz); if (l > 1e-4f) { n0x = -tz / l; n0z = tx / l; h0 = true; } }
                    if (k < m - 1) { float tx = rx[k + 1] - rx[k], tz = rz[k + 1] - rz[k], l = std::hypot(tx, tz); if (l > 1e-4f) { n1x = -tz / l; n1z = tx / l; h1 = true; } }
                    if (!h0 && !h1) { n0x = 0; n0z = 1; n1x = 0; n1z = 1; }
                    else if (!h0) { n0x = n1x; n0z = n1z; }
                    else if (!h1) { n1x = n0x; n1z = n0z; }
                    float mx = n0x + n1x, mz = n0z + n1z, l = std::hypot(mx, mz);
                    if (l < 1e-3f) { mx = n1x; mz = n1z; l = 1; }
                    mx /= l; mz /= l;
                    float ch = std::max(0.6f, mx * n0x + mz * n0z);
                    x = rx[k] + mx * half * side / ch; z = rz[k] + mz * half * side / ch;
                };
                auto P = [](float x, float y, float z) { return Vec3{x, y, z}; };
                // Dammboeschung: je Stuetzpunkt ein Fusspunkt (Breite geglaettet). Frueher je Abschnitt mit eigener Richtung berechnet:
                // in Kurven und an Abzweigen entstanden daraus gezackte, uebereinanderliegende Flaechen.
                std::vector<float> skE[2], skX[2], skZ[2], skFx[2], skFz[2];
                if (!G.bridge && !G.tunnel)
                    for (int s2 = 0; s2 < 2; s2++) {
                        float sd = s2 ? 1.0f : -1.0f;
                        skE[s2].assign(m, 0.0f); skX[s2].resize(m); skZ[s2].resize(m); skFx[s2].resize(m); skFz[s2].resize(m);
                        for (int k = 0; k < m; k++) {
                            edgeXZ(k, sd, Wd, skX[s2][k], skZ[s2][k]);
                            float up = ry[k] - H(skX[s2][k], skZ[s2][k]);
                            skE[s2][k] = up > 0.35f ? std::min(up * 1.6f, 14.0f) : 0.0f;
                        }
                        for (int pass = 0; pass < 3; pass++) {
                            std::vector<float> t = skE[s2];
                            for (int k = 1; k + 1 < m; k++) skE[s2][k] = 0.25f * t[k - 1] + 0.5f * t[k] + 0.25f * t[k + 1];
                        }
                        for (int k = 0; k < m; k++) {
                            float dx = skX[s2][k] - rx[k], dz = skZ[s2][k] - rz[k], dl = std::max(1e-3f, std::hypot(dx, dz));
                            skFx[s2][k] = skX[s2][k] + dx / dl * skE[s2][k]; skFz[s2][k] = skZ[s2][k] + dz / dl * skE[s2][k];
                        }
                    }
                float nextPier = 10.0f;
                for (int k = 0; k + 1 < m; k++) {
                    float x0 = rx[k], z0 = rz[k], x1 = rx[k + 1], z1 = rz[k + 1];
                    float tx = x1 - x0, tz = z1 - z0, tl = std::hypot(tx, tz);
                    if (tl < 0.05f || !inside((x0 + x1) * 0.5f, (z0 + z1) * 0.5f)) continue;
                    tx /= tl; tz /= tl;
                    float nx = -tz, nz = tx, mx = (x0 + x1) * 0.5f, mz = (z0 + z1) * 0.5f;
                    float ya = ry[k], yb = ry[k + 1], ym = (ya + yb) * 0.5f;
                    float gc = H(mx, mz), gl = H(mx - nx * Wd, mz - nz * Wd), gr = H(mx + nx * Wd, mz + nz * Wd);
                    float lx0, lz0, lx1, lz1, rx0, rz0, rx1, rz1;
                    edgeXZ(k, -1, Wd, lx0, lz0); edgeXZ(k + 1, -1, Wd, lx1, lz1);
                    edgeXZ(k, 1, Wd, rx0, rz0); edgeXZ(k + 1, 1, Wd, rx1, rz1);
                    if (G.tunnel) { // Roehre: Waende und Decke folgen der Neigung; die Decke liegt unter dem Gelaende, am Portal ragt sie als Galerie heraus
                        float c0 = std::max(ya + 3.4f, std::min(ya + 5.4f, H(x0, z0) - 0.5f)), c1 = std::max(yb + 3.4f, std::min(yb + 5.4f, H(x1, z1) - 0.5f));
                        pushQuadN(r.bldV, P(lx0, ya, lz0), P(lx1, yb, lz1), P(lx1, c1, lz1), P(lx0, c0, lz0), Vec3{nx, 0, nz}, tunC);
                        pushQuadN(r.bldV, P(rx0, ya, rz0), P(rx1, yb, rz1), P(rx1, c1, rz1), P(rx0, c0, rz0), Vec3{-nx, 0, -nz}, tunC);
                        pushQuadN(r.bldV, P(lx0, c0, lz0), P(lx1, c1, lz1), P(rx1, c1, rz1), P(rx0, c0, rz0), Vec3{0, -1, 0}, concD);
                        pushQuadN(r.bldV, P(lx0, c0 + 0.4f, lz0), P(lx1, c1 + 0.4f, lz1), P(rx1, c1 + 0.4f, rz1), P(rx0, c0 + 0.4f, rz0), Vec3{0, 1, 0}, tunC);
                        pushQuadN(r.bldV, P(lx0 - nx * 0.4f, ya, lz0 - nz * 0.4f), P(lx1 - nx * 0.4f, yb, lz1 - nz * 0.4f), P(lx1 - nx * 0.4f, c1 + 0.4f, lz1 - nz * 0.4f), P(lx0 - nx * 0.4f, c0 + 0.4f, lz0 - nz * 0.4f), Vec3{-nx, 0, -nz}, tunC);
                        pushQuadN(r.bldV, P(rx0 + nx * 0.4f, ya, rz0 + nz * 0.4f), P(rx1 + nx * 0.4f, yb, rz1 + nz * 0.4f), P(rx1 + nx * 0.4f, c1 + 0.4f, rz1 + nz * 0.4f), P(rx0 + nx * 0.4f, c0 + 0.4f, rz0 + nz * 0.4f), Vec3{nx, 0, nz}, tunC);
                        continue;
                    }
                    float clear = ym - std::min(gc, std::min(gl, gr));
                    if (G.bridge && clear > 2.6f) { // freie Spannweite: geneigte Platte (1,1 m), Pfeiler alle ~22 m
                        const float T = 1.1f;
                        pushQuadN(r.bldV, P(lx0, ya - 0.03f, lz0), P(lx1, yb - 0.03f, lz1), P(lx1, yb - T, lz1), P(lx0, ya - T, lz0), Vec3{-nx, 0, -nz}, conc);
                        pushQuadN(r.bldV, P(rx0, ya - 0.03f, rz0), P(rx1, yb - 0.03f, rz1), P(rx1, yb - T, rz1), P(rx0, ya - T, rz0), Vec3{nx, 0, nz}, conc);
                        pushQuadN(r.bldV, P(lx0, ya - T, lz0), P(lx1, yb - T, lz1), P(rx1, yb - T, rz1), P(rx0, ya - T, rz0), Vec3{0, -1, 0}, concD);
                        if (rd[k] >= nextPier && clear > 4.5f) {
                            float pw = clampv(Wd * 0.45f, 0.9f, 2.6f);
                            // nie auf eine Strasse unter der Bruecke stellen
                            bool onRoadBelow = false;
                            int la = std::max(1, w.layer);
                            for (size_t wj = 0; wj < ways.size() && !onRoadBelow; wj++) {
                                if (wj == wi) continue;
                                const WayTmp& o = ways[wj];
                                if ((o.bridge ? std::max(1, o.layer) : o.layer) >= la) continue;
                                const WayGeo& O = wg[wj];
                                float reach = o.hw + (o.flags & 8 ? 1.9f : 0.0f) + 1.5f + pw;
                                for (size_t j = 0; j + 1 < O.rx.size(); j++) {
                                    float ax = O.rx[j], az = O.rz[j], bx = O.rx[j + 1], bz = O.rz[j + 1];
                                    if (mx < std::min(ax, bx) - reach || mx > std::max(ax, bx) + reach || mz < std::min(az, bz) - reach || mz > std::max(az, bz) + reach) continue;
                                    float vx = bx - ax, vz = bz - az, l2 = vx * vx + vz * vz;
                                    float tt = l2 > 1e-6f ? clampv(((mx - ax) * vx + (mz - az) * vz) / l2, 0.0f, 1.0f) : 0.0f;
                                    if (std::hypot(mx - (ax + vx * tt), mz - (az + vz * tt)) < reach) { onRoadBelow = true; break; }
                                }
                            }
                            if (onRoadBelow) nextPier = rd[k] + 5.0f;
                            else {
                                nextPier = rd[k] + 22.0f;
                                float top = ym - T, bot = gc - 0.4f;
                                pushBox(r.bldV, {mx, (top + bot) * 0.5f, mz}, {0.7f, std::max(0.1f, (top - bot) * 0.5f), pw}, tx, tz, concD);
                            }
                        }
                    } else { // Rampe / Widerlager: aufgeschuettete Boeschung (1:1,6) bis zum Gelaende
                        for (int s2 = 0; s2 < 2; s2++) {
                            if ((int)skE[s2].size() != m || (skE[s2][k] < 0.4f && skE[s2][k + 1] < 0.4f)) continue;
                            float sd = s2 ? 1.0f : -1.0f;
                            float f0x = skFx[s2][k], f0z = skFz[s2][k], f1x = skFx[s2][k + 1], f1z = skFz[s2][k + 1];
                            if (onAnyRoad((f0x + f1x) * 0.5f, (f0z + f1z) * 0.5f, 0.3f)) continue;   // dort liegt eine andere Fahrbahn (Abzweig, Parallelfahrbahn)
                            pushQuadN(r.bldV, P(skX[s2][k], ya - 0.03f, skZ[s2][k]), P(skX[s2][k + 1], yb - 0.03f, skZ[s2][k + 1]),
                                      P(f1x, H(f1x, f1z) - 0.05f, f1z), P(f0x, H(f0x, f0z) - 0.05f, f0z), Vec3{nx * sd, 0.6f, nz * sd}, fillC);
                        }
                    }
                    if (G.bridge) { // Gelaender (0,9 m hoch, 0,3 m dick), folgt der Fahrbahn
                        for (int sd = -1; sd <= 1; sd += 2) {
                            float ox0, oz0, ox1, oz1, ix0, iz0, ix1, iz1;
                            edgeXZ(k, (float)sd, Wd, ox0, oz0); edgeXZ(k + 1, (float)sd, Wd, ox1, oz1);
                            edgeXZ(k, (float)sd, Wd - 0.3f, ix0, iz0); edgeXZ(k + 1, (float)sd, Wd - 0.3f, ix1, iz1);
                            const float hgt = 0.9f;
                            pushQuadN(r.bldV, P(ox0, ya, oz0), P(ox1, yb, oz1), P(ox1, yb + hgt, oz1), P(ox0, ya + hgt, oz0), Vec3{nx * sd, 0, nz * sd}, concD);
                            pushQuadN(r.bldV, P(ix0, ya, iz0), P(ix1, yb, iz1), P(ix1, yb + hgt, iz1), P(ix0, ya + hgt, iz0), Vec3{-nx * sd, 0, -nz * sd}, concD);
                            pushQuadN(r.bldV, P(ix0, ya + hgt, iz0), P(ix1, yb + hgt, iz1), P(ox1, yb + hgt, oz1), P(ox0, ya + hgt, oz0), Vec3{0, 1, 0}, conc);
                        }
                    }
                }
            }
            // ----- Spurpfeile aus turn:lanes vor Kreuzungen (zweimal: 8 m und 22 m vor der Haltelinie) -----
            if (!w.ring && w.klass <= 5 && (int)w.nodes.size() == n && w.lanes >= 1 && m >= 3 && (!w.turnF.empty() || !w.turnB.empty())) {
                auto laneArrows = [&](const std::string& spec, bool fwdDir) {
                    std::vector<std::string> tok;
                    { std::string cur; for (char ch : spec) { if (ch == '|') { tok.push_back(cur); cur.clear(); } else cur += ch; } tok.push_back(cur); }
                    int nl = (int)tok.size();
                    if (nl < 1 || nl > w.lanes || junction[w.nodes[fwdDir ? n - 1 : 0]] < 3) return;
                    float L = rd[m - 1];
                    if (L < 26.0f) return;
                    float lw = 2.0f * w.hw / (float)w.lanes, sc = clampv(lw / 3.3f, 0.7f, 1.1f);
                    int first = fwdDir ? w.lanesBack : 0;   // links der Wegrichtung liegen zuerst die Gegenrichtungsspuren
                    for (float back : {8.0f, 22.0f}) {
                        if (back > L - 4.0f) continue;
                        float dp = fwdDir ? L - back : back;
                        int kk = 0;
                        while (kk + 2 < m && rd[kk + 1] < dp) kk++;
                        float sg = rd[kk + 1] - rd[kk], tt = sg > 1e-4f ? clampv((dp - rd[kk]) / sg, 0.0f, 1.0f) : 0.0f;
                        float px = rx[kk] + (rx[kk + 1] - rx[kk]) * tt, pz = rz[kk] + (rz[kk + 1] - rz[kk]) * tt;
                        float dx = rx[kk + 1] - rx[kk], dz = rz[kk + 1] - rz[kk], dl = std::hypot(dx, dz);
                        if (dl < 1e-4f) continue;
                        dx /= dl; dz /= dl;
                        float fx = fwdDir ? dx : -dx, fz = fwdDir ? dz : -dz, nx = -dz, nz = dx;   // Fahrtrichtung, rechts der Wegrichtung
                        for (int j = 0; j < nl; j++) {
                            const std::string& t = tok[j];
                            bool thru = t.find("through") != std::string::npos;
                            bool lf = t.find("left") != std::string::npos && t.find("merge") == std::string::npos;
                            bool rt = t.find("right") != std::string::npos && t.find("merge") == std::string::npos;
                            if (!thru && !lf && !rt) continue;
                            int idx = fwdDir ? first + j : (nl - 1 - j);
                            if (idx < 0 || idx >= w.lanes) continue;
                            float lat = -w.hw + ((float)idx + 0.5f) * lw, cxp = px + nx * lat, czp = pz + nz * lat;
                            float yb = ry.empty() ? H(cxp, czp) : ry[kk];
                            arrowGlyph(asphV, cxp, czp, yb + 0.045f, fx, fz, thru, lf, rt, sc);
                        }
                    }
                };
                if (!w.turnF.empty()) laneArrows(w.turnF, true);
                if (!w.turnB.empty()) laneArrows(w.turnB, false);
            }
            // ----- Fussgaengeruebergaenge: Zebrastreifen auf der Fahrbahn, blaues Hinweisschild je Fahrtrichtung -----
            if (w.klass >= 2 && w.klass <= 6 && !w.ring && (int)w.nodes.size() == n && m >= 2 && !crossings.empty()) {
                for (int i = 0; i < n; i++) {
                    auto itc = crossings.find(w.nodes[i]);
                    if (itc == crossings.end() || !itc->second || !inside(w.px[i], w.pz[i])) continue;
                    int q = 0;
                    { float bd = 1e18f; for (int k = 0; k < m; k++) { float dd = (rx[k] - w.px[i]) * (rx[k] - w.px[i]) + (rz[k] - w.pz[i]) * (rz[k] - w.pz[i]); if (dd < bd) { bd = dd; q = k; } } }
                    int q0 = std::max(0, q - 1), q1 = std::min(m - 1, q + 1);
                    float dx = rx[q1] - rx[q0], dz = rz[q1] - rz[q0], dl = std::hypot(dx, dz);
                    if (dl < 1e-3f) continue;
                    dx /= dl; dz /= dl;
                    float nx = -dz, nz = dx, px = rx[q], pz = rz[q];
                    for (float lat = -w.hw + 0.55f; lat <= w.hw - 0.5f; lat += 1.0f) {
                        float cxp = px + nx * lat, czp = pz + nz * lat;
                        float y = (ry.empty() ? H(cxp, czp) : ry[q]) + 0.045f;
                        markBar(asphV, cxp - dx * 1.6f, czp - dz * 1.6f, cxp + dx * 1.6f, czp + dz * 1.6f, 0.25f, y);
                    }
                    for (int sgn = -1; sgn <= 1; sgn += 2) {
                        if ((w.oneway == 1 && sgn < 0) || (w.oneway == -1 && sgn > 0) || (int)r.signs.size() > 900) continue;
                        float tx = dx * sgn, tz = dz * sgn;   // Fahrtrichtung
                        float off = w.hw + ((w.flags & 8) ? 1.9f : 0.3f) + 0.7f;
                        SignDef sd;
                        sd.kind = 10; sd.tx = tx; sd.tz = tz; sd.hw = w.hw;
                        float bx = px - tx * 2.4f, bz = pz - tz * 2.4f;
                        sd.pos = {bx - tz * off, ry.empty() ? H(bx - tz * off, bz + tx * off) : ry[q], bz + tx * off};
                        r.signs.push_back(sd);
                    }
                }
            }
            // ----- Strassenbaeume und Laternen entlang der Gehsteige -----
            if (w.klass >= 2 && w.klass <= 5 && !w.ring && m >= 2) {
                float nextTree = 8.0f + hash2((int)(w.id & 0xFFFF), 3) * 20.0f, nextLamp = 12.0f + hash2((int)(w.id & 0xFFFF), 7) * 30.0f;
                int sideT = (int)(w.id & 1) ? 1 : -1, sideL = -sideT;
                for (int k = 0; k + 1 < m; k++) {
                    float tx = rx[k + 1] - rx[k], tz = rz[k + 1] - rz[k], tl = std::hypot(tx, tz);
                    if (tl < 0.01f) continue;
                    tx /= tl; tz /= tl;
                    float nx = -tz, nz = tx;
                    while (rd[k] >= nextTree && (hasSw || w.klass >= 4) && ry.empty()) {
                        float off = hwq + (hasSw ? 1.0f : 3.2f);
                        float px2 = rx[k] + nx * off * sideT, pz2 = rz[k] + nz * off * sideT;
                        nextTree += (w.klass >= 4 ? 17.0f : 26.0f) + hash2((int)(px2 * 2), (int)(pz2 * 2)) * 8.0f;
                        sideT = -sideT;
                        if (!inside(px2, pz2) || treeCount > 1600 || onAnyRoad(px2, pz2, 1.0f)) continue;
                        bool inB = false;
                        for (const Building& bb : r.buildings) { if (px2 < bb.minx - 1 || px2 > bb.maxx + 1 || pz2 < bb.minz - 1 || pz2 > bb.maxz + 1) continue; if (pointInPoly(bb.poly, px2, pz2)) { inB = true; break; } }
                        if (inB) continue;
                        if (w.klass <= 3 && hash2((int)(px2), (int)(pz2)) < 0.55f) continue; // grosse Strassen: weniger Baeume
                        r.trees.push_back({px2, H(px2, pz2), pz2, hash2((int)(px2 * 5), (int)(pz2 * 5))});
                        treeCount++;
                    }
                    while (rd[k] >= nextLamp) {
                        float off = hwq + (hasSw ? 0.7f : 1.2f);
                        float sg = (float)sideL;
                        float px2 = rx[k] + nx * off * sg, pz2 = rz[k] + nz * off * sg;
                        nextLamp += 38.0f;
                        sideL = -sideL;
                        if (!inside(px2, pz2) || lampCount > 1200 || onAnyRoad(px2, pz2, 0.4f)) continue;
                        pushLamp(r.bldV, px2, ry.empty() ? H(px2, pz2) : ry[k], pz2, -nx * sg, -nz * sg);
                        lampCount++;
                    }
                }
            }
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
                    if (!ry.empty()) { float bd = 1e18f; for (int q = 0; q < m; q++) { float dd = (rx[q] - c.pos.x) * (rx[q] - c.pos.x) + (rz[q] - c.pos.z) * (rz[q] - c.pos.z); if (dd < bd) { bd = dd; c.pos.y = ry[q]; } } }
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
        for (const Deck& dk : decks) { // Bahnbruecke ueber der Unterfuehrung: Betonplatte mit Schotterbett und zwei Schienen
            pushBox(r.bldV, {dk.x, dk.top - 0.45f, dk.z}, {dk.len, 0.5f, 2.6f}, dk.tx, dk.tz, Color{142, 140, 134, 0});
            pushBox(r.bldV, {dk.x, dk.top + 0.10f, dk.z}, {dk.len, 0.06f, 1.7f}, dk.tx, dk.tz, Color{96, 88, 80, 0});
            for (float sd : {-0.72f, 0.72f}) pushBox(r.bldV, {dk.x - dk.tz * sd, dk.top + 0.20f, dk.z + dk.tx * sd}, {dk.len, 0.05f, 0.05f}, dk.tx, dk.tz, Color{60, 58, 56, 0});
            for (float sd : {-1.0f, 1.0f}) pushBox(r.bldV, {dk.x - dk.tz * sd * 2.5f, dk.top + 0.45f, dk.z + dk.tx * sd * 2.5f}, {dk.len, 0.5f, 0.08f}, dk.tx, dk.tz, Color{110, 112, 116, 0});
        }
        { // ----- Verkehrsschilder: Tempolimit bei Aenderung, Autobahn-Ausfahrten mit Wegweiserbruecke (Wechselanzeige) und Hinweistafel -----
            // Punkt auf dem Weg im Abstand dist vom Wegende; tx/tz = Richtung vom Ende weg
            auto onWay = [&](int wi, bool atStart, float dist, float& px, float& pz, float& tx, float& tz, float& py) {
                const WayGeo& G = wg[wi];
                int m = (int)G.rx.size();
                if (m < 2 || G.rd[m - 1] < dist + 1.0f) return false;
                float s = atStart ? dist : G.rd[m - 1] - dist;
                int k = 0;
                while (k + 2 < m && G.rd[k + 1] < s) k++;
                float sg = G.rd[k + 1] - G.rd[k], t = sg > 1e-4f ? clampv((s - G.rd[k]) / sg, 0.0f, 1.0f) : 0.0f;
                px = G.rx[k] + (G.rx[k + 1] - G.rx[k]) * t; pz = G.rz[k] + (G.rz[k + 1] - G.rz[k]) * t;
                float dx = G.rx[k + 1] - G.rx[k], dz = G.rz[k + 1] - G.rz[k], dl = std::hypot(dx, dz);
                if (dl < 1e-4f) return false;
                dx /= dl; dz /= dl;
                tx = atStart ? dx : -dx; tz = atStart ? dz : -dz;
                py = G.ry.empty() ? H(px, pz) : G.ry[k];
                return true;
            };
            // (a) Tempolimit-Schilder: am Anfang eines Abschnitts mit anderem, ausdruecklich getaggtem Tempo (rechts der Fahrtrichtung)
            for (auto& kv : endsAt) {
                if (kv.second.size() != 2 || (int)r.signs.size() > 400) continue;
                for (int e = 0; e < 2; e++) {
                    int wi = kv.second[e], oi = kv.second[1 - e];
                    if (wi == oi) continue;
                    const WayTmp& W = ways[wi];
                    const WayTmp& O = ways[oi];
                    if (W.ring || O.ring || W.klass > 5 || !W.speedTag || W.maxspeed < 10 || W.maxspeed > 130 || std::fabs(W.maxspeed - O.maxspeed) < 1.0f) continue;
                    bool atStart = W.nodes.front() == kv.first;
                    if (atStart ? W.oneway == -1 : W.oneway == 1) continue;   // dort faehrt niemand in W hinein
                    float px, pz, tx, tz, py;
                    if (!onWay(wi, atStart, 6.0f, px, pz, tx, tz, py)) continue;
                    float off = W.hw + ((W.flags & 8) ? 1.9f : 0.3f) + 0.9f;
                    SignDef s;
                    s.kind = 0; s.value = (int)W.maxspeed; s.tx = tx; s.tz = tz; s.hw = W.hw;
                    s.pos = {px - tz * off, py, pz + tx * off};
                    if (inside(s.pos.x, s.pos.z)) r.signs.push_back(s);
                }
            }
            // Wege je Knoten (fuer Ziele der Ausfahrten, Autobahnbeginn)
            std::unordered_map<int64_t, std::vector<int>> nodeWays;
            for (int wi = 0; wi < (int)ways.size(); wi++) {
                const WayTmp& W = ways[wi];
                if (W.klass > 5 || W.nodes.size() != W.px.size()) continue;
                for (int64_t id : W.nodes) nodeWays[id].push_back(wi);
            }
            auto splitDest = [](const std::string& d, std::vector<std::string>& out) {
                std::string cur;
                auto flush = [&]() { size_t a = cur.find_first_not_of(' '), b = cur.find_last_not_of(' '); if (a != std::string::npos) out.push_back(cur.substr(a, b - a + 1)); cur.clear(); };
                for (char ch : d) { if (ch == ';') flush(); else cur += ch; }
                flush();
            };
            auto labelOf = [&](const WayTmp& W) {
                std::string nm = (W.name >= 0 && W.name < (int)r.names.size()) ? r.names[W.name] : "";
                if (!W.ref.empty() && nm != W.ref) return nm.empty() ? W.ref : W.ref + " " + nm;
                return nm;
            };
            // Vom Punkt (Weg, Index) entlang der Richtungsfahrbahn zurueckgehen, auch ueber Weggrenzen hinweg; liefert die erreichte Strecke
            auto walkBack = [&](int mi, int kk, float dist, int& wo, int& ko) {
                float done = 0;
                int cw = mi, ck = kk;
                for (int guard = 0; guard < 60; guard++) {
                    const WayGeo& G = wg[cw];
                    float avail = G.rd[ck];
                    if (done + avail >= dist) {
                        float target = G.rd[ck] - (dist - done);
                        int k = ck;
                        while (k > 0 && G.rd[k] > target) k--;
                        wo = cw; ko = k;
                        return dist;
                    }
                    done += avail;
                    int64_t N0 = ways[cw].nodes.front();
                    int pred = -1;
                    auto it = endsAt.find(N0);
                    if (it != endsAt.end())
                        for (int o : it->second) {
                            const WayTmp& O = ways[o];
                            if (o == cw || O.link || O.klass != ways[cw].klass || O.oneway != 1 || O.nodes.back() != N0 || wg[o].rx.size() < 2) continue;
                            pred = o;
                            break;
                        }
                    if (pred < 0) { wo = cw; ko = 0; return done; }
                    cw = pred; ck = (int)wg[pred].rx.size() - 1;
                }
                wo = cw; ko = ck;
                return done;
            };
            auto poseAt = [&](int wi, int k, Vec3& pos, float& tx, float& tz) {
                const WayGeo& G = wg[wi];
                int m = (int)G.rx.size(), k1 = std::min(k + 1, m - 1), k0 = k1 > k ? k : std::max(0, k - 1);
                float dx = G.rx[k1] - G.rx[k0], dz = G.rz[k1] - G.rz[k0], dl = std::max(1e-4f, std::hypot(dx, dz));
                tx = dx / dl; tz = dz / dl;
                pos = {G.rx[k], G.ry.empty() ? H(G.rx[k], G.rz[k]) : G.ry[k], G.rz[k]};
            };
            // (b) Jede Ausfahrt / jeder Knoten an Autobahnen und Schnellstrassen: Vorwegweiser (1000 m), Wegweiserbruecke mit Wechselanzeige (450 m),
            //     Baken (300/200/100 m), Ausfahrtstafel an der Rampe. Ziele: destination-Tag, sonst Name/Nummer der Strasse, zu der die Rampe fuehrt.
            for (int li = 0; li < (int)ways.size(); li++) {
                const WayTmp& L = ways[li];
                if (!L.link || L.klass > 1 || L.oneway == -1 || L.nodes.size() != L.px.size() || L.nodes.size() < 2 || wg[li].rx.size() < 2) continue;
                int64_t N = L.nodes.front();
                if (!inside(L.px[0], L.pz[0])) continue;   // nur die Kachel mit dem Abzweig erzeugt die Schilder (keine Doppelten)
                auto nw = nodeWays.find(N);
                if (nw == nodeWays.end()) continue;
                int mi = -1, idx = -1;
                for (int cand : nw->second) {
                    const WayTmp& M = ways[cand];
                    if (cand == li || M.link || M.klass > 1 || M.oneway != 1 || wg[cand].rx.size() < 2) continue;
                    int q = -1;
                    for (size_t t = 1; t < M.nodes.size(); t++) if (M.nodes[t] == N) { q = (int)t; break; }
                    if (q < 0) continue;
                    mi = cand; idx = q;
                    break;
                }
                if (mi < 0) continue;
                const WayTmp& M = ways[mi];
                const WayGeo& G = wg[mi];
                int m = (int)G.rx.size(), kk = 0;
                { float bd = 1e18f; for (int k = 0; k < m; k++) { float dd = (G.rx[k] - M.px[idx]) * (G.rx[k] - M.px[idx]) + (G.rz[k] - M.pz[idx]) * (G.rz[k] - M.pz[idx]); if (dd < bd) { bd = dd; kk = k; } } }
                // Ziele bestimmen
                SignDef base;
                base.hw = M.hw; base.lanes = std::max(1, M.lanes); base.value = (int)M.maxspeed;
                base.seed = (uint32_t)(M.id * 2654435761ull) ^ (uint32_t)(L.id * 40503ull);
                base.ref = L.jref;
                { auto jn = jnodes.find(N); if (jn != jnodes.end()) { base.head = jn->second.first; if (base.ref.empty()) base.ref = jn->second.second; } }
                std::vector<std::string> dests;
                splitDest(L.dest, dests);
                bool toMotorway = false;
                { // der Rampe folgen bis zur naechsten benannten Strasse
                    int cur = li;
                    for (int step = 0; step < 10; step++) {
                        int64_t E = ways[cur].nodes.back();
                        auto ew = nodeWays.find(E);
                        if (ew == nodeWays.end()) break;
                        int next = -1;
                        std::string lab;
                        for (int o : ew->second) {
                            const WayTmp& O = ways[o];
                            if (o == cur) continue;
                            if (!O.link) { std::string l = labelOf(O); if (!l.empty() && lab.empty()) { lab = l; toMotorway = O.klass <= 1; } }
                            else if (O.nodes.front() == E && next < 0) next = o;
                        }
                        if (!lab.empty()) { if (dests.empty()) dests.push_back(lab); else if (toMotorway && dests.size() < 3 && lab.size() < 6) dests.insert(dests.begin(), lab); break; }
                        if (next < 0) break;
                        cur = next;
                        if (dests.empty()) splitDest(ways[cur].dest, dests);
                    }
                }
                if (dests.size() > 0) base.l1 = dests[0];
                if (dests.size() > 1) base.l2 = dests[1];
                if (dests.size() > 2) base.l3 = dests[2];
                if (base.head.empty() && toMotorway) base.head = "Knoten";
                { std::vector<std::string> td; splitDest(M.dest, td); base.thru = M.ref; if (!td.empty()) base.thru += (base.thru.empty() ? "" : "  ") + td[0]; }
                auto place = [&](float back, int kind, int value, float side) {
                    int wo, ko;
                    float got = walkBack(mi, kk, back, wo, ko);
                    if (got < back - 1.0f) return false;
                    SignDef g = base;
                    g.kind = kind;
                    if (value >= 0) g.value = value;
                    poseAt(wo, ko, g.pos, g.tx, g.tz);
                    g.hw = ways[wo].hw; g.lanes = std::max(1, ways[wo].lanes);
                    if (side != 0) { float off = (g.hw + side); g.pos.x += -g.tz * off; g.pos.z += g.tx * off; if (wg[wo].ry.empty()) g.pos.y = H(g.pos.x, g.pos.z); }
                    r.signs.push_back(g);
                    return true;
                };
                if (!place(1000.0f, 12, 1000, 2.6f)) place(700.0f, 12, 700, 2.6f);
                if (!place(450.0f, 1, -1, 0)) if (!place(300.0f, 1, -1, 0)) place(160.0f, 1, -1, 0);
                place(300.0f, 11, 3, 1.3f); place(200.0f, 11, 2, 1.3f); place(100.0f, 11, 1, 1.3f);
                { // Ausfahrtstafel rechts neben der Rampe, 28 m nach dem Abzweig
                    const WayGeo& LG = wg[li];
                    int lm = (int)LG.rx.size(), k = 0;
                    while (k + 1 < lm - 1 && LG.rd[k] < 28.0f) k++;
                    SignDef p = base;
                    p.kind = 2;
                    poseAt(li, k, p.pos, p.tx, p.tz);
                    float off = L.hw + 1.6f;
                    p.pos.x += -p.tz * off; p.pos.z += p.tx * off;
                    if (LG.ry.empty()) p.pos.y = H(p.pos.x, p.pos.z);
                    r.signs.push_back(p);
                }
            }
            // (c) Kreisverkehr: an jeder Einfahrt blaues Rundschild mit Vorrang geben
            {
                std::unordered_map<int64_t, bool> ringNode;
                for (const WayTmp& W : ways) if (W.ring && W.nodes.size() == W.px.size()) for (int64_t id : W.nodes) ringNode[id] = true;
                if (!ringNode.empty())
                    for (int wi = 0; wi < (int)ways.size(); wi++) {
                        const WayTmp& W = ways[wi];
                        if (W.ring || W.klass > 5 || W.nodes.size() != W.px.size() || W.nodes.size() < 2 || (int)r.signs.size() > 900) continue;
                        for (int e = 0; e < 2; e++) {
                            bool atStart = e == 0;
                            int64_t nd = atStart ? W.nodes.front() : W.nodes.back();
                            if (!ringNode.count(nd)) continue;
                            if (atStart ? W.oneway == 1 : W.oneway == -1) continue;   // hier faehrt man aus dem Kreis heraus
                            float px, pz, tx, tz, py;
                            if (!onWay(wi, atStart, 7.0f, px, pz, tx, tz, py)) continue;
                            SignDef sd;
                            sd.kind = 7; sd.tx = -tx; sd.tz = -tz; sd.hw = W.hw;
                            float off = W.hw + ((W.flags & 8) ? 1.9f : 0.3f) + 0.8f;
                            sd.pos = {px - sd.tz * off, py, pz + sd.tx * off};
                            if (inside(sd.pos.x, sd.pos.z)) r.signs.push_back(sd);
                        }
                    }
            }
            // (d) Einbahnstrassen: Pfeilschild an der Einfahrt, "Einfahrt verboten" am anderen Ende (nur an echten Kreuzungen, nicht an Fortsetzungen)
            for (int wi = 0; wi < (int)ways.size(); wi++) {
                const WayTmp& W = ways[wi];
                if (W.oneway == 0 || W.ring || W.klass > 5 || W.klass < 1 || W.nodes.size() != W.px.size() || W.nodes.size() < 2 || (int)r.signs.size() > 900) continue;
                if (W.link && W.klass > 1) continue;
                const WayGeo& G = wg[wi];
                if (G.rx.size() < 2 || G.rd.back() < 30.0f) continue;
                for (int e = 0; e < 2; e++) {
                    bool atStart = e == 0;
                    bool entry = atStart ? W.oneway == 1 : W.oneway == -1;
                    int64_t nd = atStart ? W.nodes.front() : W.nodes.back();
                    if (junction[nd] < 3) continue;
                    bool cont = false;   // Fortsetzung derselben Einbahn / Richtungsfahrbahn?
                    auto it = endsAt.find(nd);
                    if (it != endsAt.end()) for (int o : it->second) if (o != wi && ways[o].oneway != 0 && (ways[o].name == W.name || ways[o].klass <= 1) && ways[o].klass == W.klass) cont = true;
                    if (cont) continue;
                    float px, pz, tx, tz, py;
                    if (!onWay(wi, atStart, entry ? 5.0f : 3.0f, px, pz, tx, tz, py)) continue;
                    SignDef sd;
                    sd.hw = W.hw;
                    float off = W.hw + ((W.flags & 8) ? 1.9f : 0.3f) + 0.8f;
                    if (entry) { if (W.klass < 4 || W.link) continue; sd.kind = 5; sd.tx = tx; sd.tz = tz; }
                    else { if (W.klass < 2 && !W.link) continue; sd.kind = 6; sd.tx = tx; sd.tz = tz; }   // zeigt dem, der falsch einfahren wuerde, entgegen
                    sd.pos = {px - sd.tz * off, py, pz + sd.tx * off};
                    if (inside(sd.pos.x, sd.pos.z)) r.signs.push_back(sd);
                }
            }
            // (e) Autobahnbeginn: Auffahrt, die von einer normalen Strasse abzweigt
            for (int li = 0; li < (int)ways.size(); li++) {
                const WayTmp& L = ways[li];
                if (!L.link || L.klass != 0 || L.oneway == -1 || L.nodes.size() != L.px.size() || L.nodes.size() < 2 || wg[li].rx.size() < 2) continue;
                auto nw = nodeWays.find(L.nodes.front());
                if (nw == nodeWays.end()) continue;
                bool fromRoad = false;
                for (int o : nw->second) if (o != li && ways[o].klass >= 2) fromRoad = true;
                if (!fromRoad) continue;
                float px, pz, tx, tz, py;
                if (!onWay(li, true, 14.0f, px, pz, tx, tz, py)) continue;
                SignDef sd;
                sd.kind = 8; sd.tx = tx; sd.tz = tz; sd.hw = L.hw;
                float off = L.hw + 1.4f;
                sd.pos = {px - tz * off, py, pz + tx * off};
                if (inside(sd.pos.x, sd.pos.z)) r.signs.push_back(sd);
            }
        }
        if (!r.signs.empty()) {
            int cnt[16] = {};
            for (const SignDef& s : r.signs) cnt[clampv(s.kind, 0, 15)]++;
            logMsg("Schilder in Kachel %d/%d: %d Tempolimit, %d Wegweiserbruecken, %d Ausfahrtstafeln, %d Vorwegweiser, %d Baken, %d Einbahn, %d Einfahrt verboten, %d Kreisverkehr, %d Autobahn, %d Zebrastreifen-Schilder",
                   cx, cy, cnt[0], cnt[1], cnt[2], cnt[12], cnt[11], cnt[5], cnt[6], cnt[7], cnt[8], cnt[10]);
            for (const SignDef& s : r.signs)
                if (s.kind == 0) {
                    logMsg("  Tempolimit %d bei %.5f,%.5f Richtung %.3f,%.3f", s.value, myToLat(g.my0 + s.pos.z / g.S), (g.mx0 + s.pos.x / g.S) * 360.0 - 180.0, s.tx, s.tz);
                    break;
                }
            for (const SignDef& s : r.signs)
                if (s.kind == 1) {
                    logMsg("  Wegweiserbruecke bei %.5f,%.5f Richtung %.3f,%.3f: %s | %s | %s (Ausfahrt %s, %d Spuren)", myToLat(g.my0 + s.pos.z / g.S), (g.mx0 + s.pos.x / g.S) * 360.0 - 180.0,
                           s.tx, s.tz, s.l1.c_str(), s.l2.c_str(), s.l3.c_str(), s.ref.c_str(), s.lanes);
                    break;
                }
        }
        { // Kreuzungen: Asphaltscheibe ueber den Strassenenden (saubere Einmuendungen, keine Linien quer durch die Kreuzung)
            for (auto& kv : jmap) {
                float Rj = std::min(24.0f, kv.second.hw * 1.2f + 0.7f);
                Color jc{1, 0, 3, (uint8_t)std::min(255.0f, Rj * 10.0f)};
                const int NJ = 20;
                float jy = kv.second.y;   // Kreuzung auf Bruecke/Rampe: ebene Scheibe auf Fahrbahnhoehe
                auto JV = [&](float x, float z, float lat) { asphV.push_back({{x, (jy > -1e8f ? jy : H(x, z)) + 0.035f, z}, {0, 1, 0}, {lat, 0}, jc}); };
                for (int i = 0; i < NJ; i++) {
                    float a0 = i * 2 * PI / NJ, a1 = (i + 1) * 2 * PI / NJ;
                    if (!inside(kv.second.x, kv.second.z)) break;
                    JV(kv.second.x, kv.second.z, 0);
                    JV(kv.second.x + std::cos(a0) * Rj, kv.second.z + std::sin(a0) * Rj, Rj);
                    JV(kv.second.x + std::cos(a1) * Rj, kv.second.z + std::sin(a1) * Rj, Rj);
                }
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
        bucketCells(deckV, r.x0, r.z0, r.x1, r.z1, r.deckCells);
        bucketCells(r.bldV, r.x0, r.z0, r.x1, r.z1, r.bldCells);
        std::vector<uint8_t> roadMask;
        { // Strassen aus dem Wasser aussparen (Bruecken und Tunnel ausgenommen: dort liegt die Fahrbahn ueber bzw. unter dem Wasser)
            const float CSm = 4.0f;
            int wnx = (int)std::ceil((r.x1 - r.x0) / CSm), wnz = (int)std::ceil((r.z1 - r.z0) / CSm);
            if (wnx >= 1 && wnz >= 1 && wnx <= 1000 && wnz <= 1000) {
                roadMask.assign((size_t)wnx * wnz, 0);
                for (size_t wi = 0; wi < ways.size(); wi++) {
                    const WayGeo& G = wg[wi];
                    if (G.bridge || G.tunnel || ways[wi].klass > 6) continue;
                    float hwm = ways[wi].hw + ((ways[wi].flags & 8) ? 1.9f : 0.0f) + 0.8f;
                    for (size_t k = 0; k < G.rx.size(); k++) {
                        int c0 = (int)std::floor((G.rx[k] - hwm - r.x0) / CSm), c1 = (int)std::floor((G.rx[k] + hwm - r.x0) / CSm);
                        int q0 = (int)std::floor((G.rz[k] - hwm - r.z0) / CSm), q1 = (int)std::floor((G.rz[k] + hwm - r.z0) / CSm);
                        for (int q = std::max(0, q0); q <= std::min(wnz - 1, q1); q++)
                            for (int c = std::max(0, c0); c <= std::min(wnx - 1, c1); c++) roadMask[(size_t)q * wnx + c] = 1;
                    }
                }
            }
        }
        { std::vector<Vertex> wv; buildWater(d, g, cx, cy, r.x0, r.z0, r.x1, r.z1, wv, &roadMask); bucketCells(wv, r.x0, r.z0, r.x1, r.z1, r.waterCells); }
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
    for (auto& c : d->chunks) { for (Cell& q : c.second.sideCells) q.m.destroy(); for (Cell& q : c.second.roadCells) q.m.destroy(); for (Cell& q : c.second.bldCells) q.m.destroy(); for (Cell& q : c.second.waterCells) q.m.destroy(); for (Cell& q : c.second.deckCells) q.m.destroy(); }
    d->tiles.clear(); d->chunks.clear(); d->requested.clear(); d->chunkReq.clear();
    { std::lock_guard<std::mutex> lk(d->carveM); d->carveAll.clear(); }
    d->rebuildTiles.clear();
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
        if (t.tex) { glDeleteTextures(1, &t.tex); t.tex = 0; t.mesh.destroy(); }   // Neubau (Einschnitt): alte Fassung ersetzen
        t.z = r.z; t.x = r.x; t.y = r.y; t.touched = now;
        t.mesh.create(r.v.data(), (int)r.v.size());
        t.lo = {1e9f, 1e9f, 1e9f}; t.hi = {-1e9f, -1e9f, -1e9f};
        for (const Vertex& q : r.v) { t.lo = {std::min(t.lo.x, q.p[0]), std::min(t.lo.y, q.p[1]), std::min(t.lo.z, q.p[2])}; t.hi = {std::max(t.hi.x, q.p[0]), std::max(t.hi.y, q.p[1]), std::max(t.hi.z, q.p[2])}; }
        t.tex = gfxTexture(r.rgb.data(), r.w, r.h, 3, true);
        t.forest = std::move(r.forest); t.fw = r.w; t.fh = r.h;
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
        uploadCells(r.waterCells, c.waterCells);
        uploadCells(r.deckCells, c.deckCells);
        c.segs = std::move(r.segs); c.names = std::move(r.names);
        c.controls = std::move(r.controls); c.buildings = std::move(r.buildings); c.pois = std::move(r.pois); c.trees = std::move(r.trees); c.patches = std::move(r.patches); c.signs = std::move(r.signs);
        if (!r.carves.empty() || !c.carves.empty()) {
            c.carves = std::move(r.carves);
            {
                std::lock_guard<std::mutex> lk(D.carveM);
                D.carveAll.clear();
                for (auto& ch : D.chunks) D.carveAll.insert(D.carveAll.end(), ch.second.carves.begin(), ch.second.carves.end());
            }
            double nz2 = (double)(1 << D.zoom[2]);
            for (const CarveSeg& cv : c.carves) { // alle nahen Gelaendekacheln im Umkreis des Einschnitts neu bauen
                float xa = std::min(cv.ax, cv.bx) - 25, xb = std::max(cv.ax, cv.bx) + 25, za = std::min(cv.az, cv.bz) - 25, zb = std::max(cv.az, cv.bz) + 25;
                int tx0 = (int)std::floor((g.mx0 + xa / g.S) * nz2), tx1 = (int)std::floor((g.mx0 + xb / g.S) * nz2), ty0 = (int)std::floor((g.my0 + za / g.S) * nz2), ty1 = (int)std::floor((g.my0 + zb / g.S) * nz2);
                for (int ty = ty0; ty <= ty1; ty++) for (int tx = tx0; tx <= tx1; tx++) D.rebuildTiles[tkey(D.zoom[2], tx, ty)] = true;
            }
            logMsg("Einschnitte (Unterfuehrungen) in Kachel %d/%d: %d Abschnitte", r.cx, r.cy, (int)c.carves.size());
        }
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
                if (it != D.tiles.end()) {
                    it->second.touched = now;
                    auto rb = D.rebuildTiles.find(k);
                    if (rb == D.rebuildTiles.end()) continue;
                    auto rq0 = D.requested.find(k);
                    if (rq0 != D.requested.end() && rq0->second == 0) continue;   // laeuft gerade
                    D.rebuildTiles.erase(rb);
                    want.push_back({z, x, y, (float)(dx * dx + dy * dy)});
                    continue;
                }
                D.rebuildTiles.erase(k);   // wird ohnehin frisch gebaut
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
            for (Cell& q : it->second.sideCells) q.m.destroy(); for (Cell& q : it->second.roadCells) q.m.destroy(); for (Cell& q : it->second.bldCells) q.m.destroy(); for (Cell& q : it->second.waterCells) q.m.destroy(); for (Cell& q : it->second.deckCells) q.m.destroy();
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
    // ueberschriebene Strassen (z. B. Unterfuehrung unter einer im Hoehenmodell enthaltenen Bruecke): hoechste Flaeche unter yRef
    float bestP = -1e9f;
    for (auto& kv : d->chunks) {
        if (kv.second.patches.empty() || x < kv.second.x0 - 40 || x > kv.second.x1 + 40 || z < kv.second.z0 - 40 || z > kv.second.z1 + 40) continue;
        for (const RoadPatch& p : kv.second.patches) {
            if (x < p.x0 - p.hw - 1 || x > p.x1 + p.hw + 1 || z < p.z0 - p.hw - 1 || z > p.z1 + p.hw + 1) continue;
            for (size_t i = 0; i + 1 < p.px.size(); i++) {
                float vx = p.px[i + 1] - p.px[i], vz = p.pz[i + 1] - p.pz[i], l2 = vx * vx + vz * vz;
                if (l2 < 1e-6f) continue;
                float t = clampv(((x - p.px[i]) * vx + (z - p.pz[i]) * vz) / l2, 0.0f, 1.0f);
                if (std::hypot(x - (p.px[i] + vx * t), z - (p.pz[i] + vz * t)) > p.hw + 0.6f) continue;
                float y = p.py[i] + (p.py[i + 1] - p.py[i]) * t + 0.03f;
                if (y <= yRef && y > bestP) bestP = y;
            }
        }
    }
    if (bestP > -1e8f && (h > yRef || bestP > h)) h = bestP; // ueberschriebene Fahrbahn gilt auch, wenn das gemessene Gelaende darueber liegt
    float hh;
    if (surfaceHook && surfaceHook(x, z, yRef, hh) && hh > h) h = hh;
    return true;
}
bool World::roadHeightOverride(int64_t id, int& mode, float& delta) { return heightOverride(id, mode, delta); }
void World::setRoadHeight(int64_t id, int mode, float delta) {
    {
        std::lock_guard<std::mutex> lk(OV.m);
        if (mode == 0) OV.hov.erase(id); else OV.hov[id] = {mode, delta};
        std::error_code ec;
        std::filesystem::create_directories("maps", ec);
        saveOverrides();
    }
    for (auto& ch : d->chunks) { for (Cell& q : ch.second.sideCells) q.m.destroy(); for (Cell& q : ch.second.roadCells) q.m.destroy(); for (Cell& q : ch.second.bldCells) q.m.destroy(); for (Cell& q : ch.second.waterCells) q.m.destroy(); for (Cell& q : ch.second.deckCells) q.m.destroy(); }
    d->chunks.clear();
    d->chunkReq.clear();
    d->version++;
}
Vec3 World::surfaceNormal(float x, float z, float yRef) {
    float a, b, c, e;
    const float s = 0.5f;
    if (!surface(x - s, z, yRef, a) || !surface(x + s, z, yRef, b) || !surface(x, z - s, yRef, c) || !surface(x, z + s, yRef, e)) return {0, 1, 0};
    Vec3 n = normalize({a - b, 2 * s, c - e});
    return n.y < 0.5f ? Vec3{0, 1, 0} : n; // Kanten von Objekten nicht als Steilhang werten
}

RoadHit World::nearestRoad(float x, float z, float maxDist, int maxKlass, float yRef) {
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
            if (yRef > -1e8f) { // andere Ebene (Bruecke ueber dem Fahrzeug, Tunnel darunter, Unterfuehrung): ignorieren
                float ys = s.ya > -1e8f ? s.ya + (s.yb - s.ya) * t : yRef, gh;
                if (s.ya <= -1e8f && height(qx, qz, gh)) ys = gh;
                if (std::fabs(ys - yRef) > 3.2f) continue;
            }
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
void World::drawTerrain(int level, const Mat4& vp, Vec3 cam) {
    int z = d->zoom[level];
    Frustum fr(vp);
    // Grobe Stufen liegen wegen ihrer weiten Maschen oft hoeher als das echte Gelaende (Taeler, Einschnitte, Unterfuehrungen):
    // die Kamera geraet dann UNTER das grobe Netz und sieht dessen Unterseite als schnell ziehende braun-graue Flaeche am Himmel.
    // Deshalb wird jede grobe Stufe dort abgesenkt, wo die naechstfeinere um die Kamera lueckenlos geladen ist.
    float sinkR = 0;
    if (level < 2) {
        int zf = d->zoom[level + 1];
        double n = (double)(1 << zf), mx = g.mx0 + cam.x / g.S, my = g.my0 + cam.z / g.S;
        int cx = (int)std::floor(mx * n), cy = (int)std::floor(my * n);
        int rOk = -1;
        for (int r = 0; r <= d->radius[level + 1]; r++) {
            bool all = true;
            for (int dy = -r; dy <= r && all; dy++)
                for (int dx = -r; dx <= r; dx++) {
                    if (std::max(std::abs(dx), std::abs(dy)) != r) continue;
                    if (!d->tiles.count(tkey(zf, cx + dx, cy + dy))) { all = false; break; }
                }
            if (!all) break;
            rOk = r;
        }
        if (rOk >= 0) {
            double ts = g.S / n;   // Kachelkante in Metern
            double fx = mx * n - cx, fy = my * n - cy;
            sinkR = (float)((rOk + std::min(std::min(fx, 1.0 - fx), std::min(fy, 1.0 - fy))) * ts);
        }
    }
    gfxSink(cam.x, cam.z, sinkR > 20.0f ? sinkR * 0.92f : 0.0f, level == 0 ? 400.0f : 60.0f);
    for (auto& kv : d->tiles) {
        if (kv.second.z != z || !fr.sees(kv.second.lo, kv.second.hi)) continue;
        gfxBindTexture(kv.second.tex);
        kv.second.mesh.draw();
    }
    gfxSink(0, 0, 0, 0);
}
void World::drawRoads(const Mat4& vp, Vec3 cam, float maxDist) {
    Frustum fr(vp);
    for (int pass = 0; pass < 2; pass++) // erst alle Gehsteige, dann aller Asphalt
        for (auto& kv : d->chunks)
            for (const Cell& c : pass == 0 ? kv.second.sideCells : kv.second.roadCells)
                if (boxDist(c.lo, c.hi, cam) < maxDist && fr.sees(c.lo, c.hi)) c.m.draw();
}
void World::drawDecks(const Mat4& vp, Vec3 cam, float maxDist) {
    Frustum fr(vp);
    for (auto& kv : d->chunks)
        for (const Cell& c : kv.second.deckCells)
            if (boxDist(c.lo, c.hi, cam) < maxDist && fr.sees(c.lo, c.hi)) c.m.draw();
}
void World::drawWater(const Mat4& vp, Vec3 cam, float maxDist) {
    Frustum fr(vp);
    for (auto& kv : d->chunks)
        for (const Cell& c : kv.second.waterCells)
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
        for (auto& ch : d->chunks) { for (Cell& q : ch.second.sideCells) q.m.destroy(); for (Cell& q : ch.second.roadCells) q.m.destroy(); for (Cell& q : ch.second.bldCells) q.m.destroy(); for (Cell& q : ch.second.waterCells) q.m.destroy(); for (Cell& q : ch.second.deckCells) q.m.destroy(); }
        d->chunks.clear();
        d->chunkReq.clear();
        d->version++;
    }
}

void World::forTreesNear(float x, float z, float r, const std::function<void(const TreePos&)>& fn) const {
    for (auto& kv : d->chunks)
        for (const TreePos& t : kv.second.trees)
            if (std::fabs(t.x - x) < r && std::fabs(t.z - z) < r) fn(t);
}
void World::forSignsNear(float x, float z, float r, const std::function<void(const SignDef&)>& fn) const {
    for (auto& kv : d->chunks)
        for (const SignDef& s : kv.second.signs)
            if (std::fabs(s.pos.x - x) < r && std::fabs(s.pos.z - z) < r) fn(s);
}
int World::forestAt(float x, float z) const {
    double mx = g.mx0 + x / g.S, my = g.my0 + z / g.S, n = (double)(1 << d->zoom[1]);
    int tx = (int)std::floor(mx * n), ty = (int)std::floor(my * n);
    auto it = d->tiles.find(tkey(d->zoom[1], tx, ty));
    if (it == d->tiles.end() || it->second.forest.empty()) return -1;
    const Tile& t = it->second;
    int px = clampv((int)((mx * n - tx) * t.fw), 0, t.fw - 1), py = clampv((int)((my * n - ty) * t.fh), 0, t.fh - 1);
    return t.forest[(size_t)py * t.fw + px];
}
bool World::osmReady(float x, float z) const {
    double mx = g.mx0 + x / g.S, my = g.my0 + z / g.S;
    auto it = d->chunks.find(tkey(14, (int)std::floor(mx * 16384.0), (int)std::floor(my * 16384.0)));
    return it != d->chunks.end() && it->second.ok;
}
void World::forPoisNear(float x, float z, float r, const std::function<void(const Poi&)>& fn) const {
    for (auto& kv : d->chunks)
        for (const Poi& p : kv.second.pois)
            if (std::fabs(p.x - x) < r && std::fabs(p.z - z) < r) fn(p);
}
