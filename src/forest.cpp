// Baeume und Waelder: prozedurale Baeume mit Blattkarten, per Instanced Rendering gezeichnet.
//  - Strassenbaeume kommen aus den OSM-Kacheln (World::forTreesNear)
//  - Waelder stehen auf OSM-Waldflaechen (World::forestAt), ohne OSM-Landnutzung dort, wo das Satellitenbild dunkel und gruen ist
//  - Nah volles Modell, weiter weg ein stark vereinfachtes Modell (Kegel / Kugel), dahinter nichts
#include "game.h"
#include "net.h"
#include <nlohmann/json.hpp>
#include <unordered_map>
#include <cstring>
#include <functional>

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
    auto emit = [&](const json& m, const Mat4& T) {
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
                    Vec3 pp, nn{0, 1, 0};
                    pp.x = readComp(P.p + vi * P.stride, P.ct, false); pp.y = readComp(P.p + vi * P.stride + compSize(P.ct), P.ct, false); pp.z = readComp(P.p + vi * P.stride + 2 * compSize(P.ct), P.ct, false);
                    if (hasN) { nn.x = readComp(N.p + vi * N.stride, N.ct, false); nn.y = readComp(N.p + vi * N.stride + compSize(N.ct), N.ct, false); nn.z = readComp(N.p + vi * N.stride + 2 * compSize(N.ct), N.ct, false); }
                    pp = xformPoint(T, pp); nn = normalize(xformDir(T, nn));
                    v.p[0] = pp.x; v.p[1] = pp.y; v.p[2] = pp.z; v.n[0] = nn.x; v.n[1] = nn.y; v.n[2] = nn.z;
                    float col[3] = {base[0], base[1], base[2]};
                    if (hasC) for (int c = 0; c < 3; c++) col[c] *= readComp(C.p + vi * C.stride + c * compSize(C.ct), C.ct, C.norm);
                    v.c = Color{(uint8_t)(srgb(col[0]) * 235), (uint8_t)(srgb(col[1]) * 235), (uint8_t)(srgb(col[2]) * 235), 255};
                    out.push_back(v);
                }
        }
    };
    // Knotenhierarchie (Matrizen oder Position/Drehung/Skalierung) abarbeiten
    std::function<void(int, const Mat4&)> visit = [&](int ni, const Mat4& parent) {
        const json& n = j["nodes"][ni];
        Mat4 L = identity();
        if (n.contains("matrix")) { for (int i = 0; i < 16; i++) L.m[i] = n["matrix"][i].get<float>(); }
        else {
            Vec3 t{0, 0, 0}, sc{1, 1, 1};
            Quat q{1, 0, 0, 0};
            if (n.contains("translation")) t = {n["translation"][0].get<float>(), n["translation"][1].get<float>(), n["translation"][2].get<float>()};
            if (n.contains("scale")) sc = {n["scale"][0].get<float>(), n["scale"][1].get<float>(), n["scale"][2].get<float>()};
            if (n.contains("rotation")) q = Quat{n["rotation"][3].get<float>(), n["rotation"][0].get<float>(), n["rotation"][1].get<float>(), n["rotation"][2].get<float>()};
            L = mul(fromQuatPos(q, t), scaleM(sc.x, sc.y, sc.z));
        }
        Mat4 W = mul(parent, L);
        if (n.contains("mesh")) emit(j["meshes"][n["mesh"].get<int>()], W);
        if (n.contains("children")) for (const json& c : n["children"]) visit(c.get<int>(), W);
    };
    if (j.contains("scenes") && j.contains("nodes"))
        for (const json& r : j["scenes"][j.value("scene", 0)]["nodes"]) visit(r.get<int>(), identity());
    return !out.empty();
}

struct Species {
    Mesh full, lod;
    float height = 1, radius = 1;     // Modellgroesse (Einheiten)
    float minScale = 3, maxScale = 4; // Zielhoehe / height
    bool ok = false;
};
// 0 Linde, 1 Kiefer, 2 Birke, 3 Fichte, 4 Pappel, 5 Buche (alle prozedural mit Blattkarten)
const int NSP = 6;
Species SP[NSP];

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

void pushCluster(std::vector<Vertex>& out, Vec3 c, float r, int cards, Vec3 crownC, bool needles, Color col, uint32_t seed);
void pushCore(std::vector<Vertex>& out, Vec3 c, Vec3 r, Color col);
Color jitterC(Color c, float k);
void pushLimb(std::vector<Vertex>& out, Vec3 a, Vec3 b, float ra, float rb, Color col);
// Modell fertigstellen: Fuss auf y = 0, Fernansicht (einfacher Stamm + Krone aus mittleren Farben), Zielgroessen in Metern
bool finishSpecies(int k, std::vector<Vertex>& v, bool pine, float minMeters, float maxMeters, const char* label) {
    Species& s = SP[k];
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
    std::vector<Vertex> l;   // Fernmodell: Stamm, Kern und wenige grosse Blattkarten (gleiche Textur wie nah, keine Kanten)
    float H = s.height, R = s.radius;
    pushLimb(l, {0, 0, 0}, {0, H * (pine ? 0.5f : 0.45f), 0}, R * 0.07f, R * 0.04f, bark);
    if (pine) {
        Vec3 cc{0, H * 0.55f, 0};
        pushCore(l, cc, {R * 0.45f, H * 0.38f, R * 0.45f}, jitterC(foliage, 0.55f));
        for (int i = 0; i < 4; i++) pushCluster(l, {0, H * (0.30f + 0.17f * i), 0}, R * (0.95f - 0.18f * i), 4, cc, true, foliage, 900 + i);
    } else {
        Vec3 cc{0, H * 0.66f, 0};
        pushCore(l, cc, {R * 0.70f, H * 0.28f, R * 0.70f}, jitterC(foliage, 0.55f));
        for (int i = 0; i < 4; i++) { float a = i * 1.57f; pushCluster(l, cc + Vec3{std::cos(a) * R * 0.4f, (i % 2 ? 0.08f : -0.05f) * H, std::sin(a) * R * 0.4f}, R * 0.75f, 4, cc, false, foliage, 950 + i); }
    }
    s.lod.create(l.data(), (int)l.size());
    s.minScale = minMeters / H; s.maxScale = maxMeters / H;
    s.ok = true;
    logMsg("Baum-Modell %s: %d Dreiecke (Fernansicht %d), %.1f x %.1f Einheiten", label, (int)v.size() / 3, (int)l.size() / 3, H, R * 2);
    return true;
}
bool loadSpecies(int k, const char* path, bool pine) {
    std::vector<Vertex> v;
    if (!loadGlb(path, v)) { logMsg("Baum-Modell %s nicht ladbar", path); return false; }
    return finishSpecies(k, v, pine, pine ? 10.0f : 6.5f, pine ? 16.0f : 10.0f, path);
}

// ---------- Prozedurale Baumarten (Einheitshoehe 1, die Instanz skaliert auf Meter) ----------
// Kronen aus vielen kleinen Blattkarten (Textur mit Transparenz: einzelne Blaetter bzw. Nadelzweige), dazu Stamm und Aeste.
// Die Normalen der Karten zeigen vom Kronenmittelpunkt nach aussen: die Krone wirkt rund und weich beleuchtet statt facettiert.
float rnd01(uint32_t& s) { s = s * 1664525u + 1013904223u; return (s >> 8) / 16777216.0f; }
Color jitterC(Color c, float k) { return Color{(uint8_t)clampv(c.r * k, 0.0f, 255.0f), (uint8_t)clampv(c.g * k, 0.0f, 255.0f), (uint8_t)clampv(c.b * k, 0.0f, 255.0f), 255}; }

// Atlas 512 x 512: links Laubblaetter (u 0..0.5), rechts Nadelzweige (u 0.5..1), unten ein deckender Rindenstreifen (v 0.92..1)
GLuint leafTex = 0;
const float BARK_V = 0.96f;
void buildLeafTexture() {
    const int W = 512, H = 512;
    std::vector<uint8_t> px((size_t)W * H * 4, 0);
    uint32_t st = 1234;
    auto put = [&](int x, int y, float v, float g) {
        if (x < 0 || y < 0 || x >= W || y >= H) return;
        uint8_t* p = &px[((size_t)y * W + x) * 4];
        p[0] = (uint8_t)clampv(v * 0.92f * 255, 0.0f, 255.0f); p[1] = (uint8_t)clampv(v * g * 255, 0.0f, 255.0f); p[2] = (uint8_t)clampv(v * 0.82f * 255, 0.0f, 255.0f); p[3] = 255;
    };
    // Laubblaetter: dicht in der Mitte, locker am Rand (runder Zweigbuschel)
    for (int i = 0; i < 520; i++) {
        float r = std::sqrt(rnd01(st)) * 108.0f, a = rnd01(st) * 6.2832f;
        float cx = 128 + std::cos(a) * r, cy = 230 + std::sin(a) * r * 0.95f;
        float len = 7 + rnd01(st) * 7, wid = 3 + rnd01(st) * 2.5f, ang = rnd01(st) * 6.2832f, ca = std::cos(ang), sa = std::sin(ang);
        float shade = 0.50f + 0.32f * rnd01(st) - 0.18f * (r / 108.0f), g = 1.0f + 0.10f * rnd01(st);
        for (int y = (int)(cy - len); y <= (int)(cy + len); y++)
            for (int x = (int)(cx - len); x <= (int)(cx + len); x++) {
                float dx = x - cx, dy = y - cy, u = dx * ca + dy * sa, v = -dx * sa + dy * ca;
                float e = (u * u) / (len * len) + (v * v) / (wid * wid);
                if (e > 1) continue;
                put(x, y, shade * (0.86f + 0.14f * (1 - e)) * (std::fabs(v) < 0.6f ? 0.85f : 1.0f), g);   // Mittelrippe etwas dunkler
            }
    }
    // Nadelzweige: Zweige vom Mittelpunkt, Nadeln beidseitig
    for (int b = 0; b < 9; b++) {
        float a = -1.1f + b * 0.28f + rnd01(st) * 0.1f, ca = std::cos(a), sa = std::sin(a);
        float L = 70 + rnd01(st) * 60;
        for (float s2 = 0; s2 < L; s2 += 2.0f) {
            float bx = 300 + ca * s2 * 1.6f, by = 230 + sa * s2;
            for (int sd = -1; sd <= 1; sd += 2) {
                float na = a + sd * (0.9f + rnd01(st) * 0.3f), nl = 8 + rnd01(st) * 6 - s2 * 0.03f, shade = 0.38f + 0.25f * rnd01(st);
                for (float t = 0; t < nl; t += 0.6f) { put((int)(bx + std::cos(na) * t), (int)(by + std::sin(na) * t), shade, 1.05f); put((int)(bx + std::cos(na) * t) + 1, (int)(by + std::sin(na) * t), shade * 0.9f, 1.05f); }
            }
            put((int)bx, (int)by, 0.30f, 0.9f); put((int)bx, (int)by + 1, 0.30f, 0.9f);
        }
    }
    // Rinde: deckend, senkrechte Streifen
    for (int y = (int)(H * 0.92f); y < H; y++)
        for (int x = 0; x < W; x++) {
            float v = 0.55f + 0.18f * std::sin(x * 0.9f + std::sin(x * 0.13f) * 3) + 0.08f * (((x * 7 + y * 13) % 11) / 11.0f);
            uint8_t* p = &px[((size_t)y * W + x) * 4];
            p[0] = p[1] = p[2] = (uint8_t)clampv(v * 255, 0.0f, 255.0f); p[3] = 255;
        }
    leafTex = gfxTexture(px.data(), W, H, 4, true);
}

struct Crown { Vec3 c; float r; };
// Karte (Quad) mit Blatt- oder Nadeltextur; Normale = Richtung vom Kronenmittelpunkt (weiche, runde Beleuchtung)
void pushCard(std::vector<Vertex>& out, Vec3 p, Vec3 axU, Vec3 axV, Vec3 center, bool needles, Color col) {
    float u0 = needles ? 0.5f : 0.0f, u1 = u0 + 0.5f;
    Vec3 q[4] = {p - axU - axV, p + axU - axV, p + axU + axV, p - axU + axV};
    float uv[4][2] = {{u0, 0.9f}, {u1, 0.9f}, {u1, 0.0f}, {u0, 0.0f}};
    Vertex v[4];
    for (int i = 0; i < 4; i++) {
        Vec3 n = normalize(q[i] - center + Vec3{0, 0.15f, 0});
        v[i] = {{q[i].x, q[i].y, q[i].z}, {n.x, n.y, n.z}, {uv[i][0], uv[i][1]}, col};
    }
    out.push_back(v[0]); out.push_back(v[1]); out.push_back(v[2]);
    out.push_back(v[0]); out.push_back(v[2]); out.push_back(v[3]);
}
// Blattbuschel: viele zufaellig gedrehte Karten um c
void pushCluster(std::vector<Vertex>& out, Vec3 c, float r, int cards, Vec3 crownC, bool needles, Color col, uint32_t seed) {
    uint32_t st = seed * 2654435761u + 7u;
    for (int i = 0; i < cards; i++) {
        Vec3 d = normalize(Vec3{rnd01(st) - 0.5f, (rnd01(st) - 0.5f) * 0.7f, rnd01(st) - 0.5f});
        Vec3 p = c + d * (r * 0.45f * rnd01(st));
        Vec3 nrm = normalize(Vec3{rnd01(st) - 0.5f, rnd01(st) * 0.8f + 0.2f, rnd01(st) - 0.5f});
        Vec3 ref = std::fabs(nrm.y) > 0.9f ? Vec3{1, 0, 0} : Vec3{0, 1, 0};
        Vec3 au = normalize(cross(nrm, ref)), av = cross(nrm, au);
        float s = r * (0.55f + 0.35f * rnd01(st));
        // oben heller (Sonne), innen/unten dunkler (Eigenschatten)
        float k = 0.78f + 0.30f * clampv((p.y - crownC.y) / std::max(r, 0.01f) * 0.5f + 0.5f, 0.0f, 1.0f) + 0.10f * (rnd01(st) - 0.5f);
        pushCard(out, p, au * s, av * s, crownC, needles, jitterC(col, k));
    }
}
// Kegelstumpf/Ast von a nach b (6 Seiten), Rindentextur
void pushLimb(std::vector<Vertex>& out, Vec3 a, Vec3 b, float ra, float rb, Color col) {
    Vec3 ax = normalize(b - a), ref = std::fabs(ax.y) > 0.9f ? Vec3{1, 0, 0} : Vec3{0, 1, 0};
    Vec3 u = normalize(cross(ax, ref)), w = cross(ax, u);
    const int S = 6;
    for (int i = 0; i < S; i++) {
        float a0 = i * 2 * PI / S, a1 = (i + 1) * 2 * PI / S;
        Vec3 d0 = u * std::cos(a0) + w * std::sin(a0), d1 = u * std::cos(a1) + w * std::sin(a1);
        Vec3 p0 = a + d0 * ra, p1 = a + d1 * ra, q0 = b + d0 * rb, q1 = b + d1 * rb;
        float uA = (float)i / S, uB = (float)(i + 1) / S;
        Vertex vv[4] = {{{p0.x, p0.y, p0.z}, {d0.x, d0.y, d0.z}, {uA, BARK_V}, col}, {{p1.x, p1.y, p1.z}, {d1.x, d1.y, d1.z}, {uB, BARK_V}, col},
                        {{q1.x, q1.y, q1.z}, {d1.x, d1.y, d1.z}, {uB, 0.99f}, col}, {{q0.x, q0.y, q0.z}, {d0.x, d0.y, d0.z}, {uA, 0.99f}, col}};
        out.push_back(vv[0]); out.push_back(vv[1]); out.push_back(vv[2]);
        out.push_back(vv[0]); out.push_back(vv[2]); out.push_back(vv[3]);
    }
}
// dunkler Kern der Krone (deckend), damit man nicht durch die Krone hindurchsieht
void pushCore(std::vector<Vertex>& out, Vec3 c, Vec3 r, Color col) {
    const int S = 7, R = 4;
    auto P = [&](int i, int j) {
        float a = i * 2 * PI / S, b = -PI / 2 + j * PI / R;
        return Vec3{c.x + std::cos(a) * std::cos(b) * r.x, c.y + std::sin(b) * r.y, c.z + std::sin(a) * std::cos(b) * r.z};
    };
    auto T = [&](Vec3 a, Vec3 b, Vec3 d) {
        for (Vec3 q : {a, b, d}) { Vec3 n = normalize(q - c); out.push_back({{q.x, q.y, q.z}, {n.x, n.y, n.z}, {0.5f, 0.97f}, col}); }
    };
    for (int i = 0; i < S; i++)
        for (int j = 0; j < R; j++) { T(P(i, j), P(i + 1, j), P(i, j + 1)); T(P(i + 1, j), P(i + 1, j + 1), P(i, j + 1)); }
}
// Laubbaum: Stamm, Hauptaeste zu den Buescheln, Kern, Blattkarten
void broadleaf(int k, const char* label, Color leaf, Color bark, float trunkTop, float crownY, float crownR, float crownH, int clusters, float minM, float maxM, uint32_t seed, float trunkR) {
    std::vector<Vertex> v;
    uint32_t st = seed;
    pushLimb(v, {0, 0, 0}, {0, 0.06f, 0}, trunkR * 1.35f, trunkR, bark);                 // Wurzelanlauf
    pushLimb(v, {0, 0.06f, 0}, {0.01f, trunkTop, 0.005f}, trunkR, trunkR * 0.62f, bark);
    Vec3 cc{0, crownY, 0};
    std::vector<Crown> cl;
    for (int i = 0; i < clusters; i++) {
        float a = i * 2.39996f + rnd01(st) * 0.4f, h = rnd01(st);
        float rr = crownR * std::sqrt(1.0f - (h - 0.45f) * (h - 0.45f) * 2.2f) * (0.55f + 0.45f * rnd01(st));
        Vec3 c{std::cos(a) * rr, crownY + (h - 0.45f) * crownH, std::sin(a) * rr};
        cl.push_back({c, crownR * (0.36f + 0.14f * rnd01(st))});
    }
    for (size_t i = 0; i < cl.size(); i++) { // Aeste vom Stammende zu den aeusseren Bueschen
        if (i % 2) continue;
        Vec3 from{0, trunkTop * (0.75f + 0.25f * rnd01(st)), 0};
        pushLimb(v, from, cl[i].c * 0.8f + from * 0.2f, trunkR * 0.42f, trunkR * 0.12f, bark);
    }
    pushCore(v, cc, {crownR * 0.55f, crownH * 0.30f, crownR * 0.55f}, jitterC(leaf, 0.50f));
    for (size_t i = 0; i < cl.size(); i++) pushCluster(v, cl[i].c, cl[i].r, 9, cc, false, leaf, seed * 31 + (uint32_t)i);
    finishSpecies(k, v, false, minM, maxM, label);
}
void buildProcedural() {
    buildLeafTexture();
    broadleaf(0, "Linde (Blattkarten)", Color{92, 132, 60, 255}, Color{104, 90, 76, 255}, 0.44f, 0.64f, 0.32f, 0.50f, 16, 8.0f, 14.0f, 21, 0.032f);
    broadleaf(5, "Buche (Blattkarten)", Color{70, 112, 50, 255}, Color{118, 112, 104, 255}, 0.40f, 0.62f, 0.38f, 0.48f, 18, 12.0f, 20.0f, 5, 0.036f);
    { // 2 Birke: schlanker weisser Stamm, lockere haengende Bueschel
        std::vector<Vertex> v;
        uint32_t st = 11;
        Color bark{224, 220, 212, 255}, leaf{128, 162, 72, 255};
        pushLimb(v, {0, 0, 0}, {0.015f, 0.80f, 0.01f}, 0.020f, 0.008f, bark);
        Vec3 cc{0, 0.66f, 0};
        pushCore(v, cc, {0.08f, 0.13f, 0.08f}, jitterC(leaf, 0.55f));
        for (int i = 0; i < 12; i++) {
            float a = i * 2.39996f + rnd01(st) * 0.3f, y = 0.42f + 0.48f * rnd01(st), rr = 0.07f + 0.12f * (1.0f - std::fabs(y - 0.62f) * 2.0f);
            Vec3 c{std::cos(a) * rr, y, std::sin(a) * rr};
            if (i % 2 == 0) pushLimb(v, {0.012f, y - 0.06f, 0.008f}, c, 0.006f, 0.003f, bark);
            pushCluster(v, c, 0.10f + 0.03f * rnd01(st), 7, cc, false, leaf, 100 + i);
        }
        finishSpecies(2, v, false, 9.0f, 15.0f, "Birke (Blattkarten)");
    }
    { // 3 Fichte: Stockwerke aus haengenden Nadelzweig-Karten rund um den Stamm
        std::vector<Vertex> v;
        uint32_t st = 300;
        Color leaf{44, 84, 52, 255}, bark{88, 64, 46, 255};
        pushLimb(v, {0, 0, 0}, {0, 1.0f, 0}, 0.022f, 0.004f, bark);
        pushCore(v, {0, 0.50f, 0}, {0.06f, 0.36f, 0.06f}, jitterC(leaf, 0.55f));
        for (int t = 0; t < 14; t++) {
            float y = 0.14f + 0.062f * t, r = 0.30f * (1.0f - (y - 0.1f) / 0.95f) + 0.03f;
            int n = 9 + (int)(r * 20);
            for (int i = 0; i < n; i++) {
                float a = (i + rnd01(st) * 0.6f) * 6.2832f / n + t * 0.7f, ca = std::cos(a), sa = std::sin(a);
                Vec3 out{ca, -0.32f, sa};   // Zweig haengt leicht nach unten
                Vec3 mid = Vec3{0, y, 0} + out * (r * 0.55f);
                Vec3 side{-sa, 0, ca};
                Vec3 au = normalize(out) * (r * 0.58f), av = side * (r * 0.55f);
                pushCard(v, mid, au, av, Vec3{0, y + 0.05f, 0}, true, jitterC(leaf, 0.85f + 0.3f * rnd01(st) + 0.12f * (t / 14.0f)));
            }
        }
        finishSpecies(3, v, true, 14.0f, 24.0f, "Fichte (Nadelkarten)");
    }
    { // 1 Kiefer: langer kahler Stamm, lockere schirmfoermige Nadelbueschel oben
        std::vector<Vertex> v;
        uint32_t st = 900;
        Color leaf{58, 92, 54, 255}, bark{136, 92, 62, 255};
        pushLimb(v, {0, 0, 0}, {0.02f, 0.82f, 0.01f}, 0.026f, 0.012f, bark);
        Vec3 cc{0, 0.84f, 0};
        pushCore(v, cc, {0.08f, 0.04f, 0.08f}, jitterC(leaf, 0.55f));
        for (int i = 0; i < 11; i++) {
            float a = i * 2.39996f, rr = 0.06f + 0.14f * rnd01(st), y = 0.74f + 0.20f * rnd01(st);
            Vec3 c{std::cos(a) * rr, y, std::sin(a) * rr};
            pushLimb(v, {0.015f, 0.66f + 0.1f * rnd01(st), 0.008f}, c, 0.010f, 0.004f, bark);
            pushCluster(v, c, 0.09f + 0.03f * rnd01(st), 7, cc, true, leaf, 200 + i);
        }
        finishSpecies(1, v, true, 12.0f, 20.0f, "Kiefer (Nadelkarten)");
    }
    { // 4 Pappel: hohe schmale Saeule
        std::vector<Vertex> v;
        uint32_t st = 77;
        Color leaf{96, 138, 58, 255}, bark{108, 96, 80, 255};
        pushLimb(v, {0, 0, 0}, {0, 0.86f, 0}, 0.016f, 0.006f, bark);
        Vec3 cc{0, 0.62f, 0};
        pushCore(v, cc, {0.05f, 0.24f, 0.05f}, jitterC(leaf, 0.55f));
        for (int i = 0; i < 14; i++) {
            float y = 0.30f + 0.66f * (i / 13.0f), a = i * 2.39996f, rr = 0.04f * (1.0f - std::fabs(y - 0.6f));
            pushCluster(v, {std::cos(a) * rr, y, std::sin(a) * rr}, 0.075f + 0.02f * rnd01(st), 6, cc, false, leaf, 500 + i);
        }
        finishSpecies(4, v, false, 16.0f, 26.0f, "Pappel (Blattkarten)");
    }
}

// ---------- Waldzellen ----------
struct TreeI { float x, y, z, s; uint8_t kind; };
struct FCell { std::vector<TreeI> t; };
std::unordered_map<int64_t, FCell> cells;
const float CELL = 48.0f, SPACING = 8.0f;
const float FULL_R = 220, LOD1_R = 330, LOD2_R = 520, STREET_R = 400;   // bis 220 m das volle Modell mit Blattkarten

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
            // Mischwald: Laubbaeume, Buchen, Birken, Kiefern, Fichten
            float kr = hash3(cx * 31 + gx, cz * 17 + gz, 5);
            uint8_t kind = kr < 0.15f ? 0 : kr < 0.35f ? 5 : kr < 0.47f ? 2 : kr < 0.70f ? 1 : 3;
            out.t.push_back({px, h - 0.15f, pz, SP[kind].minScale + (SP[kind].maxScale - SP[kind].minScale) * r, kind});
        }
    return true;
}

// Instanzlisten: [Art][0 voll, 1 nah-einfach, 2 fern-einfach]
std::vector<float> lists[NSP][3];
GLuint bufs[NSP][3] = {};
int counts[NSP][3] = {};
}

namespace forest {

void init() {
    // Die mitgelieferten .glb-Baeume (assets/trees) sind sehr kantig; alle Arten werden jetzt mit Blattkarten erzeugt.
    (void)loadSpecies;
    buildProcedural();
}

void update(Vec3 cam) {
    bool anyOk = false;
    for (const Species& s : SP) anyOk = anyOk || s.ok;
    if (!anyOk) return;
    static int geoGen = -1;
    if (geoGen != world.geo().gen) { geoGen = world.geo().gen; cells.clear(); } // Ortswechsel: neuer lokaler Ursprung
    static int landVer = -1;
    if (landVer != world.landVersion()) { landVer = world.landVersion(); cells.clear(); } // neue Landnutzung: Wald nur auf OSM-Waldflaechen
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
        // Strassenbaeume: Laubbaeume gemischt, vereinzelt Birke und Pappel, selten Kiefer
        uint8_t kind = p.rnd < 0.30f ? 0 : p.rnd < 0.62f ? 5 : p.rnd < 0.80f ? 2 : p.rnd < 0.92f ? 4 : 1;
        float s = SP[kind].minScale + (SP[kind].maxScale - SP[kind].minScale) * 0.5f * (0.4f + p.rnd);
        add({p.x, p.y, p.z, s * (kind == 1 ? 0.7f : (kind == 4 ? 0.8f : 0.62f)), kind});
    });
    for (int k = 0; k < NSP; k++)
        for (int b = 0; b < 3; b++) {
            counts[k][b] = (int)lists[k][b].size() / 4;
            bufs[k][b] = gfxInstBuffer(bufs[k][b], lists[k][b].data(), counts[k][b]);
        }
}

void draw(bool mirror) {
    gfxModel(identity());
    gfxInstanced(true);
    gfxBindTexture(leafTex);
    gfxMode(10);   // Blattkarten mit Transparenz (Alpha-Test), durchscheinendes Licht; nah volles Modell, fern wenige grosse Karten
    for (int k = 0; k < NSP; k++) if (SP[k].ok) SP[k].full.drawInstanced(bufs[k][0], counts[k][0]);
    for (int k = 0; k < NSP; k++) {
        if (!SP[k].ok) continue;
        SP[k].lod.drawInstanced(bufs[k][1], counts[k][1]);
        if (!mirror) SP[k].lod.drawInstanced(bufs[k][2], counts[k][2]);
    }
    gfxInstanced(false);
}

} // namespace forest
