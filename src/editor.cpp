// Welt-Editor: Objekte platzieren (Block, Zylinder, Keil, Kugel, OBJ-Modelle), faerben, texturieren,
// Strassen mit Kurven bauen, alles in maps/welt.json speichern. Positionen sind Geokoordinaten.
#include "game.h"
#include "net.h"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <map>
#include "stb_image_write.h"

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {
const double DPI = 3.14159265358979323846;
const char* MAP_FILE = "maps/welt.json";
const char* SHAPE_NAMES[] = {"Block", "Zylinder", "Keil (Rampe)", "Kugel", "Modell"};

struct Obj {
    int shape = 0;
    double lat = 0, lon = 0;
    float alt = 0, tile = 2;
    Vec3 rot;              // Gieren, Nicken, Rollen
    Vec3 scale{2, 2, 2};
    Color color{205, 205, 205, 255};
    std::string texture, model;
    bool solid = true;
    Vec3 pos;              // lokal, abgeleitet
};
struct RPoint { double lat, lon; float lift; float x, z; };
struct Road {
    int type = 1;
    std::vector<RPoint> pts;
    std::vector<Vec3> line;     // abgetastete Mittellinie mit Fahrbahnhoehe
    std::vector<int> ptSample;
    float minx = 0, maxx = 0, minz = 0, maxz = 0;
    bool built = false;
};
struct RoadType { const char* name; float hw; int lanes, flags, oneway; float speed; int klass; bool sidewalk; };
const RoadType TYPES[] = {
    {"Stadtstra\xC3\x9F" "e mit Gehsteig", 3.5f, 2, 1, 0, 50, 4, true},
    {"Landstra\xC3\x9F" "e", 3.7f, 2, 1 | 2, 0, 100, 3, false},
    {"4 Spuren", 7.2f, 4, 1 | 2, 0, 70, 2, true},
    {"Einbahn", 2.4f, 1, 4, 1, 30, 5, true},
    {"Autobahn (eine Richtung)", 5.6f, 2, 1 | 2 | 4, 1, 130, 0, false},
    {"Feldweg", 1.6f, 1, 0, 0, 30, 7, false}};
const int NTYPES = 6;
struct Model { Mesh mesh; Vec3 bmin{-0.5f, -0.5f, -0.5f}, bmax{0.5f, 0.5f, 0.5f}; };

std::vector<Obj> objs;
std::vector<Road> roads;
Mesh shapes[4], solidMesh;
std::map<std::string, Model> models;
std::map<std::string, GLuint> textures;
std::vector<std::string> textureList, modelList, undoStack;
GLuint whiteTex = 0;

bool on = false, dirty = true, drawing = false, testDrive = false, modified = false;
int tool = 0, sel = -1, selRoad = -1, selPt = -1, roadType = 0, geoGen = -1;
double nextBuild = 0;
Vec3 camP;
float camYaw = 0, camPitch = -0.45f;
bool rmb = false, lmbPressed = false, lmbDown = false, rmbMoved = false;
float mouseX = 0, mouseY = 0, lastMX = 0, lastMY = 0, vw = 1, vh = 1;
int dragAxis = -1;
bool dragBody = false, dragPoint = false;
float dragOffset = 0;
Mat4 lastVP = identity();
Batch gizmo;
std::string statusMsg;
double statusUntil = 0;

double lonToMx(double lon) { return (lon + 180.0) / 360.0; }
double latToMy(double lat) { return (1.0 - std::asinh(std::tan(lat * DPI / 180.0)) / DPI) / 2.0; }
void toLocal(double lat, double lon, float& x, float& z) {
    const GeoRef& g = world.geo();
    x = (float)((lonToMx(lon) - g.mx0) * g.S);
    z = (float)((latToMy(lat) - g.my0) * g.S);
}
Quat rotOf(const Obj& o) { return qmul(qaxis({0, 1, 0}, o.rot.x), qmul(qaxis({1, 0, 0}, o.rot.y), qaxis({0, 0, 1}, o.rot.z))); }
Mat4 matOf(const Obj& o) { return mul(fromQuatPos(rotOf(o), o.pos), scaleM(o.scale.x, o.scale.y, o.scale.z)); }
void say(const std::string& s) { statusMsg = s; statusUntil = simTime + 3; }

// ---------- Grundformen ----------
void makeShapes() {
    Batch b;
    Color w{255, 255, 255, 255};
    b.box(identity(), {0, 0, 0}, {0.5f, 0.5f, 0.5f}, w);
    shapes[0].create(b.v.data(), (int)b.v.size());
    b.clear();
    const int N = 28;
    for (int i = 0; i < N; i++) {
        float a0 = i * 2 * PI / N, a1 = (i + 1) * 2 * PI / N, am = (a0 + a1) / 2;
        Vec3 p0{std::cos(a0) * 0.5f, 0, std::sin(a0) * 0.5f}, p1{std::cos(a1) * 0.5f, 0, std::sin(a1) * 0.5f}, up{0, 0.5f, 0};
        b.quad(p0 - up, p1 - up, p1 + up, p0 + up, {std::cos(am), 0, std::sin(am)}, w);
        b.tri(up, p0 + up, p1 + up, {0, 1, 0}, w);
        b.tri(-up, p1 - up, p0 - up, {0, -1, 0}, w);
    }
    shapes[1].create(b.v.data(), (int)b.v.size());
    b.clear();
    Vec3 A{-0.5f, -0.5f, -0.5f}, B{0.5f, -0.5f, -0.5f}, C{0.5f, -0.5f, 0.5f}, D{-0.5f, -0.5f, 0.5f}, E{-0.5f, 0.5f, -0.5f}, F{0.5f, 0.5f, -0.5f};
    b.quad(A, B, C, D, {0, -1, 0}, w);
    b.quad(A, B, F, E, {0, 0, -1}, w);
    b.quad(E, F, C, D, normalize({0, 1, 1}), w);
    b.tri(A, D, E, {-1, 0, 0}, w);
    b.tri(B, C, F, {1, 0, 0}, w);
    shapes[2].create(b.v.data(), (int)b.v.size());
    b.clear();
    const int LA = 14, LO = 24;
    auto sp = [](int i, int k) {
        float th = i * PI / LA, ph = k * 2 * PI / LO;
        return Vec3{std::sin(th) * std::cos(ph) * 0.5f, std::cos(th) * 0.5f, std::sin(th) * std::sin(ph) * 0.5f};
    };
    for (int i = 0; i < LA; i++)
        for (int k = 0; k < LO; k++) {
            Vec3 p[4] = {sp(i, k), sp(i, k + 1), sp(i + 1, k + 1), sp(i + 1, k)};
            int id[6] = {0, 1, 2, 0, 2, 3};
            for (int q : id) b.vert(p[q], normalize(p[q]), 0, 0, w);
        }
    shapes[3].create(b.v.data(), (int)b.v.size());
}

// ---------- Mitgelieferte Beispieltexturen (werden einmalig nach assets/textures geschrieben) ----------
float hashf(int x, int y) {
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return ((h ^ (h >> 16)) & 0xFFFF) / 65535.0f;
}
void genTextures() {
    std::error_code ec;
    fs::create_directories("assets/textures", ec);
    fs::create_directories("assets/models", ec);
    if (!fs::is_empty("assets/textures", ec)) return;
    const int S = 256;
    std::vector<uint8_t> px(S * S * 3);
    auto put = [&](int x, int y, float r, float g, float b) {
        uint8_t* p = &px[(y * S + x) * 3];
        p[0] = (uint8_t)clampv(r, 0.0f, 255.0f); p[1] = (uint8_t)clampv(g, 0.0f, 255.0f); p[2] = (uint8_t)clampv(b, 0.0f, 255.0f);
    };
    auto write = [&](const char* name) { stbi_flip_vertically_on_write(0); stbi_write_png((std::string("assets/textures/") + name).c_str(), S, S, 3, px.data(), S * 3); };
    for (int y = 0; y < S; y++) for (int x = 0; x < S; x++) {
        int row = y / 32, bx = (x + (row % 2) * 32) % 64;
        bool mortar = y % 32 < 3 || bx < 3;
        float n = hashf(x, y) * 22 + hashf((x + (row % 2) * 32) / 64, row) * 40;
        if (mortar) put(x, y, 178 + n * 0.3f, 172 + n * 0.3f, 162 + n * 0.3f); else put(x, y, 128 + n, 58 + n * 0.5f, 44 + n * 0.4f);
    }
    write("ziegel.png");
    for (int y = 0; y < S; y++) for (int x = 0; x < S; x++) { float n = hashf(x, y) * 26 + hashf(x / 8, y / 8) * 18; put(x, y, 132 + n, 132 + n, 130 + n); }
    write("beton.png");
    for (int y = 0; y < S; y++) for (int x = 0; x < S; x++) { float n = hashf(x, y) * 50 + hashf(x / 4, y / 4) * 30; put(x, y, 40 + n * 0.5f, 92 + n, 30 + n * 0.4f); }
    write("gras.png");
    for (int y = 0; y < S; y++) for (int x = 0; x < S; x++) {
        float n = std::sin(x * 0.25f + std::sin(y * 0.05f) * 3) * 14 + hashf(x, y / 6) * 20;
        bool gap = x % 64 < 2;
        put(x, y, gap ? 60 : 142 + n, gap ? 40 : 96 + n * 0.8f, gap ? 25 : 56 + n * 0.5f);
    }
    write("holz.png");
    for (int y = 0; y < S; y++) for (int x = 0; x < S; x++) {
        bool joint = x % 32 < 2 || y % 32 < 2;
        float n = hashf(x / 32, y / 32) * 34 + hashf(x, y) * 14;
        put(x, y, joint ? 62 : 120 + n, joint ? 60 : 118 + n, joint ? 58 : 114 + n);
    }
    write("pflaster.png");
    for (int y = 0; y < S; y++) for (int x = 0; x < S; x++) { float n = hashf(x / 12, y) * 22 + hashf(x, y) * 8; put(x, y, 150 + n, 156 + n, 164 + n); }
    write("metall.png");
    for (int y = 0; y < S; y++) for (int x = 0; x < S; x++) {
        int cx = x % 64, cy = y % 85;
        bool win = cx > 14 && cx < 50 && cy > 18 && cy < 66;
        float n = hashf(x, y) * 12;
        if (win) put(x, y, 34 + hashf(x / 64, y / 85) * 60, 46 + hashf(x / 64, y / 85) * 60, 62 + hashf(x / 64, y / 85) * 60); else put(x, y, 204 + n, 196 + n, 180 + n);
    }
    write("fassade.png");
}

void scanAssets() {
    textureList.clear(); modelList.clear();
    std::error_code ec;
    for (auto& e : fs::directory_iterator("assets/textures", ec)) {
        std::string ext = e.path().extension().string();
        for (char& c : ext) c = (char)tolower((unsigned char)c);
        if (ext == ".png" || ext == ".jpg" || ext == ".jpeg") textureList.push_back(e.path().filename().string());
    }
    for (auto& e : fs::directory_iterator("assets/models", ec))
        if (e.path().extension() == ".obj") modelList.push_back(e.path().filename().string());
    std::sort(textureList.begin(), textureList.end());
    std::sort(modelList.begin(), modelList.end());
}

GLuint texOf(const std::string& name) {
    if (name.empty()) return whiteTex;
    auto it = textures.find(name);
    if (it != textures.end()) return it->second ? it->second : whiteTex;
    GLuint t = gfxTextureFile("assets/textures/" + name);
    textures[name] = t;
    return t ? t : whiteTex;
}

// Einfacher Wavefront-OBJ-Leser (Positionen, Normalen, beliebige Polygone)
Model& modelOf(const std::string& name) {
    auto it = models.find(name);
    if (it != models.end()) return it->second;
    Model& m = models[name];
    std::ifstream f("assets/models/" + name);
    std::vector<Vec3> P, N;
    Batch b;
    std::string line;
    Vec3 lo{1e9f, 1e9f, 1e9f}, hi{-1e9f, -1e9f, -1e9f};
    while (std::getline(f, line)) {
        std::istringstream ss(line);
        std::string t;
        ss >> t;
        if (t == "v") { Vec3 p; ss >> p.x >> p.y >> p.z; P.push_back(p); lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)}; hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)}; }
        else if (t == "vn") { Vec3 n; ss >> n.x >> n.y >> n.z; N.push_back(n); }
        else if (t == "f") {
            std::vector<Vec3> fp, fn;
            std::string tok;
            while (ss >> tok) {
                int vi = atoi(tok.c_str()), ni = 0;
                size_t s2 = tok.rfind('/');
                if (s2 != std::string::npos && tok.find('/') != s2) ni = atoi(tok.c_str() + s2 + 1);
                if (vi < 0) vi = (int)P.size() + vi + 1;
                if (ni < 0) ni = (int)N.size() + ni + 1;
                if (vi < 1 || vi > (int)P.size()) continue;
                fp.push_back(P[vi - 1]);
                fn.push_back(ni >= 1 && ni <= (int)N.size() ? N[ni - 1] : Vec3{0, 0, 0});
            }
            for (size_t k = 1; k + 1 < fp.size(); k++) {
                Vec3 flat = normalize(cross(fp[k] - fp[0], fp[k + 1] - fp[0]));
                size_t id[3] = {0, k, k + 1};
                for (size_t q : id) b.vert(fp[q], length(fn[q]) > 0.1f ? normalize(fn[q]) : flat, 0, 0, Color{});
            }
        }
    }
    if (!b.v.empty()) { m.mesh.create(b.v.data(), (int)b.v.size()); m.bmin = lo; m.bmax = hi; }
    else logMsg("Modell %s konnte nicht gelesen werden", name.c_str());
    return m;
}

void boundsOf(const Obj& o, Vec3& lo, Vec3& hi) {
    lo = {-0.5f, -0.5f, -0.5f}; hi = {0.5f, 0.5f, 0.5f};
    if (o.shape == 4) { Model& m = modelOf(o.model); lo = m.bmin; hi = m.bmax; }
    lo = {lo.x * o.scale.x, lo.y * o.scale.y, lo.z * o.scale.z};
    hi = {hi.x * o.scale.x, hi.y * o.scale.y, hi.z * o.scale.z};
}

// ---------- Speichern / Laden ----------
json serialize() {
    json j;
    j["objects"] = json::array();
    for (const Obj& o : objs)
        j["objects"].push_back({{"shape", o.shape}, {"lat", o.lat}, {"lon", o.lon}, {"alt", o.alt}, {"rot", {o.rot.x, o.rot.y, o.rot.z}},
                                {"scale", {o.scale.x, o.scale.y, o.scale.z}}, {"color", {o.color.r, o.color.g, o.color.b}}, {"texture", o.texture},
                                {"model", o.model}, {"solid", o.solid}, {"tile", o.tile}});
    j["roads"] = json::array();
    for (const Road& r : roads) {
        json pts = json::array();
        for (const RPoint& p : r.pts) pts.push_back({p.lat, p.lon, p.lift});
        j["roads"].push_back({{"type", r.type}, {"points", pts}});
    }
    return j;
}
void deserialize(const json& j) {
    objs.clear(); roads.clear();
    try {
        if (j.contains("objects"))
            for (const json& e : j["objects"]) {
                Obj o;
                o.shape = e.value("shape", 0); o.lat = e.value("lat", 0.0); o.lon = e.value("lon", 0.0); o.alt = e.value("alt", 0.0f);
                o.rot = Vec3(e["rot"][0].get<float>(), e["rot"][1].get<float>(), e["rot"][2].get<float>());
                o.scale = Vec3(e["scale"][0].get<float>(), e["scale"][1].get<float>(), e["scale"][2].get<float>());
                o.color = Color{e["color"][0].get<uint8_t>(), e["color"][1].get<uint8_t>(), e["color"][2].get<uint8_t>(), 255};
                o.texture = e.value("texture", ""); o.model = e.value("model", ""); o.solid = e.value("solid", true); o.tile = e.value("tile", 2.0f);
                objs.push_back(o);
            }
        if (j.contains("roads"))
            for (const json& e : j["roads"]) {
                Road r;
                r.type = clampv(e.value("type", 0), 0, NTYPES - 1);
                for (const json& p : e["points"]) r.pts.push_back(RPoint{p[0].get<double>(), p[1].get<double>(), p[2].get<float>(), 0, 0});
                if (r.pts.size() >= 2) roads.push_back(r);
            }
    } catch (...) { logMsg("Kartendatei fehlerhaft"); }
    geoGen = -1; dirty = true; nextBuild = 0;
}
void pushUndo() {
    undoStack.push_back(serialize().dump());
    if (undoStack.size() > 40) undoStack.erase(undoStack.begin());
    modified = true;
}
void undo() {
    if (undoStack.empty()) return;
    deserialize(json::parse(undoStack.back(), nullptr, false));
    undoStack.pop_back();
    sel = selRoad = selPt = -1; drawing = false;
    say("R\xC3\xBC" "ckg\xC3\xA4ngig");
}

// ---------- Strassen aufbauen ----------
void buildRoads() {
    std::vector<Vertex> rv;
    std::vector<RoadSeg> segs;
    Batch solid;
    bool incomplete = false;
    Color concrete{122, 122, 118, 255}, walk{172, 172, 166, 255}, curb{140, 140, 136, 255};
    for (Road& r : roads) {
        r.built = false;
        int n = (int)r.pts.size();
        if (n < 2 || std::fabs(r.pts[0].x) > 9000 || std::fabs(r.pts[0].z) > 9000) continue;
        const RoadType& T = TYPES[r.type];
        std::vector<Vec3> c;
        std::vector<float> lift;
        r.ptSample.assign(n, 0);
        for (int i = 0; i + 1 < n; i++) {
            const RPoint &p0 = r.pts[std::max(i - 1, 0)], &p1 = r.pts[i], &p2 = r.pts[i + 1], &p3 = r.pts[std::min(i + 2, n - 1)];
            float len = std::hypot(p2.x - p1.x, p2.z - p1.z);
            int steps = std::max(1, (int)std::ceil(len / 4.0f));
            r.ptSample[i] = (int)c.size();
            for (int s = 0; s < steps + (i == n - 2 ? 1 : 0); s++) {
                float t = (float)s / steps, t2 = t * t, t3 = t2 * t;
                auto cr = [&](float a, float b, float cc, float d) { return 0.5f * (2 * b + (-a + cc) * t + (2 * a - 5 * b + 4 * cc - d) * t2 + (-a + 3 * b - 3 * cc + d) * t3); };
                c.push_back({cr(p0.x, p1.x, p2.x, p3.x), 0, cr(p0.z, p1.z, p2.z, p3.z)});
                lift.push_back(p1.lift + (p2.lift - p1.lift) * t);
            }
        }
        r.ptSample[n - 1] = (int)c.size() - 1;
        int m = (int)c.size();
        std::vector<Vec3> rt(m);
        std::vector<float> raw(m), hl(m), hr(m), dist(m, 0.0f);
        float ext = T.hw + (T.sidewalk ? 1.9f : 0.0f);
        bool ok = true;
        for (int k = 0; k < m && ok; k++) {
            int a = std::max(k - 1, 0), b = std::min(k + 1, m - 1);
            Vec3 t = normalize({c[b].x - c[a].x, 0, c[b].z - c[a].z});
            rt[k] = {-t.z, 0, t.x};
            float hc;
            ok = world.height(c[k].x, c[k].z, hc) && world.height(c[k].x - rt[k].x * ext, c[k].z - rt[k].z * ext, hl[k]) && world.height(c[k].x + rt[k].x * ext, c[k].z + rt[k].z * ext, hr[k]);
            raw[k] = std::max(hc, std::max(hl[k], hr[k])) + 0.10f + lift[k];
            if (k > 0) dist[k] = dist[k - 1] + length(c[k] - c[k - 1]);
        }
        if (!ok) { incomplete = true; continue; }
        r.minx = r.minz = 1e9f; r.maxx = r.maxz = -1e9f;
        for (int k = 0; k < m; k++) {
            float sum = 0; int cnt = 0;
            for (int q = std::max(0, k - 4); q <= std::min(m - 1, k + 4); q++) { sum += raw[q]; cnt++; }
            c[k].y = std::max(sum / cnt, raw[k]);
            r.minx = std::min(r.minx, c[k].x); r.maxx = std::max(r.maxx, c[k].x);
            r.minz = std::min(r.minz, c[k].z); r.maxz = std::max(r.maxz, c[k].z);
        }
        Color col{(uint8_t)T.lanes, (uint8_t)T.flags, (uint8_t)T.klass, (uint8_t)(T.hw * 10)};
        float hw = col.a / 10.0f;
        auto rv3 = [&](Vec3 p, float lat, float along) { rv.push_back({{p.x, p.y, p.z}, {0, 1, 0}, {lat, along}, col}); };
        for (int k = 0; k + 1 < m; k++) {
            Vec3 L0 = c[k] - rt[k] * hw, R0 = c[k] + rt[k] * hw, L1 = c[k + 1] - rt[k + 1] * hw, R1 = c[k + 1] + rt[k + 1] * hw;
            rv3(L0, -hw, dist[k]); rv3(R0, hw, dist[k]); rv3(R1, hw, dist[k + 1]);
            rv3(L0, -hw, dist[k]); rv3(R1, hw, dist[k + 1]); rv3(L1, -hw, dist[k + 1]);
            segs.push_back({c[k].x, c[k].z, c[k + 1].x, c[k + 1].z, hw, T.speed, (int8_t)T.oneway, (uint8_t)T.klass, (uint8_t)T.lanes, (uint8_t)(T.flags & 1), 0});
            for (float s : {-1.0f, 1.0f}) { // Gehsteig, Bordstein und Boeschung/Stuetzmauer
                Vec3 e0 = c[k] + rt[k] * (hw * s), e1 = c[k + 1] + rt[k + 1] * (hw * s), nrm = rt[k] * s;
                float g0 = (s < 0 ? hl[k] : hr[k]) - 0.6f, g1 = (s < 0 ? hl[k + 1] : hr[k + 1]) - 0.6f;
                if (T.sidewalk) {
                    Vec3 up{0, 0.13f, 0}, o0 = c[k] + rt[k] * ((hw + 1.8f) * s), o1 = c[k + 1] + rt[k + 1] * ((hw + 1.8f) * s);
                    solid.quad(e0, e1, e1 + up, e0 + up, -nrm, curb);
                    solid.quad(e0 + up, e1 + up, o1 + up, o0 + up, {0, 1, 0}, walk);
                    e0 = o0 + up; e1 = o1 + up;
                }
                if (e0.y - g0 > 0.2f || e1.y - g1 > 0.2f) solid.quad(e0, e1, {e1.x, std::min(g1, e1.y), e1.z}, {e0.x, std::min(g0, e0.y), e0.z}, nrm, concrete);
            }
        }
        r.line = c;
        r.built = true;
    }
    // Kreuzungen: wo sich Stuetzpunkte zweier Strassen treffen, die Markierungen mit einer Asphaltflaeche ueberdecken
    for (size_t a = 0; a < roads.size(); a++) {
        if (!roads[a].built) continue;
        for (size_t i = 0; i < roads[a].pts.size(); i++) {
            float rad = 0;
            for (size_t b = 0; b < roads.size(); b++) {
                if (b == a || !roads[b].built) continue;
                for (const RPoint& q : roads[b].pts)
                    if (std::hypot(q.x - roads[a].pts[i].x, q.z - roads[a].pts[i].z) < 0.8f) rad = std::max(rad, std::max(TYPES[roads[a].type].hw, TYPES[roads[b].type].hw) * 1.3f);
            }
            if (rad <= 0) continue;
            Vec3 ctr = roads[a].line[roads[a].ptSample[i]] + Vec3{0, 0.02f, 0};
            Color col{1, 0, 3, 255};
            for (int k = 0; k < 20; k++) {
                float a0 = k * 2 * PI / 20, a1 = (k + 1) * 2 * PI / 20;
                Vec3 p0 = ctr + Vec3{std::cos(a0) * rad, 0, std::sin(a0) * rad}, p1 = ctr + Vec3{std::cos(a1) * rad, 0, std::sin(a1) * rad};
                rv.push_back({{ctr.x, ctr.y, ctr.z}, {0, 1, 0}, {0, 0}, col});
                rv.push_back({{p0.x, p0.y, p0.z}, {0, 1, 0}, {0, 0}, col});
                rv.push_back({{p1.x, p1.y, p1.z}, {0, 1, 0}, {0, 0}, col});
            }
        }
    }
    world.setCustomRoads(rv, segs);
    solidMesh.create(solid.v.data(), (int)solid.v.size());
    if (incomplete) nextBuild = simTime + 1.0; else dirty = false;
}

// ---------- Hilfen fuer Maus und Kamera ----------
Vec3 unproject(float nx, float ny, float nz, const Mat4& inv) {
    const float* m = inv.m;
    float x = m[0] * nx + m[4] * ny + m[8] * nz + m[12], y = m[1] * nx + m[5] * ny + m[9] * nz + m[13];
    float z = m[2] * nx + m[6] * ny + m[10] * nz + m[14], w = m[3] * nx + m[7] * ny + m[11] * nz + m[15];
    return {x / w, y / w, z / w};
}
bool project(Vec3 p, float& sx, float& sy) {
    const float* m = lastVP.m;
    float x = m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12], y = m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13];
    float w = m[3] * p.x + m[7] * p.y + m[11] * p.z + m[15];
    if (w < 0.05f) return false;
    sx = (x / w * 0.5f + 0.5f) * vw; sy = (1.0f - (y / w * 0.5f + 0.5f)) * vh;
    return true;
}
void mouseRay(Vec3& ro, Vec3& rd) {
    Mat4 inv = invert(lastVP);
    float nx = mouseX / vw * 2 - 1, ny = 1 - mouseY / vh * 2;
    ro = unproject(nx, ny, -1, inv);
    rd = normalize(unproject(nx, ny, 1, inv) - ro);
}
bool rayGround(Vec3 ro, Vec3 rd, Vec3& hit, bool withObjects) {
    float t = 0.5f, prev = 0;
    for (int i = 0; i < 400 && t < 4000; i++) {
        Vec3 p = ro + rd * t;
        float h;
        bool ok = withObjects ? world.surface(p.x, p.z, 1e9f, h) : world.height(p.x, p.z, h);
        if (ok && p.y < h) {
            float a = prev, b = t;
            for (int k = 0; k < 10; k++) {
                float mid = (a + b) * 0.5f;
                Vec3 q = ro + rd * mid;
                float hh = h;
                if (withObjects) world.surface(q.x, q.z, 1e9f, hh); else world.height(q.x, q.z, hh);
                if (q.y < hh) b = mid; else a = mid;
            }
            hit = ro + rd * b;
            return true;
        }
        prev = t;
        t += std::max(0.5f, t * 0.03f);
    }
    return false;
}
bool rayObj(Vec3 ro, Vec3 rd, const Obj& o, float& tHit) {
    Quat qi = qconj(rotOf(o));
    Vec3 lo, hi, p = qrot(qi, ro - o.pos), d = qrot(qi, rd);
    boundsOf(o, lo, hi);
    float t0 = 0, t1 = 1e9f;
    const float pp[3] = {p.x, p.y, p.z}, dd[3] = {d.x, d.y, d.z}, l[3] = {lo.x, lo.y, lo.z}, h[3] = {hi.x, hi.y, hi.z};
    for (int a = 0; a < 3; a++) {
        if (std::fabs(dd[a]) < 1e-7f) { if (pp[a] < l[a] || pp[a] > h[a]) return false; continue; }
        float ta = (l[a] - pp[a]) / dd[a], tb = (h[a] - pp[a]) / dd[a];
        if (ta > tb) std::swap(ta, tb);
        t0 = std::max(t0, ta); t1 = std::min(t1, tb);
        if (t0 > t1) return false;
    }
    tHit = t0;
    return true;
}
void syncGeo(Obj& o) { world.toLatLon(o.pos.x, o.pos.z, o.lat, o.lon); o.alt = o.pos.y; }
Vec3 axisDir(int a, const Obj* o) {
    Vec3 d = a == 0 ? Vec3{1, 0, 0} : a == 1 ? Vec3{0, 1, 0} : Vec3{0, 0, 1};
    return (o && tool == 2) ? qrot(rotOf(*o), d) : d;
}
float gizmoLen(Vec3 p) { return std::max(1.0f, length(p - camP) * 0.16f); }
float distToSeg(float px, float py, float ax, float ay, float bx, float by) {
    float vx = bx - ax, vy = by - ay, l2 = vx * vx + vy * vy;
    float t = l2 > 0 ? clampv(((px - ax) * vx + (py - ay) * vy) / l2, 0.0f, 1.0f) : 0;
    return std::hypot(px - (ax + vx * t), py - (ay + vy * t));
}
Vec3 pointPos(const RPoint& p) { float h = 0; world.height(p.x, p.z, h); return {p.x, h + p.lift + 0.6f, p.z}; }

void insertObject(int shape, const std::string& model) {
    pushUndo();
    Obj o;
    o.shape = shape; o.model = model;
    if (shape == 2) o.scale = {4, 1, 6};
    if (shape == 4) o.scale = {1, 1, 1};
    Mat4 cm = editor::camera();
    Vec3 ro = camP, rd = normalize(xformDir(cm, {0, 0, -1})), hit;
    if (!rayGround(ro, rd, hit, true) || length(hit - ro) > 150) { hit = ro + rd * 15; float h; if (world.height(hit.x, hit.z, h)) hit.y = h; }
    Vec3 lo, hi;
    boundsOf(o, lo, hi);
    o.pos = {hit.x, hit.y - lo.y, hit.z};
    o.rot.x = camYaw;
    syncGeo(o);
    objs.push_back(o);
    sel = (int)objs.size() - 1; selRoad = selPt = -1;
    if (tool == 3) tool = 0;
}
} // namespace

// ---------- Oeffentliche Schnittstelle ----------
namespace editor {

void init() {
    uint8_t w[3] = {255, 255, 255};
    whiteTex = gfxTexture(w, 1, 1, 3, false);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    makeShapes();
    genTextures();
    scanAssets();
    std::vector<uint8_t> data;
    if (readFile(MAP_FILE, data)) {
        json j = json::parse(data.begin(), data.end(), nullptr, false);
        if (!j.is_discarded()) deserialize(j);
        logMsg("Karte geladen: %d Objekte, %d Strassen", (int)objs.size(), (int)roads.size());
    }
    world.surfaceHook = [](float x, float z, float yRef, float& h) { return surface(x, z, yRef, h); };
}

void save() {
    std::string s = serialize().dump(1);
    writeFile(MAP_FILE, std::vector<uint8_t>(s.begin(), s.end()));
    modified = false;
    say("Gespeichert: maps/welt.json");
}

bool active() { return on; }
void open(Vec3 at, float yaw) {
    on = true;
    camP = at + Vec3{std::sin(yaw) * 10, 7, std::cos(yaw) * 10};
    camYaw = yaw; camPitch = -0.45f;
    scanAssets();
}
void setCamera(Vec3 p, float yaw, float pitch) { camP = p; camYaw = yaw; camPitch = pitch; }
void close() { on = false; drawing = false; dragAxis = -1; dragBody = dragPoint = false; }
Mat4 camera() { return mul(translate(camP), mul(fromQuatPos(qaxis({0, 1, 0}, camYaw), {}), fromQuatPos(qaxis({1, 0, 0}, camPitch), {}))); }
Vec3 cameraPos() { return camP; }

bool wantsTestDrive(Vec3& pos, float& yaw) {
    if (!testDrive) return false;
    testDrive = false;
    Vec3 rd = normalize(xformDir(camera(), {0, 0, -1})), hit;
    pos = rayGround(camP, rd, hit, true) && length(hit - camP) < 300 ? hit : camP;
    yaw = camYaw;
    return true;
}

void refresh(double now) {
    if (geoGen != world.geo().gen) {
        geoGen = world.geo().gen;
        for (Obj& o : objs) { toLocal(o.lat, o.lon, o.pos.x, o.pos.z); o.pos.y = o.alt; }
        for (Road& r : roads) { r.built = false; for (RPoint& p : r.pts) toLocal(p.lat, p.lon, p.x, p.z); }
        dirty = true; nextBuild = 0;
    }
    if (dirty && now >= nextBuild) buildRoads();
}

bool surface(float x, float z, float yRef, float& h) {
    float best = -1e9f;
    for (const Road& r : roads) {
        if (!r.built) continue;
        const RoadType& T = TYPES[r.type];
        float ext = T.hw + (T.sidewalk ? 1.8f : 0.3f);
        if (x < r.minx - ext || x > r.maxx + ext || z < r.minz - ext || z > r.maxz + ext) continue;
        for (size_t k = 0; k + 1 < r.line.size(); k++) {
            Vec3 a = r.line[k], b = r.line[k + 1];
            float vx = b.x - a.x, vz = b.z - a.z, l2 = vx * vx + vz * vz;
            if (l2 < 1e-6f) continue;
            float t = clampv(((x - a.x) * vx + (z - a.z) * vz) / l2, 0.0f, 1.0f);
            float dd = std::hypot(x - (a.x + vx * t), z - (a.z + vz * t));
            if (dd > ext) continue;
            float y = a.y + (b.y - a.y) * t + (dd > T.hw ? 0.13f : 0.0f);
            if (y <= yRef && y > best) best = y;
        }
    }
    for (const Obj& o : objs) {
        if (!o.solid || o.shape >= 3 || std::fabs(o.rot.y) > 0.05f || std::fabs(o.rot.z) > 0.05f) continue;
        if (std::fabs(x - o.pos.x) > o.scale.x + o.scale.z || std::fabs(z - o.pos.z) > o.scale.x + o.scale.z) continue;
        Vec3 l = qrot(qaxis({0, 1, 0}, -o.rot.x), {x - o.pos.x, 0, z - o.pos.z});
        float u = l.x / o.scale.x, w = l.z / o.scale.z;
        if (o.shape == 1 ? (u * u + w * w > 0.25f) : (std::fabs(u) > 0.5f || std::fabs(w) > 0.5f)) continue;
        float y = o.shape == 2 ? o.pos.y - o.scale.y * 0.5f + o.scale.y * (0.5f - w) : o.pos.y + o.scale.y * 0.5f;
        if (y <= yRef && y > best) best = y;
    }
    h = best;
    return best > -1e8f;
}

bool blocked(float x, float z, float y) {
    float feet = y - 0.54f;
    for (const Obj& o : objs) {
        if (!o.solid) continue;
        Vec3 lo, hi;
        boundsOf(o, lo, hi);
        float reach = std::max(hi.x - lo.x, hi.z - lo.z) + 2;
        if (std::fabs(x - o.pos.x) > reach || std::fabs(z - o.pos.z) > reach) continue;
        Vec3 l = qrot(qaxis({0, 1, 0}, -o.rot.x), {x - o.pos.x, 0, z - o.pos.z});
        const float m = 0.7f;
        if (l.x < lo.x - m || l.x > hi.x + m || l.z < lo.z - m || l.z > hi.z + m) continue;
        float top = o.pos.y + hi.y, bottom = o.pos.y + lo.y;
        if (o.shape == 2) top = o.pos.y + lo.y + (hi.y - lo.y) * clampv(0.5f - l.z / o.scale.z, 0.0f, 1.0f);
        if (top > feet + 0.4f && bottom < y + 0.8f) return true;
    }
    return false;
}

void event(const SDL_Event& e) {
    if (e.type == SDL_EVENT_MOUSE_MOTION) {
        mouseX = e.motion.x; mouseY = e.motion.y;
        if (rmb) {
            camYaw -= e.motion.xrel * 0.0035f;
            camPitch = clampv(camPitch - e.motion.yrel * 0.0035f, -1.5f, 1.5f);
            if (std::fabs(e.motion.xrel) + std::fabs(e.motion.yrel) > 2) rmbMoved = true;
        }
    } else if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
        mouseX = e.button.x; mouseY = e.button.y;
        if (e.button.button == SDL_BUTTON_LEFT) { lmbPressed = true; lmbDown = true; }
        if (e.button.button == SDL_BUTTON_RIGHT) { rmb = true; rmbMoved = false; }
    } else if (e.type == SDL_EVENT_MOUSE_BUTTON_UP) {
        if (e.button.button == SDL_BUTTON_LEFT) { lmbDown = false; dragAxis = -1; dragBody = dragPoint = false; }
        if (e.button.button == SDL_BUTTON_RIGHT) {
            rmb = false;
            if (!rmbMoved && drawing) { drawing = false; if (!roads.empty() && roads.back().pts.size() < 2) roads.pop_back(); dirty = true; nextBuild = 0; }
        }
    } else if (e.type == SDL_EVENT_MOUSE_WHEEL) {
        camP += normalize(xformDir(camera(), {0, 0, -1})) * (e.wheel.y * 4.0f);
    } else if (e.type == SDL_EVENT_KEY_DOWN) {
        bool ctrl = (e.key.mod & SDL_KMOD_CTRL) != 0;
        SDL_Keycode k = e.key.key;
        if (ctrl && k == SDLK_S) save();
        else if (ctrl && k == SDLK_Z) undo();
        else if (ctrl && k == SDLK_D && sel >= 0) {
            pushUndo();
            Obj o = objs[sel];
            o.pos += Vec3{o.scale.x + 0.5f, 0, 0};
            syncGeo(o);
            objs.push_back(o);
            sel = (int)objs.size() - 1;
        } else if (k == SDLK_DELETE || k == SDLK_BACKSPACE) {
            if (drawing && !roads.empty()) { roads.back().pts.pop_back(); if (roads.back().pts.empty()) { roads.pop_back(); drawing = false; } dirty = true; nextBuild = 0; }
            else if (sel >= 0) { pushUndo(); objs.erase(objs.begin() + sel); sel = -1; }
            else if (selRoad >= 0 && selPt >= 0) {
                pushUndo();
                Road& r = roads[selRoad];
                r.pts.erase(r.pts.begin() + selPt);
                if (r.pts.size() < 2) roads.erase(roads.begin() + selRoad);
                selRoad = selPt = -1; dirty = true; nextBuild = 0;
            }
        } else if (k == SDLK_RETURN && drawing) {
            drawing = false;
            if (!roads.empty() && roads.back().pts.size() < 2) roads.pop_back();
            dirty = true; nextBuild = 0;
        } else if (k == SDLK_PAGEUP || k == SDLK_PAGEDOWN) {
            float d = k == SDLK_PAGEUP ? 0.25f : -0.25f;
            if (sel >= 0) { pushUndo(); objs[sel].pos.y += d; syncGeo(objs[sel]); }
            else if (selRoad >= 0 && selPt >= 0) { pushUndo(); roads[selRoad].pts[selPt].lift = std::max(0.0f, roads[selRoad].pts[selPt].lift + d * 2); dirty = true; nextBuild = 0; }
        } else if (!ctrl && k >= SDLK_1 && k <= SDLK_4) tool = k - SDLK_1;
    }
}

void update(float dt, const Mat4& vp, float ww, float wh) {
    lastVP = vp; vw = ww; vh = wh;
    const bool* keys = SDL_GetKeyboardState(nullptr);
    if (!keys[SDL_SCANCODE_LCTRL] && !keys[SDL_SCANCODE_RCTRL]) {
        Mat4 cm = camera();
        Vec3 f = normalize(xformDir(cm, {0, 0, -1})), r = normalize(xformDir(cm, {1, 0, 0}));
        float sp = (keys[SDL_SCANCODE_LSHIFT] ? 60.0f : 14.0f) * dt;
        if (keys[SDL_SCANCODE_W]) camP += f * sp;
        if (keys[SDL_SCANCODE_S]) camP += f * -sp;
        if (keys[SDL_SCANCODE_D]) camP += r * sp;
        if (keys[SDL_SCANCODE_A]) camP += r * -sp;
        if (keys[SDL_SCANCODE_E]) camP.y += sp;
        if (keys[SDL_SCANCODE_Q]) camP.y -= sp;
    }
    float gh;
    if (world.height(camP.x, camP.z, gh) && camP.y < gh + 0.6f) camP.y = gh + 0.6f;

    float dmx = mouseX - lastMX, dmy = mouseY - lastMY;
    lastMX = mouseX; lastMY = mouseY;
    bool click = lmbPressed && !ui.captured;
    lmbPressed = false;
    Vec3 ro, rd, hit;
    mouseRay(ro, rd);

    if (click) {
        if (tool == 3) { // Strasse: Punkt setzen
            if (rayGround(ro, rd, hit, false)) {
                RPoint np{0, 0, 0, hit.x, hit.z};
                int snapR = -1, snapP = -1;
                for (size_t a = 0; a < roads.size(); a++)
                    for (size_t i = 0; i < roads[a].pts.size(); i++)
                        if (!(drawing && a + 1 == roads.size() && i + 1 == roads[a].pts.size()) && std::hypot(roads[a].pts[i].x - hit.x, roads[a].pts[i].z - hit.z) < 6.0f) { snapR = (int)a; snapP = (int)i; }
                if (snapR >= 0) np = roads[snapR].pts[snapP];
                else world.toLatLon(hit.x, hit.z, np.lat, np.lon);
                pushUndo();
                if (!drawing && snapR >= 0 && snapP == (int)roads[snapR].pts.size() - 1) { // bestehende Strasse verlaengern
                    std::swap(roads[snapR], roads.back());
                    drawing = true;
                } else {
                    if (!drawing) { Road r; r.type = roadType; roads.push_back(r); drawing = true; }
                    roads.back().pts.push_back(np);
                }
                dirty = true; nextBuild = 0;
            }
        } else {
            bool handled = false;
            if (sel >= 0) { // Achsengriff getroffen?
                Vec3 c = objs[sel].pos;
                float L = gizmoLen(c), sx, sy, tx, ty;
                for (int a = 0; a < 3 && !handled; a++)
                    if (project(c, sx, sy) && project(c + axisDir(a, &objs[sel]) * L, tx, ty) && distToSeg(mouseX, mouseY, sx, sy, tx, ty) < 12) { dragAxis = a; handled = true; pushUndo(); }
            }
            if (!handled) { // Strassen-Stuetzpunkt?
                float best = 16;
                for (size_t a = 0; a < roads.size(); a++)
                    for (size_t i = 0; i < roads[a].pts.size(); i++) {
                        float sx, sy;
                        if (project(pointPos(roads[a].pts[i]), sx, sy) && std::hypot(sx - mouseX, sy - mouseY) < best) { best = std::hypot(sx - mouseX, sy - mouseY); selRoad = (int)a; selPt = (int)i; handled = true; }
                    }
                if (handled) { sel = -1; dragPoint = true; pushUndo(); }
            }
            if (!handled) { // Objekt?
                float bestT = 1e9f;
                int pick = -1;
                for (size_t i = 0; i < objs.size(); i++) { float t; if (rayObj(ro, rd, objs[i], t) && t < bestT) { bestT = t; pick = (int)i; } }
                sel = pick; selRoad = selPt = -1;
                if (pick >= 0 && tool == 0) {
                    dragBody = true;
                    pushUndo();
                    float th = objs[pick].pos.y;
                    world.height(objs[pick].pos.x, objs[pick].pos.z, th);
                    dragOffset = objs[pick].pos.y - th;
                }
            }
        }
    }
    bool snap = keys[SDL_SCANCODE_LCTRL] || keys[SDL_SCANCODE_RCTRL];
    if (lmbDown && sel >= 0 && dragAxis >= 0 && (dmx != 0 || dmy != 0)) {
        Obj& o = objs[sel];
        Vec3 ax = axisDir(dragAxis, &o);
        float L = gizmoLen(o.pos), sx, sy, tx, ty;
        if (project(o.pos, sx, sy) && project(o.pos + ax * L, tx, ty)) {
            float ax2 = (tx - sx) * (tx - sx) + (ty - sy) * (ty - sy);
            float d = ax2 > 1 ? (dmx * (tx - sx) + dmy * (ty - sy)) / ax2 * L : 0;
            if (tool == 0) o.pos += ax * d;
            else if (tool == 2) { float* s = dragAxis == 0 ? &o.scale.x : dragAxis == 1 ? &o.scale.y : &o.scale.z; *s = std::max(0.05f, *s + d * 2); }
            else { float* r = dragAxis == 1 ? &o.rot.x : dragAxis == 0 ? &o.rot.y : &o.rot.z; *r += dmx * 0.01f; }
            syncGeo(o);
        }
    } else if (lmbDown && dragBody && sel >= 0 && (dmx != 0 || dmy != 0) && rayGround(ro, rd, hit, false)) {
        Obj& o = objs[sel];
        if (snap) { hit.x = std::round(hit.x * 2) / 2; hit.z = std::round(hit.z * 2) / 2; }
        float th = hit.y;
        world.height(hit.x, hit.z, th);
        o.pos = {hit.x, th + dragOffset, hit.z};
        syncGeo(o);
    } else if (lmbDown && dragPoint && selRoad >= 0 && selPt >= 0 && (dmx != 0 || dmy != 0) && rayGround(ro, rd, hit, false)) {
        RPoint& p = roads[selRoad].pts[selPt];
        p.x = hit.x; p.z = hit.z;
        world.toLatLon(hit.x, hit.z, p.lat, p.lon);
        dirty = true; nextBuild = 0;
    }
    if (!lmbDown && sel >= 0 && snap && tool == 1) { // Drehung auf 15 Grad einrasten
        Obj& o = objs[sel];
        const float st = PI / 12;
        o.rot = {std::round(o.rot.x / st) * st, std::round(o.rot.y / st) * st, std::round(o.rot.z / st) * st};
    }
}

void buildUI() {
    float W = ui.W, H = ui.H;
    // Kopfleiste
    ui.panel(0, 0, W, 46, Color{14, 16, 22, 240}, 0);
    ui.label(14, 30, 20, col::accent, "worldridr Studio");
    float x = 190;
    if (ui.button(x, 7, 104, 32, modified ? "Speichern *" : "Speichern")) save();
    x += 112;
    if (ui.button(x, 7, 96, 32, "Testfahrt")) testDrive = true;
    x += 104;
    if (ui.button(x, 7, 112, 32, "R\xC3\xBC" "ckg\xC3\xA4ngig")) undo();
    x += 140;
    const char* tools[4] = {"Bewegen", "Drehen", "Skalieren", "Stra\xC3\x9F" "e"};
    for (int i = 0; i < 4; i++) {
        if (ui.button(x, 7, 100, 32, std::string(1, (char)('1' + i)) + "  " + tools[i], tool == i)) { tool = i; if (i != 3) drawing = false; }
        x += 106;
    }
    // Linke Leiste: Einfuegen oder Strassentypen
    float lh = tool == 3 ? 76 + NTYPES * 38.0f : 76 + (4 + modelList.size()) * 38.0f;
    ui.panel(10, 58, 232, lh);
    ui.label(24, 86, 17, col::accent, tool == 3 ? "Stra\xC3\x9F" "entyp" : "Einf\xC3\xBC" "gen");
    float y = 100;
    if (tool == 3) {
        for (int i = 0; i < NTYPES; i++) { if (ui.button(20, y, 212, 32, TYPES[i].name, roadType == i, 15)) roadType = i; y += 38; }
    } else {
        for (int i = 0; i < 4; i++) { if (ui.button(20, y, 212, 32, SHAPE_NAMES[i])) insertObject(i, ""); y += 38; }
        for (const std::string& m : modelList) { if (ui.button(20, y, 212, 32, m, false, 14)) insertObject(4, m); y += 38; }
    }
    ui.label(24, y + 14, 12, col::gray, tool == 3 ? "Klick = Punkt, Enter/Rechtsklick = fertig" : "Modelle: .obj nach assets/models");
    // Rechte Leiste: Eigenschaften
    char buf[160];
    if (sel >= 0 && sel < (int)objs.size()) {
        Obj& o = objs[sel];
        float px = W - 282, ph = 372 + textureList.size() * 26.0f;
        ui.panel(px, 58, 272, ph);
        ui.label(px + 14, 86, 17, col::accent, std::string("Eigenschaften: ") + (o.shape == 4 ? o.model : SHAPE_NAMES[o.shape]));
        snprintf(buf, sizeof buf, "Gr\xC3\xB6\xC3\x9F" "e  %.2f x %.2f x %.2f m", o.scale.x, o.scale.y, o.scale.z);
        ui.label(px + 14, 110, 14, col::white, buf);
        snprintf(buf, sizeof buf, "H\xC3\xB6he %.2f m   Drehung %.0f\xC2\xB0", o.pos.y, o.rot.x * 180 / PI);
        ui.label(px + 14, 130, 14, col::white, buf);
        const char* dims[3] = {"Breite", "H\xC3\xB6he", "L\xC3\xA4nge"};
        float* sv[3] = {&o.scale.x, &o.scale.y, &o.scale.z};
        for (int i = 0; i < 3; i++) {
            float yy = 142 + i * 32.0f;
            ui.label(px + 14, yy + 21, 14, col::gray, dims[i]);
            if (ui.button(px + 96, yy, 36, 26, "-")) { pushUndo(); *sv[i] = std::max(0.1f, *sv[i] - 0.5f); }
            if (ui.button(px + 138, yy, 36, 26, "+")) { pushUndo(); *sv[i] += 0.5f; }
            if (ui.button(px + 180, yy, 36, 26, "x2", false, 13)) { pushUndo(); *sv[i] *= 2; }
        }
        ui.label(px + 14, 256, 14, col::gray, "Farbe");
        static const Color pal[12] = {{235, 235, 235, 255}, {150, 150, 150, 255}, {60, 60, 64, 255}, {200, 60, 50, 255}, {230, 140, 40, 255}, {235, 205, 60, 255},
                                      {80, 170, 80, 255}, {50, 130, 200, 255}, {110, 80, 180, 255}, {140, 95, 60, 255}, {215, 195, 160, 255}, {20, 20, 22, 255}};
        for (int i = 0; i < 12; i++) {
            float bx = px + 14 + (i % 6) * 41.0f, byy = 264 + (i / 6) * 30.0f;
            ui.geo.rrect(bx, byy, 36, 25, 5, pal[i]);
            if (ui.over(bx, byy, 36, 25)) { ui.captured = true; if (ui.clicked) { pushUndo(); o.color = pal[i]; ui.clicked = false; } }
        }
        if (ui.button(px + 14, 332, 118, 28, o.solid ? "Fest: ja" : "Fest: nein", o.solid, 14)) { pushUndo(); o.solid = !o.solid; }
        if (ui.button(px + 140, 332, 118, 28, "L\xC3\xB6schen", false, 14)) { pushUndo(); objs.erase(objs.begin() + sel); sel = -1; return; }
        snprintf(buf, sizeof buf, "Textur (Kachel %.1f m)", o.tile);
        ui.label(px + 14, 386, 14, col::gray, buf);
        if (ui.button(px + 190, 368, 30, 24, "-")) o.tile = std::max(0.25f, o.tile * 0.5f);
        if (ui.button(px + 226, 368, 30, 24, "+")) o.tile = std::min(64.0f, o.tile * 2);
        float ty = 398;
        if (ui.button(px + 14, ty, 244, 22, "keine", o.texture.empty(), 13)) { pushUndo(); o.texture.clear(); }
        for (const std::string& t : textureList) {
            ty += 26;
            if (ui.button(px + 14, ty, 244, 22, t, o.texture == t, 13)) { pushUndo(); o.texture = t; }
        }
    } else if (selRoad >= 0 && selRoad < (int)roads.size() && selPt >= 0) {
        float px = W - 282;
        ui.panel(px, 58, 272, 150);
        ui.label(px + 14, 86, 17, col::accent, "Stra\xC3\x9F" "enpunkt");
        ui.label(px + 14, 110, 14, col::white, TYPES[roads[selRoad].type].name);
        snprintf(buf, sizeof buf, "Anhebung %.1f m (Br\xC3\xBC" "cke/Rampe)", roads[selRoad].pts[selPt].lift);
        ui.label(px + 14, 132, 14, col::white, buf);
        if (ui.button(px + 14, 144, 76, 28, "- 0,5 m", false, 14)) { pushUndo(); roads[selRoad].pts[selPt].lift = std::max(0.0f, roads[selRoad].pts[selPt].lift - 0.5f); dirty = true; nextBuild = 0; }
        if (ui.button(px + 96, 144, 76, 28, "+ 0,5 m", false, 14)) { pushUndo(); roads[selRoad].pts[selPt].lift += 0.5f; dirty = true; nextBuild = 0; }
        if (ui.button(px + 178, 144, 80, 28, "Typ >", false, 14)) { pushUndo(); roads[selRoad].type = (roads[selRoad].type + 1) % NTYPES; dirty = true; nextBuild = 0; }
        ui.label(px + 14, 194, 12, col::gray, "Ziehen = verschieben, Entf = Punkt l\xC3\xB6schen");
    }
    // Fusszeile
    ui.panel(0, H - 30, W, 30, Color{14, 16, 22, 230}, 0);
    std::string hint = simTime < statusUntil ? statusMsg
        : "Rechte Maustaste + WASD/QE = fliegen  |  Shift = schnell  |  Klick = ausw\xC3\xA4hlen, ziehen = verschieben  |  Strg+D duplizieren  |  Bild auf/ab = H\xC3\xB6he  |  Strg+S speichern  |  Esc = Men\xC3\xBC";
    ui.label(12, H - 10, 13, simTime < statusUntil ? col::accent : col::gray, hint);
    snprintf(buf, sizeof buf, "%d Objekte, %d Stra\xC3\x9F" "en", (int)objs.size(), (int)roads.size());
    ui.labelR(W - 12, H - 10, 13, col::gray, buf);
}

void drawWorld() {
    gfxModel(identity());
    gfxMode(0, FLAG_LIT | FLAG_FOG);
    solidMesh.draw();
    gfxMode(7);
    for (const Obj& o : objs) {
        if (std::fabs(o.pos.x) > 12000 || std::fabs(o.pos.z) > 12000) continue;
        gfxModel(matOf(o));
        gfxTint(o.color.r / 255.0f, o.color.g / 255.0f, o.color.b / 255.0f);
        gfxTile(o.tile);
        gfxBindTexture(texOf(o.texture));
        if (o.shape == 4) modelOf(o.model).mesh.draw(); else shapes[o.shape].draw();
    }
    gfxModel(identity());
}

void drawGizmos(const Mat4& vp) {
    gizmo.clear();
    Mat4 I = identity();
    Color yellow{255, 210, 60, 255};
    if (sel >= 0 && sel < (int)objs.size()) {
        const Obj& o = objs[sel];
        Vec3 lo, hi;
        boundsOf(o, lo, hi);
        Mat4 R = fromQuatPos(rotOf(o), o.pos);
        float t = gizmoLen(o.pos) * 0.012f;
        Vec3 c[8];
        for (int i = 0; i < 8; i++) c[i] = {(i & 1) ? hi.x : lo.x, (i & 2) ? hi.y : lo.y, (i & 4) ? hi.z : lo.z};
        const int ed[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
        for (auto& e : ed) gizmo.beam(R, c[e[0]], c[e[1]], t, yellow);
        float L = gizmoLen(o.pos);
        const Color ac[3] = {{240, 70, 60, 255}, {90, 220, 90, 255}, {80, 140, 255, 255}};
        for (int a = 0; a < 3; a++) {
            Vec3 d = axisDir(a, &o);
            gizmo.beam(I, o.pos, o.pos + d * L, L * (dragAxis == a ? 0.035f : 0.022f), ac[a]);
            gizmo.box(I, o.pos + d * L, {L * 0.06f, L * 0.06f, L * 0.06f}, ac[a]);
        }
    }
    for (size_t a = 0; a < roads.size(); a++)
        for (size_t i = 0; i < roads[a].pts.size(); i++) {
            Vec3 p = pointPos(roads[a].pts[i]);
            if (length(p - camP) > 900) continue;
            bool s = (int)a == selRoad && (int)i == selPt;
            float sz = std::max(0.25f, length(p - camP) * 0.008f) * (s ? 1.6f : 1.0f);
            gizmo.box(I, p, {sz, sz, sz}, s ? yellow : Color{90, 200, 255, 255});
            if (i > 0) gizmo.beam(I, pointPos(roads[a].pts[i - 1]), p, sz * 0.25f, Color{90, 200, 255, 255});
        }
    glDisable(GL_DEPTH_TEST);
    gfxModel(I);
    gfxMVP(vp);
    gfxMode(0, 0);
    gizmo.draw();
    glEnable(GL_DEPTH_TEST);
}

} // namespace editor
