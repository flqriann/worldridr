// Baeume und Waelder: .glb-Modelle (assets/trees) werden per Instanced Rendering gezeichnet.
//  - Strassenbaeume kommen aus den OSM-Kacheln (World::forTreesNear)
//  - Waelder entstehen dort, wo das Satellitenbild dunkel und gruen ist (World::forestAt), nicht auf Strassen/Gebaeuden
//  - Nah volles Modell, weiter weg ein stark vereinfachtes Modell (Kegel / Kugel), dahinter nichts
#include "game.h"
#include "net.h"
#include <nlohmann/json.hpp>
#include <unordered_map>
#include <cstring>

using json = nlohmann::json;

namespace {
// ---------- minimaler glTF-Binaerlader: Dreiecke mit Materialfarbe (und COLOR_0), keine Texturen ----------
bool loadGlb(const std::string& path, std::vector<Vertex>& out) {
    std::vector<uint8_t> d;
    if (!readFile(path, d) || d.size() < 28) return false;
    uint32_t jl;
    memcpy(&jl, &d[12], 4);
    if (20 + (size_t)jl + 8 > d.size()) return false;
    json j = json::parse(d.begin() + 20, d.begin() + 20 + jl, nullptr, false);
    if (j.is_discarded()) return false;
    size_t bo = 20 + (size_t)jl;
    uint32_t bl;
    memcpy(&bl, &d[bo], 4);
    const uint8_t* bin = d.data() + bo + 8;
    if (bo + 8 + bl > d.size()) return false;

    auto readComp = [&](const uint8_t* p, int ct, bool norm) -> float {
        switch (ct) {
        case 5126: { float f; memcpy(&f, p, 4); return f; }
        case 5123: { uint16_t u; memcpy(&u, p, 2); return norm ? u / 65535.0f : (float)u; }
        case 5121: return norm ? *p / 255.0f : (float)*p;
        case 5125: { uint32_t u; memcpy(&u, p, 4); return (float)u; }
        }
        return 0;
    };
    auto compSize = [](int ct) { return ct == 5126 || ct == 5125 ? 4 : ct == 5123 ? 2 : 1; };
    struct Acc { const uint8_t* p = nullptr; int count = 0, comps = 0, ct = 0; size_t stride = 0; bool norm = false; };
    auto acc = [&](int ai) {
        Acc a;
        const json& A = j["accessors"][ai];
        const json& B = j["bufferViews"][A["bufferView"].get<int>()];
        std::string t = A["type"];
        a.comps = t == "SCALAR" ? 1 : t == "VEC2" ? 2 : t == "VEC3" ? 3 : t == "VEC4" ? 4 : 1;
        a.ct = A["componentType"]; a.count = A["count"]; a.norm = A.value("normalized", false);
        size_t off = (size_t)B.value("byteOffset", 0) + (size_t)A.value("byteOffset", 0);
        a.stride = B.value("byteStride", 0);
        if (!a.stride) a.stride = (size_t)a.comps * compSize(a.ct);
        a.p = bin + off;
        return a;
    };
    auto srgb = [](float c) { return std::pow(std::min(std::max(c, 0.0f), 1.0f), 1.0f / 2.2f); };
    for (const json& m : j["meshes"])
        for (const json& pr : m["primitives"]) {
            if (pr.value("mode", 4) != 4 || !pr["attributes"].contains("POSITION")) continue;
            Acc P = acc(pr["attributes"]["POSITION"]);
            Acc N, C;
            bool hasN = pr["attributes"].contains("NORMAL"), hasC = pr["attributes"].contains("COLOR_0");
            if (hasN) N = acc(pr["attributes"]["NORMAL"]);
            if (hasC) C = acc(pr["attributes"]["COLOR_0"]);
            float base[3] = {0.7f, 0.7f, 0.7f};
            if (pr.contains("material")) {
                const json& mat = j["materials"][pr["material"].get<int>()];
                if (mat.contains("pbrMetallicRoughness") && mat["pbrMetallicRoughness"].contains("baseColorFactor")) {
                    const json& bc = mat["pbrMetallicRoughness"]["baseColorFactor"];
                    for (int i = 0; i < 3; i++) base[i] = bc[i].get<float>();
                }
            }
            std::vector<int> idx;
            if (pr.contains("indices")) {
                Acc I = acc(pr["indices"]);
                for (int i = 0; i < I.count; i++) idx.push_back((int)readComp(I.p + i * I.stride, I.ct, false));
            } else for (int i = 0; i < P.count; i++) idx.push_back(i);
            for (size_t k = 0; k + 2 < idx.size(); k += 3)
                for (int q = 0; q < 3; q++) {
                    int vi = idx[k + q];
                    if (vi < 0 || vi >= P.count) continue;
                    Vertex v{};
                    for (int c = 0; c < 3; c++) v.p[c] = readComp(P.p + vi * P.stride + c * compSize(P.ct), P.ct, false);
                    if (hasN) for (int c = 0; c < 3; c++) v.n[c] = readComp(N.p + vi * N.stride + c * compSize(N.ct), N.ct, false);
                    else v.n[1] = 1;
                    float col[3] = {base[0], base[1], base[2]};
                    if (hasC) for (int c = 0; c < 3; c++) col[c] *= readComp(C.p + vi * C.stride + c * compSize(C.ct), C.ct, C.norm);
                    v.c = Color{(uint8_t)(srgb(col[0]) * 235), (uint8_t)(srgb(col[1]) * 235), (uint8_t)(srgb(col[2]) * 235), 255};
                    out.push_back(v);
                }
        }
    return !out.empty();
}

struct Species {
    Mesh full, lod;
    float height = 1, radius = 1;     // Modellgroesse (Einheiten)
    float minScale = 3, maxScale = 4; // Zielhoehe / height
    bool ok = false;
};
Species SP[2]; // 0 Laubbaum, 1 Kiefer

void pushSphere(std::vector<Vertex>& out, Vec3 c, Vec3 r, Color col) {
    const int S = 7, R = 4;
    auto P = [&](int i, int j) {
        float a = i * 2 * PI / S, b = -PI / 2 + j * PI / R;
        return Vec3{c.x + std::cos(a) * std::cos(b) * r.x, c.y + std::sin(b) * r.y, c.z + std::sin(a) * std::cos(b) * r.z};
    };
    auto T = [&](Vec3 a, Vec3 b, Vec3 d) {
        Vec3 n = normalize(cross(b - a, d - a));
        if (dot(n, a - c) < 0) n = -n;
        for (Vec3 q : {a, b, d}) out.push_back({{q.x, q.y, q.z}, {n.x, n.y, n.z}, {0, 0}, col});
    };
    for (int i = 0; i < S; i++)
        for (int j = 0; j < R; j++) { T(P(i, j), P(i + 1, j), P(i, j + 1)); T(P(i + 1, j), P(i + 1, j + 1), P(i, j + 1)); }
}
void pushCone(std::vector<Vertex>& out, Vec3 base, float r, float h, Color col) {
    const int S = 8;
    Vec3 apex = base + Vec3{0, h, 0};
    for (int i = 0; i < S; i++) {
        float a0 = i * 2 * PI / S, a1 = (i + 1) * 2 * PI / S;
        Vec3 p0 = base + Vec3{std::cos(a0) * r, 0, std::sin(a0) * r}, p1 = base + Vec3{std::cos(a1) * r, 0, std::sin(a1) * r};
        Vec3 n = normalize(cross(p1 - p0, apex - p0));
        if (dot(n, p0 - base) < 0) n = -n;
        for (Vec3 q : {p0, p1, apex}) out.push_back({{q.x, q.y, q.z}, {n.x, n.y, n.z}, {0, 0}, col});
        Vec3 dn{0, -1, 0};
        for (Vec3 q : {base, p1, p0}) out.push_back({{q.x, q.y, q.z}, {dn.x, dn.y, dn.z}, {0, 0}, col});
    }
}
void pushTrunk(std::vector<Vertex>& out, float h, float r, Color col) {
    const int S = 5;
    for (int i = 0; i < S; i++) {
        float a0 = i * 2 * PI / S, a1 = (i + 1) * 2 * PI / S;
        Vec3 b0{std::cos(a0) * r, 0, std::sin(a0) * r}, b1{std::cos(a1) * r, 0, std::sin(a1) * r};
        Vec3 t0 = b0 * 0.6f + Vec3{0, h, 0}, t1 = b1 * 0.6f + Vec3{0, h, 0};
        Vec3 n = normalize(Vec3{(b0.x + b1.x) * 0.5f, 0, (b0.z + b1.z) * 0.5f});
        for (Vec3 q : {b0, b1, t1, b0, t1, t0}) out.push_back({{q.x, q.y, q.z}, {n.x, n.y, n.z}, {0, 0}, col});
    }
}

bool loadSpecies(int k, const char* path, bool pine) {
    Species& s = SP[k];
    std::vector<Vertex> v;
    if (!loadGlb(path, v)) { logMsg("Baum-Modell %s nicht ladbar", path); return false; }
    float lo = 1e9f, hi = -1e9f, rad = 0;
    for (const Vertex& q : v) { lo = std::min(lo, q.p[1]); hi = std::max(hi, q.p[1]); rad = std::max(rad, std::max(std::fabs(q.p[0]), std::fabs(q.p[2]))); }
    for (Vertex& q : v) q.p[1] -= lo; // Fuss auf y = 0
    s.height = hi - lo; s.radius = rad;
    s.full.create(v.data(), (int)v.size());
    // mittlere Farben: Laub (obere Haelfte) und Stamm (unten) fuer die einfache Fernansicht
    double fr = 0, fg = 0, fb = 0, tr = 0, tg = 0, tb = 0; int nf = 0, nt = 0;
    for (const Vertex& q : v) {
        if (q.p[1] > s.height * 0.3f && q.c.g > q.c.r) { fr += q.c.r; fg += q.c.g; fb += q.c.b; nf++; }
        else if (q.p[1] < s.height * 0.1f) { tr += q.c.r; tg += q.c.g; tb += q.c.b; nt++; }
    }
    Color foliage{(uint8_t)(nf ? fr / nf : 60), (uint8_t)(nf ? fg / nf : 110), (uint8_t)(nf ? fb / nf : 50), 255};
    Color bark{(uint8_t)(nt ? tr / nt : 90), (uint8_t)(nt ? tg / nt : 62), (uint8_t)(nt ? tb / nt : 40), 255};
    std::vector<Vertex> l;
    float H = s.height, R = s.radius;
    pushTrunk(l, H * (pine ? 0.3f : 0.42f), R * 0.1f, bark);
    if (pine) {
        for (int i = 0; i < 4; i++) {
            Color c = foliage;
            c.r = (uint8_t)(c.r * (0.8f + 0.07f * i)); c.g = (uint8_t)(std::min(255.0f, c.g * (0.8f + 0.07f * i))); c.b = (uint8_t)(c.b * (0.8f + 0.07f * i));
            pushCone(l, {0, H * (0.2f + 0.17f * i), 0}, R * (1.0f - 0.2f * i), H * 0.34f, c);
        }
    } else pushSphere(l, {0, H * 0.66f, 0}, {R, H * 0.34f, R}, foliage);
    s.lod.create(l.data(), (int)l.size());
    s.minScale = (pine ? 10.0f : 6.5f) / H; s.maxScale = (pine ? 16.0f : 10.0f) / H;
    s.ok = true;
    logMsg("Baum-Modell %s: %d Dreiecke (Fernansicht %d), %.1f x %.1f Einheiten", path, (int)v.size() / 3, (int)l.size() / 3, H, R * 2);
    return true;
}

// ---------- Waldzellen ----------
struct TreeI { float x, y, z, s; uint8_t kind; };
struct FCell { std::vector<TreeI> t; };
std::unordered_map<int64_t, FCell> cells;
const float CELL = 48.0f, SPACING = 8.0f;
const float FULL_R = 60, LOD1_R = 160, LOD2_R = 380, STREET_R = 320;

float hash3(int a, int b, int c) {
    uint32_t h = (uint32_t)a * 374761393u + (uint32_t)b * 668265263u + (uint32_t)c * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return ((h ^ (h >> 16)) & 0xFFFFFF) / 16777216.0f;
}
int64_t ck(int cx, int cz) { return ((int64_t)cx << 32) ^ (int64_t)(uint32_t)cz; }

// false = Daten fehlen noch (spaeter nochmal versuchen)
bool genCell(int cx, int cz, FCell& out) {
    float x0 = cx * CELL, z0 = cz * CELL;
    if (!world.osmReady(x0 + CELL * 0.5f, z0 + CELL * 0.5f)) return false;
    int n = (int)(CELL / SPACING);
    for (int gx = 0; gx < n; gx++)
        for (int gz = 0; gz < n; gz++) {
            float px = x0 + (gx + hash3(cx * 31 + gx, cz * 17 + gz, 1) * 0.9f + 0.05f) * SPACING;
            float pz = z0 + (gz + hash3(cx * 31 + gx, cz * 17 + gz, 2) * 0.9f + 0.05f) * SPACING;
            int f = world.forestAt(px, pz);
            if (f < 0) return false;
            if (!f || hash3(cx * 31 + gx, cz * 17 + gz, 3) > 0.86f) continue;
            float h;
            if (!world.height(px, pz, h)) return false;
            RoadHit rh = world.nearestRoad(px, pz, 14.0f);
            if (rh.found && rh.dist < rh.seg->hw + 3.0f) continue;
            if (world.insideBuilding(px, pz)) continue;
            float r = hash3(cx * 31 + gx, cz * 17 + gz, 4);
            uint8_t kind = hash3(cx * 31 + gx, cz * 17 + gz, 5) < 0.55f ? 1 : 0;
            out.t.push_back({px, h - 0.15f, pz, SP[kind].minScale + (SP[kind].maxScale - SP[kind].minScale) * r, kind});
        }
    return true;
}

// Instanzlisten: [Art][0 voll, 1 nah-einfach, 2 fern-einfach]
std::vector<float> lists[2][3];
GLuint bufs[2][3] = {};
int counts[2][3] = {};
}

namespace forest {

void init() {
    loadSpecies(0, "assets/trees/tree.glb", false);
    loadSpecies(1, "assets/trees/pine.glb", true);
}

void update(Vec3 cam) {
    if (!SP[0].ok && !SP[1].ok) return;
    static int geoGen = -1;
    if (geoGen != world.geo().gen) { geoGen = world.geo().gen; cells.clear(); } // Ortswechsel: neuer lokaler Ursprung
    // fehlende Zellen im Umkreis erzeugen (wenige pro Bild), weit entfernte verwerfen
    int cx0 = (int)std::floor(cam.x / CELL), cz0 = (int)std::floor(cam.z / CELL), rc = (int)(LOD2_R / CELL) + 1, made = 0;
    for (int ring = 0; ring <= rc && made < 4; ring++)
        for (int dx = -ring; dx <= ring && made < 4; dx++)
            for (int dz = -ring; dz <= ring && made < 4; dz++) {
                if (std::max(std::abs(dx), std::abs(dz)) != ring) continue;
                int64_t k = ck(cx0 + dx, cz0 + dz);
                if (cells.count(k)) continue;
                FCell c;
                if (genCell(cx0 + dx, cz0 + dz, c)) cells[k] = std::move(c);
                made++;
            }
    if (cells.size() > 700)
        for (auto it = cells.begin(); it != cells.end();) {
            // Zelle aus dem Schluessel zurueckrechnen
            int cx = (int)(it->first >> 32), cz = (int)(uint32_t)(it->first & 0xFFFFFFFF);
            if (std::abs(cx - cx0) > rc + 3 || std::abs(cz - cz0) > rc + 3) it = cells.erase(it); else ++it;
        }
    for (auto& a : lists) for (auto& l : a) l.clear();
    auto add = [&](const TreeI& t) {
        float d = std::hypot(t.x - cam.x, t.z - cam.z);
        int b = d < FULL_R ? 0 : d < LOD1_R ? 1 : d < LOD2_R ? 2 : -1;
        if (b < 0 || !SP[t.kind].ok) return;
        auto& l = lists[t.kind][b];
        l.push_back(t.x); l.push_back(t.y); l.push_back(t.z); l.push_back(t.s);
    };
    for (int dx = -rc; dx <= rc; dx++)
        for (int dz = -rc; dz <= rc; dz++) {
            auto it = cells.find(ck(cx0 + dx, cz0 + dz));
            if (it != cells.end()) for (const TreeI& t : it->second.t) add(t);
        }
    world.forTreesNear(cam.x, cam.z, STREET_R, [&](const TreePos& p) {
        uint8_t kind = p.rnd < 0.2f ? 1 : 0;
        float s = SP[kind].minScale + (SP[kind].maxScale - SP[kind].minScale) * 0.5f * (0.4f + p.rnd);
        add({p.x, p.y, p.z, s * (kind ? 0.7f : 0.55f), kind});
    });
    for (int k = 0; k < 2; k++)
        for (int b = 0; b < 3; b++) {
            counts[k][b] = (int)lists[k][b].size() / 4;
            bufs[k][b] = gfxInstBuffer(bufs[k][b], lists[k][b].data(), counts[k][b]);
        }
}

void draw(bool mirror) {
    gfxModel(identity());
    gfxMode(0, FLAG_LIT | FLAG_FOG);
    gfxInstanced(true);
    for (int k = 0; k < 2; k++) {
        if (!SP[k].ok) continue;
        SP[k].full.drawInstanced(bufs[k][0], counts[k][0]);
        SP[k].lod.drawInstanced(bufs[k][1], counts[k][1]);
        if (!mirror) SP[k].lod.drawInstanced(bufs[k][2], counts[k][2]);
    }
    gfxInstanced(false);
}

} // namespace forest
