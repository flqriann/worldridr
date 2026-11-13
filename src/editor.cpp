// Welt-Editor: Objekte platzieren (Block, Zylinder, Keil, Kugel, OBJ-Modelle), faerben, texturieren,
// Strassen mit Kurven bauen, alles in maps/welt.json speichern. Positionen sind Geokoordinaten.
#include "game.h"
#include "net.h"
#include "editor_mesh.h"
#include <memory>
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
const char* SHAPE_NAMES[] = {"Block", "Zylinder", "Keil (Rampe)", "Kugel", "Modell", "Mesh"};

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
    std::string name;
    EMesh mesh;            // shape 5: frei bearbeitbares Mesh (Eckpunkte und Flaechen mit eigenen Farben)
    std::shared_ptr<Mesh> emesh;
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

// ---------- Studio-Zustand: Mesh-Bearbeitung, Raster, Farben, Explorer ----------
std::vector<Color> customPal;                 // eigene Lieblingsfarben (werden in der Karte gespeichert)
std::vector<std::string> redoStack;
bool editMode = false;                        // Mesh-Bearbeitung (Tab) am ausgewaehlten Objekt
int pickMode = 2;                             // 0 Eckpunkt, 1 Kante, 2 Flaeche
std::vector<char> selV, selF;                 // ausgewaehlte Eckpunkte / Flaechen des bearbeiteten Meshes
struct Modal {                                // Blender-artiges Verschieben / Drehen / Skalieren (G, R, S)
    bool active = false;
    int type = 0, axis = -1;                  // type 0 G, 1 R, 2 S; axis -1 frei, 0..2 x/y/z, 3 Flaechennormale
    Vec3 dirLocal{0, 1, 0};
    std::vector<int> ids;
    std::vector<Vec3> start;
    Vec3 pivot;
    float sx = 0, sy = 0;
    bool drag = false, pushed = true;         // drag: Pfeil mit gedrueckter Maustaste gezogen; pushed: Undo-Schritt gehoert dazu
    std::string num, info;                    // getippte Zahl, Anzeige
    Vec3 pivotW, hit0, normalW;
} mo;
bool lmbReleased = false;
Vec3 dragStart;                               // Objekt-Gizmo: Ausgangswert beim Ziehen
float dragAcc = 0;
float gridStep = 0.5f;
bool gridOn = true;
Color brush{205, 205, 205, 255};              // aktuelle Farbe (Objekt oder ausgewaehlte Flaechen)
int explorerScroll = 0;
bool boxSelecting = false, mmb = false, dragMoved = false;
float boxX0 = 0, boxY0 = 0;
int nameCounter = 0;

void rebuildMesh(Obj& o) {
    std::vector<Vertex> v;
    emesh::triangulate(o.mesh, v);
    o.emesh.reset(new Mesh, [](Mesh* m) { m->destroy(); delete m; });
    o.emesh->create(v.data(), (int)v.size());
}
// Block/Zylinder/Keil/Kugel in ein Mesh umwandeln (Skalierung wird eingebacken), damit man Flaechen und Eckpunkte bearbeiten kann
bool convertToMesh(Obj& o) {
    if (o.shape == 5) return true;
    if (o.shape == 4) return false;
    EMesh m;
    if (o.shape == 0) m = emesh::block(o.scale);
    else if (o.shape == 1) m = emesh::cylinder(o.scale);
    else if (o.shape == 2) m = emesh::wedge(o.scale);
    else m = emesh::sphere(o.scale);
    for (EFace& f : m.f) f.c = o.color;
    o.mesh = std::move(m);
    o.shape = 5;
    o.scale = {1, 1, 1};
    rebuildMesh(o);
    return true;
}
std::string newName(const std::string& base) { return base + " " + std::to_string(++nameCounter); }
Vec3 snapV(Vec3 p) {
    if (!gridOn) return p;
    return {std::round(p.x / gridStep) * gridStep, std::round(p.y / gridStep) * gridStep, std::round(p.z / gridStep) * gridStep};
}
float snapF(float v) { return gridOn ? std::round(v / gridStep) * gridStep : v; }
std::string gridLabel() {
    char b[48];
    if (!gridOn) return "Raster: aus";
    snprintf(b, sizeof b, "Raster: %g m", gridStep);
    for (char* p = b; *p; p++) if (*p == '.') *p = ',';
    return b;
}

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
    if (o.shape == 5) emesh::bounds(o.mesh, lo, hi);
    lo = {lo.x * o.scale.x, lo.y * o.scale.y, lo.z * o.scale.z};
    hi = {hi.x * o.scale.x, hi.y * o.scale.y, hi.z * o.scale.z};
}

// ---------- Speichern / Laden ----------
json serialize() {
    json j;
    j["objects"] = json::array();
    for (const Obj& o : objs) {
        json je = {{"shape", o.shape}, {"lat", o.lat}, {"lon", o.lon}, {"alt", o.alt}, {"rot", {o.rot.x, o.rot.y, o.rot.z}},
                   {"scale", {o.scale.x, o.scale.y, o.scale.z}}, {"color", {o.color.r, o.color.g, o.color.b}}, {"texture", o.texture},
                   {"model", o.model}, {"solid", o.solid}, {"tile", o.tile}, {"name", o.name}};
        if (o.shape == 5) { // frei bearbeitbares Mesh: Eckpunkte und Flaechen mit Farbe
            json mv = json::array(), mf = json::array();
            for (const Vec3& p : o.mesh.v) mv.push_back({p.x, p.y, p.z});
            for (const EFace& f : o.mesh.f) mf.push_back({{"i", f.v}, {"c", {f.c.r, f.c.g, f.c.b}}});
            je["mesh"] = {{"v", mv}, {"f", mf}};
        }
        j["objects"].push_back(je);
    }
    j["palette"] = json::array();
    for (const Color& c : customPal) j["palette"].push_back({c.r, c.g, c.b});
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
                o.name = e.value("name", "");
                if (o.shape == 5 && e.contains("mesh")) {
                    for (const json& p : e["mesh"]["v"]) o.mesh.v.push_back({p[0].get<float>(), p[1].get<float>(), p[2].get<float>()});
                    for (const json& fj : e["mesh"]["f"]) {
                        EFace f;
                        f.v = fj["i"].get<std::vector<int>>();
                        f.c = Color{fj["c"][0].get<uint8_t>(), fj["c"][1].get<uint8_t>(), fj["c"][2].get<uint8_t>(), 255};
                        bool ok = f.v.size() >= 3;
                        for (int i : f.v) if (i < 0 || i >= (int)o.mesh.v.size()) ok = false;
                        if (ok) o.mesh.f.push_back(f);
                    }
                    rebuildMesh(o);
                }
                if (o.name.empty()) o.name = newName(o.shape == 4 ? o.model : SHAPE_NAMES[o.shape]);
                objs.push_back(o);
            }
        customPal.clear();
        if (j.contains("palette"))
            for (const json& p : j["palette"]) customPal.push_back(Color{p[0].get<uint8_t>(), p[1].get<uint8_t>(), p[2].get<uint8_t>(), 255});
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
    if (undoStack.size() > 60) undoStack.erase(undoStack.begin());
    redoStack.clear();
    modified = true;
}
void undo() {
    if (undoStack.empty()) return;
    redoStack.push_back(serialize().dump());
    deserialize(json::parse(undoStack.back(), nullptr, false));
    undoStack.pop_back();
    sel = selRoad = selPt = -1; drawing = false; editMode = false; mo.active = false;
    say("R\xC3\xBC" "ckg\xC3\xA4ngig");
}
void redo() {
    if (redoStack.empty()) return;
    undoStack.push_back(serialize().dump());
    deserialize(json::parse(redoStack.back(), nullptr, false));
    redoStack.pop_back();
    sel = selRoad = selPt = -1; drawing = false; editMode = false; mo.active = false;
    say("Wiederholt");
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
Vec3 toWorld(const Obj& o, Vec3 p) { return o.pos + qrot(rotOf(o), Vec3{p.x * o.scale.x, p.y * o.scale.y, p.z * o.scale.z}); }
Vec3 toObj(const Obj& o, Vec3 w) { Vec3 l = qrot(qconj(rotOf(o)), w - o.pos); return {l.x / o.scale.x, l.y / o.scale.y, l.z / o.scale.z}; }
Vec3 dirToObj(const Obj& o, Vec3 d) { Vec3 l = qrot(qconj(rotOf(o)), d); return {l.x / o.scale.x, l.y / o.scale.y, l.z / o.scale.z}; }
bool rayObj(Vec3 ro, Vec3 rd, const Obj& o, float& tHit) {
    if (o.shape == 5) { // Mesh: genau gegen die Flaechen testen
        float t;
        if (emesh::pickFace(o.mesh, toObj(o, ro), dirToObj(o, rd), t) < 0) return false;
        tHit = t;
        return true;
    }
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

const char* MESH_NAMES[] = {"Treppe", "Dach", "Pyramide", "Kegel", "Platte", "L-Winkel"};
const int NMESH = 6;
EMesh makeMesh(int kind) {
    switch (kind) {
    case 0: return emesh::stairs({3, 2, 4});
    case 1: return emesh::roof({4, 2, 6});
    case 2: return emesh::pyramid({3, 3, 3});
    case 3: return emesh::cone({2, 2, 2});
    case 4: return emesh::plate({4, 0.3f, 4});
    default: return emesh::lshape({4, 2, 4});
    }
}
bool shiftHeld() { const bool* k = SDL_GetKeyboardState(nullptr); return k[SDL_SCANCODE_LSHIFT] || k[SDL_SCANCODE_RSHIFT]; }
bool snapOn() { const bool* k = SDL_GetKeyboardState(nullptr); return gridOn != (k[SDL_SCANCODE_LCTRL] || k[SDL_SCANCODE_RCTRL]); }
float snapQ(float v) { return snapOn() ? std::round(v / gridStep) * gridStep : v; }

// Mesh als begehbare / befahrbare Flaeche: Strahl nach unten bzw. oben gegen die Flaechen
bool meshNear(const Obj& o, float x, float z, float& cy, float& R) {
    Vec3 lo, hi;
    boundsOf(o, lo, hi);
    Vec3 c = toWorld(o, Vec3{(lo.x + hi.x) * 0.5f / o.scale.x, (lo.y + hi.y) * 0.5f / o.scale.y, (lo.z + hi.z) * 0.5f / o.scale.z});
    R = 0.5f * length(hi - lo) + 0.5f;
    cy = c.y;
    return std::hypot(x - c.x, z - c.z) <= R;
}
bool meshDown(const Obj& o, float x, float z, float yRef, float& h) {
    float cy, R;
    if (!meshNear(o, x, z, cy, R)) return false;
    yRef = std::min(yRef, cy + R + 1.0f);
    float t;
    if (emesh::pickFace(o.mesh, toObj(o, {x + 0.00137f, yRef, z + 0.00091f}), dirToObj(o, {0, -1, 0}), t) < 0) return false;
    h = yRef - t;
    return true;
}
int meshCountUp(const Obj& o, float x, float y, float z) {
    Vec3 ro = toObj(o, {x + 0.00137f, y, z + 0.00091f}), rd = dirToObj(o, {0, 1, 0});
    int n = 0;
    for (const EFace& f : o.mesh.f)
        for (size_t k = 1; k + 1 < f.v.size(); k++) {
            float t;
            if (emesh::rayTri(ro, rd, o.mesh.v[f.v[0]], o.mesh.v[f.v[k]], o.mesh.v[f.v[k + 1]], t)) n++;
        }
    return n;
}

// ---------- Mesh-Bearbeitung ----------
Obj* editObj() { return (editMode && sel >= 0 && sel < (int)objs.size() && objs[sel].shape == 5) ? &objs[sel] : nullptr; }
void fitSel(Obj& o) { selV.resize(o.mesh.v.size(), 0); selF.resize(o.mesh.f.size(), 0); }
void clearSel(Obj& o) { selV.assign(o.mesh.v.size(), 0); selF.assign(o.mesh.f.size(), 0); }
std::vector<int> activeVerts(const Obj& o) {
    std::vector<int> r;
    if (pickMode == 2) {
        std::vector<char> m(o.mesh.v.size(), 0);
        for (size_t i = 0; i < o.mesh.f.size() && i < selF.size(); i++)
            if (selF[i]) for (int v : o.mesh.f[i].v) m[v] = 1;
        for (size_t i = 0; i < m.size(); i++) if (m[i]) r.push_back((int)i);
    } else
        for (size_t i = 0; i < selV.size() && i < o.mesh.v.size(); i++) if (selV[i]) r.push_back((int)i);
    return r;
}
Vec3 selCenterWorld(const Obj& o) {
    std::vector<int> ids = activeVerts(o);
    if (ids.empty()) return o.pos;
    Vec3 c{};
    for (int i : ids) c += toWorld(o, o.mesh.v[i]);
    return c * (1.0f / (float)ids.size());
}
Vec3 worldAxis(int a) { return a == 0 ? Vec3{1, 0, 0} : a == 1 ? Vec3{0, 1, 0} : Vec3{0, 0, 1}; }
Vec3 camForward() { return normalize(xformDir(editor::camera(), {0, 0, -1})); }

void setPickMode(int m) {
    if (m == pickMode) return;
    if (Obj* o = editObj()) {
        fitSel(*o);
        if (pickMode == 2) { // Flaechen -> Eckpunkte
            std::vector<int> ids = activeVerts(*o);
            selV.assign(o->mesh.v.size(), 0);
            for (int i : ids) selV[i] = 1;
        } else if (m == 2) { // Eckpunkte -> Flaechen, deren Ecken alle gewaehlt sind
            selF.assign(o->mesh.f.size(), 0);
            for (size_t i = 0; i < o->mesh.f.size(); i++) {
                bool all = true;
                for (int v : o->mesh.f[i].v) if (!selV[v]) all = false;
                selF[i] = all;
            }
        }
    }
    pickMode = m;
}
void enterEdit() {
    if (sel < 0 || sel >= (int)objs.size()) return;
    Obj& o = objs[sel];
    if (o.shape == 4) { say("3D-Modelle (.obj) lassen sich nicht bearbeiten"); return; }
    if (o.shape != 5) { pushUndo(); convertToMesh(o); }
    editMode = true; mo.active = false; drawing = false;
    clearSel(o);
}
void leaveEdit() { editMode = false; mo.active = false; }

void applyColor(Color c) {
    brush = c;
    if (sel < 0 || sel >= (int)objs.size()) return;
    Obj& o = objs[sel];
    o.color = c;
    if (o.shape != 5) return;
    fitSel(o);
    bool any = false;
    if (editMode) // gewaehlte Flaechen (bei Punkten/Kanten: alle Flaechen, die einen gewaehlten Punkt nutzen)
        for (size_t i = 0; i < o.mesh.f.size(); i++) {
            bool hit = false;
            if (pickMode == 2) hit = selF[i] != 0;
            else for (int v : o.mesh.f[i].v) if (selV[v]) hit = true;
            if (hit) { o.mesh.f[i].c = c; any = true; }
        }
    if (!any) for (EFace& f : o.mesh.f) f.c = c; // nichts gewaehlt: ganzes Objekt
    rebuildMesh(o);
}

Vec3 modalAxis() {
    if (mo.axis >= 0 && mo.axis < 3) return worldAxis(mo.axis);
    if (mo.axis == 3) return mo.normalW;
    return {0, 0, 0};
}
void modalBegin(int type, int axis, bool drag, bool pushU = true) {
    Obj* o = editObj();
    if (!o) return;
    fitSel(*o);
    std::vector<int> ids = activeVerts(*o);
    if (ids.empty()) { say("Zuerst etwas ausw\xC3\xA4hlen (Klick oder Rahmen ziehen)"); return; }
    if (pushU) pushUndo();
    mo = Modal{};
    mo.active = true; mo.type = type; mo.axis = axis; mo.drag = drag; mo.pushed = pushU; mo.ids = ids;
    for (int i : ids) mo.start.push_back(o->mesh.v[i]);
    mo.pivotW = selCenterWorld(*o);
    mo.sx = mouseX; mo.sy = mouseY;
    Vec3 n{};
    if (pickMode == 2)
        for (size_t i = 0; i < o->mesh.f.size() && i < selF.size(); i++)
            if (selF[i]) n += qrot(rotOf(*o), emesh::faceNormal(o->mesh, (int)i));
    mo.normalW = length(n) > 1e-4f ? normalize(n) : Vec3{0, 1, 0};
    Vec3 ro, rd;
    mouseRay(ro, rd);
    Vec3 f = camForward();
    float den = dot(rd, f);
    mo.hit0 = std::fabs(den) > 1e-4f ? ro + rd * (dot(mo.pivotW - ro, f) / den) : mo.pivotW;
}
Vec3 rotAround(Vec3 v, Vec3 k, float a) { return v * std::cos(a) + cross(k, v) * std::sin(a) + k * (dot(k, v) * (1 - std::cos(a))); }
void modalApply(Obj& o) {
    Vec3 ax = modalAxis(), fwd = camForward();
    bool hasAx = mo.axis >= 0, hasNum = !mo.num.empty();
    float num = 0;
    if (hasNum) { std::string s = mo.num; for (char& c : s) if (c == ',') c = '.'; num = (float)atof(s.c_str()); }
    float psx = 0, psy = 0;
    bool pv = project(mo.pivotW, psx, psy);
    char b[96];
    std::vector<Vec3> w(mo.ids.size());
    for (size_t k = 0; k < mo.ids.size(); k++) w[k] = toWorld(o, mo.start[k]);
    if (mo.type == 0) {
        Vec3 delta{};
        if (hasAx) {
            float d = 0;
            if (hasNum) d = num;
            else {
                float L = std::max(0.5f, length(mo.pivotW - camP) * 0.2f), sx, sy, tx, ty;
                if (project(mo.pivotW, sx, sy) && project(mo.pivotW + ax * L, tx, ty)) {
                    float a2 = (tx - sx) * (tx - sx) + (ty - sy) * (ty - sy);
                    if (a2 > 1) d = ((mouseX - mo.sx) * (tx - sx) + (mouseY - mo.sy) * (ty - sy)) / a2 * L;
                }
                if (snapOn()) {
                    if (mo.axis < 3) { float base = dot(mo.pivotW, ax); d = snapQ(base + d) - base; }
                    else d = snapQ(d);
                }
            }
            delta = ax * d;
            snprintf(b, sizeof b, "Verschieben %.2f m", d);
        } else {
            Vec3 ro, rd;
            mouseRay(ro, rd);
            float den = dot(rd, fwd);
            if (std::fabs(den) > 1e-4f) delta = (ro + rd * (dot(mo.pivotW - ro, fwd) / den)) - mo.hit0;
            if (snapOn()) { Vec3 t = mo.pivotW + delta; delta = Vec3{snapQ(t.x), snapQ(t.y), snapQ(t.z)} - mo.pivotW; }
            snprintf(b, sizeof b, "Verschieben %.2f / %.2f / %.2f", delta.x, delta.y, delta.z);
        }
        for (size_t k = 0; k < w.size(); k++) w[k] += delta;
    } else if (mo.type == 1) {
        Vec3 k = hasAx ? ax : fwd;
        float ang = 0;
        if (hasNum) ang = num * PI / 180;
        else if (pv) {
            float a0 = std::atan2(mo.sy - psy, mo.sx - psx), a1 = std::atan2(mouseY - psy, mouseX - psx);
            ang = a1 - a0;
            while (ang > PI) ang -= 2 * PI;
            while (ang < -PI) ang += 2 * PI;
            if (snapOn()) ang = std::round(ang / (PI / 12)) * (PI / 12);
        }
        float sgn = dot(k, fwd) >= 0 ? 1.0f : -1.0f;
        if (!hasNum) ang *= sgn;
        for (size_t i = 0; i < w.size(); i++) w[i] = mo.pivotW + rotAround(w[i] - mo.pivotW, k, ang);
        snprintf(b, sizeof b, "Drehen %.0f\xC2\xB0", ang * 180 / PI);
    } else {
        float f = 1;
        if (hasNum) f = num;
        else if (pv) {
            float d0 = std::hypot(mo.sx - psx, mo.sy - psy), d1 = std::hypot(mouseX - psx, mouseY - psy);
            f = d0 > 4 ? d1 / d0 : 1;
        }
        for (size_t i = 0; i < w.size(); i++) {
            Vec3 r = w[i] - mo.pivotW;
            if (hasAx) r = r + ax * (dot(r, ax) * (f - 1)); else r = r * f;
            w[i] = mo.pivotW + r;
        }
        snprintf(b, sizeof b, "Skalieren x%.2f", f);
    }
    mo.info = b;
    if (hasNum) mo.info += "   (Zahl: " + mo.num + ")";
    for (size_t k = 0; k < mo.ids.size(); k++) o.mesh.v[mo.ids[k]] = toObj(o, w[k]);
    rebuildMesh(o);
}
void modalFinish(bool ok) {
    Obj* o = editObj();
    if (o && !ok) {
        for (size_t k = 0; k < mo.ids.size(); k++) o->mesh.v[mo.ids[k]] = mo.start[k];
        rebuildMesh(*o);
        if (mo.pushed && !undoStack.empty()) undoStack.pop_back();
    }
    mo.active = false;
}

void editDelete(Obj& o) {
    fitSel(o);
    std::vector<char> del(o.mesh.f.size(), 0);
    bool any = false;
    for (size_t i = 0; i < o.mesh.f.size(); i++) {
        if (pickMode == 2) del[i] = selF[i];
        else for (int v : o.mesh.f[i].v) if (selV[v]) del[i] = 1;
        if (del[i]) any = true;
    }
    if (!any) return;
    pushUndo();
    emesh::deleteFaces(o.mesh, del);
    if (o.mesh.f.empty()) { objs.erase(objs.begin() + sel); sel = -1; leaveEdit(); return; }
    clearSel(o);
    rebuildMesh(o);
}
void editExtrude(Obj& o) {
    fitSel(o);
    if (pickMode != 2) { say("Zum Extrudieren eine Fl\xC3\xA4" "che w\xC3\xA4hlen (Taste 3)"); return; }
    bool any = false;
    for (char s : selF) if (s) any = true;
    if (!any) { say("Zuerst eine Fl\xC3\xA4" "che ausw\xC3\xA4hlen"); return; }
    pushUndo();
    emesh::extrudeFaces(o.mesh, selF);
    fitSel(o);
    rebuildMesh(o);
    modalBegin(0, 3, false, false);
}
void editSubdivide(Obj& o) {
    fitSel(o);
    if (pickMode != 2) { say("Zum Teilen eine Fl\xC3\xA4" "che w\xC3\xA4hlen (Taste 3)"); return; }
    bool any = false;
    for (char s : selF) if (s) any = true;
    if (!any) return;
    pushUndo();
    for (int i = (int)selF.size() - 1; i >= 0; i--) if (selF[i]) emesh::subdivideFace(o.mesh, i);
    clearSel(o);
    rebuildMesh(o);
}
void editFlip(Obj& o) {
    fitSel(o);
    pushUndo();
    bool any = false;
    for (char s : selF) if (s) any = true;
    for (size_t i = 0; i < o.mesh.f.size(); i++) if (!any || (pickMode == 2 && selF[i])) std::reverse(o.mesh.f[i].v.begin(), o.mesh.f[i].v.end());
    rebuildMesh(o);
}
void editMakeFace(Obj& o) {
    fitSel(o);
    std::vector<int> ids;
    for (size_t i = 0; i < selV.size(); i++) if (selV[i]) ids.push_back((int)i);
    if (pickMode == 2 || ids.size() < 3) { say("F: mindestens 3 Eckpunkte w\xC3\xA4hlen (Taste 1)"); return; }
    Vec3 c{};
    for (int i : ids) c += o.mesh.v[i];
    c = c * (1.0f / (float)ids.size());
    Vec3 n{};
    for (size_t i = 0; i + 1 < ids.size() && length(n) < 1e-6f; i++) n = cross(o.mesh.v[ids[i]] - c, o.mesh.v[ids[i + 1]] - c);
    if (length(n) < 1e-6f) return;
    n = normalize(n);
    Vec3 u = normalize(o.mesh.v[ids[0]] - c), wv = cross(n, u);
    std::sort(ids.begin(), ids.end(), [&](int a, int b) {
        Vec3 pa = o.mesh.v[a] - c, pb = o.mesh.v[b] - c;
        return std::atan2(dot(pa, wv), dot(pa, u)) < std::atan2(dot(pb, wv), dot(pb, u));
    });
    pushUndo();
    EFace f;
    f.v = ids; f.c = brush;
    o.mesh.f.push_back(f);
    int fi = (int)o.mesh.f.size() - 1;
    if (dot(emesh::faceNormal(o.mesh, fi), dirToObj(o, camForward())) > 0) std::reverse(o.mesh.f[fi].v.begin(), o.mesh.f[fi].v.end());
    fitSel(o);
    rebuildMesh(o);
}
void editSelectAll(Obj& o) {
    fitSel(o);
    bool any = false;
    for (char s : selV) if (s) any = true;
    for (char s : selF) if (s) any = true;
    char val = any ? 0 : 1;
    std::fill(selV.begin(), selV.end(), val);
    std::fill(selF.begin(), selF.end(), val);
}
// Klick im Bearbeitungsmodus: Eckpunkt / Kante / Flaeche unter der Maus. false = nichts getroffen
bool editClick(Obj& o, Vec3 ro, Vec3 rd, bool add) {
    fitSel(o);
    if (pickMode == 2) {
        float t;
        int fi = emesh::pickFace(o.mesh, toObj(o, ro), dirToObj(o, rd), t);
        if (fi < 0) return false;
        char was = selF[fi];
        if (!add) std::fill(selF.begin(), selF.end(), 0);
        selF[fi] = add ? !was : 1;
        return true;
    }
    if (pickMode == 0) {
        int best = -1;
        float bd = 14;
        for (size_t i = 0; i < o.mesh.v.size(); i++) {
            float sx, sy;
            if (project(toWorld(o, o.mesh.v[i]), sx, sy) && std::hypot(sx - mouseX, sy - mouseY) < bd) { bd = std::hypot(sx - mouseX, sy - mouseY); best = (int)i; }
        }
        if (best < 0) return false;
        char was = selV[best];
        if (!add) std::fill(selV.begin(), selV.end(), 0);
        selV[best] = add ? !was : 1;
        return true;
    }
    int ba = -1, bb = -1;
    float bd = 10;
    for (const EFace& f : o.mesh.f)
        for (size_t k = 0; k < f.v.size(); k++) {
            int a = f.v[k], b = f.v[(k + 1) % f.v.size()];
            float ax, ay, bx, by;
            if (project(toWorld(o, o.mesh.v[a]), ax, ay) && project(toWorld(o, o.mesh.v[b]), bx, by)) {
                float d = distToSeg(mouseX, mouseY, ax, ay, bx, by);
                if (d < bd) { bd = d; ba = a; bb = b; }
            }
        }
    if (ba < 0) return false;
    bool both = selV[ba] && selV[bb];
    if (!add) std::fill(selV.begin(), selV.end(), 0);
    selV[ba] = selV[bb] = add ? !both : 1;
    return true;
}
void boxSelect(Obj& o, float x0, float y0, float x1, float y1, bool add) {
    fitSel(o);
    if (x0 > x1) std::swap(x0, x1);
    if (y0 > y1) std::swap(y0, y1);
    if (!add) { std::fill(selV.begin(), selV.end(), 0); std::fill(selF.begin(), selF.end(), 0); }
    auto inside = [&](Vec3 p) { float sx, sy; return project(toWorld(o, p), sx, sy) && sx >= x0 && sx <= x1 && sy >= y0 && sy <= y1; };
    if (pickMode == 2) { for (size_t i = 0; i < o.mesh.f.size(); i++) if (inside(emesh::faceCenter(o.mesh, (int)i))) selF[i] = 1; }
    else for (size_t i = 0; i < o.mesh.v.size(); i++) if (inside(o.mesh.v[i])) selV[i] = 1;
}

void insertObject(int shape, const std::string& model, int meshKind = -1) {
    pushUndo();
    Obj o;
    o.shape = shape; o.model = model; o.color = brush;
    if (shape == 2) o.scale = {4, 1, 6};
    if (shape == 4) o.scale = {1, 1, 1};
    if (shape == 5) {
        o.mesh = makeMesh(meshKind);
        for (EFace& f : o.mesh.f) f.c = brush;
        o.scale = {1, 1, 1};
        rebuildMesh(o);
    }
    o.name = newName(shape == 4 ? model : shape == 5 ? MESH_NAMES[meshKind] : SHAPE_NAMES[shape]);
    editMode = false; mo.active = false;
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
        if (o.shape == 5 && o.solid) {
            float hh;
            if (meshDown(o, x, z, yRef, hh) && hh <= yRef && hh > best) best = hh;
            continue;
        }
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
        if (o.shape == 5) { // Mesh: nur sperren, wo wirklich Material ist (Treppen lassen sich so hochgehen)
            float cy, R;
            if (!meshNear(o, x, z, cy, R)) continue;
            if ((meshCountUp(o, x, feet + 0.45f, z) & 1) || (meshCountUp(o, x, y + 0.6f, z) & 1)) return true;
            continue;
        }
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

Vec3 focusPoint() {
    if (Obj* eo = editObj()) return selCenterWorld(*eo);
    if (sel >= 0 && sel < (int)objs.size()) return objs[sel].pos;
    return camP + camForward() * 20.0f;
}
void frameSelection() {
    if (sel < 0 || sel >= (int)objs.size()) return;
    const Obj& o = objs[sel];
    Vec3 lo, hi;
    boundsOf(o, lo, hi);
    Vec3 c = toWorld(o, Vec3{(lo.x + hi.x) * 0.5f / o.scale.x, (lo.y + hi.y) * 0.5f / o.scale.y, (lo.z + hi.z) * 0.5f / o.scale.z});
    float r = std::max(1.0f, 0.5f * length(hi - lo));
    camP = c - camForward() * (r * 2.4f + 2.0f);
}
bool escape() {
    if (mo.active) { modalFinish(false); return true; }
    if (editMode) { leaveEdit(); return true; }
    if (drawing) { drawing = false; if (!roads.empty() && roads.back().pts.size() < 2) roads.pop_back(); dirty = true; nextBuild = 0; return true; }
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
        if (mmb) { // Mittlere Maustaste: um die Auswahl kreisen, mit Shift verschieben
            Vec3 pivot = focusPoint();
            float dist = std::max(1.0f, length(pivot - camP));
            if (shiftHeld()) {
                Mat4 cm = camera();
                Vec3 r = xformDir(cm, {1, 0, 0}), u = xformDir(cm, {0, 1, 0});
                camP += (r * (-e.motion.xrel) + u * e.motion.yrel) * (dist * 0.0016f);
            } else {
                camYaw -= e.motion.xrel * 0.005f;
                camPitch = clampv(camPitch - e.motion.yrel * 0.005f, -1.5f, 1.5f);
                camP = pivot - camForward() * dist;
            }
        }
    } else if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
        mouseX = e.button.x; mouseY = e.button.y;
        if (e.button.button == SDL_BUTTON_LEFT) { lmbPressed = true; lmbDown = true; }
        if (e.button.button == SDL_BUTTON_MIDDLE) mmb = true;
        if (e.button.button == SDL_BUTTON_RIGHT) {
            if (mo.active) modalFinish(false); // Rechtsklick bricht Verschieben/Drehen/Skalieren ab
            else { rmb = true; rmbMoved = false; }
        }
    } else if (e.type == SDL_EVENT_MOUSE_BUTTON_UP) {
        if (e.button.button == SDL_BUTTON_LEFT) { lmbDown = false; lmbReleased = true; dragAxis = -1; dragBody = dragPoint = false; }
        if (e.button.button == SDL_BUTTON_MIDDLE) mmb = false;
        if (e.button.button == SDL_BUTTON_RIGHT) {
            rmb = false;
            if (!rmbMoved && drawing) { drawing = false; if (!roads.empty() && roads.back().pts.size() < 2) roads.pop_back(); dirty = true; nextBuild = 0; }
        }
    } else if (e.type == SDL_EVENT_MOUSE_WHEEL) {
        if (ui.captured) explorerScroll -= (int)(e.wheel.y * 2); // ueber den Leisten: Liste scrollen
        else { // zum Mauszeiger zoomen
            Vec3 ro, rd, hit;
            mouseRay(ro, rd);
            float dist = rayGround(ro, rd, hit, true) ? length(hit - ro) : 25.0f;
            camP += rd * (clampv(dist * 0.15f, 0.4f, 80.0f) * e.wheel.y);
        }
    } else if (e.type == SDL_EVENT_KEY_DOWN) {
        bool ctrl = (e.key.mod & SDL_KMOD_CTRL) != 0, shift = (e.key.mod & SDL_KMOD_SHIFT) != 0;
        SDL_Keycode k = e.key.key;
        if (mo.active) { // waehrend Verschieben/Drehen/Skalieren: Achse sperren, Zahl tippen, bestaetigen
            if (k == SDLK_X || k == SDLK_Y || k == SDLK_Z) { int a = k == SDLK_X ? 0 : k == SDLK_Y ? 1 : 2; mo.axis = mo.axis == a ? -1 : a; }
            else if (k == SDLK_N && pickMode == 2) mo.axis = mo.axis == 3 ? -1 : 3;
            else if (k == SDLK_RETURN || k == SDLK_KP_ENTER) modalFinish(true);
            else if (k >= SDLK_0 && k <= SDLK_9) mo.num += (char)('0' + (k - SDLK_0));
            else if (k == SDLK_PERIOD || k == SDLK_COMMA) mo.num += '.';
            else if (k == SDLK_MINUS && mo.num.empty()) mo.num += '-';
            else if (k == SDLK_BACKSPACE && !mo.num.empty()) mo.num.pop_back();
            return;
        }
        if (ctrl && k == SDLK_S) save();
        else if (ctrl && k == SDLK_Z && shift) redo();
        else if (ctrl && k == SDLK_Z) undo();
        else if (ctrl && k == SDLK_Y) redo();
        else if (ctrl && k == SDLK_D && sel >= 0 && sel < (int)objs.size() && !editMode) {
            pushUndo();
            Obj o = objs[sel];
            o.pos += Vec3{std::max(1.0f, o.scale.x) + 0.5f, 0, 0};
            o.name = newName(o.shape == 4 ? o.model : o.shape == 5 ? "Mesh" : SHAPE_NAMES[o.shape]);
            syncGeo(o);
            objs.push_back(o);
            sel = (int)objs.size() - 1;
        } else if (Obj* eo = editObj()) { // Bearbeitungsmodus (Blender-Tasten)
            if (k == SDLK_TAB) leaveEdit();
            else if (k >= SDLK_1 && k <= SDLK_3) setPickMode(k - SDLK_1);
            else if (k == SDLK_G) modalBegin(0, -1, false);
            else if (k == SDLK_R) modalBegin(1, -1, false);
            else if (k == SDLK_S) modalBegin(2, -1, false);
            else if (k == SDLK_E) editExtrude(*eo);
            else if (k == SDLK_X || k == SDLK_DELETE || k == SDLK_BACKSPACE) editDelete(*eo);
            else if (k == SDLK_F) editMakeFace(*eo);
            else if (k == SDLK_A) editSelectAll(*eo);
            else if (k == SDLK_U) editSubdivide(*eo);
            else if (k == SDLK_V) editFlip(*eo);
            else if (k == SDLK_H) frameSelection();
        } else if (k == SDLK_TAB) { if (sel >= 0) enterEdit(); }
        else if (k == SDLK_F) frameSelection();
        else if (k == SDLK_DELETE || k == SDLK_BACKSPACE) {
            if (drawing && !roads.empty()) { roads.back().pts.pop_back(); if (roads.back().pts.empty()) { roads.pop_back(); drawing = false; } dirty = true; nextBuild = 0; }
            else if (sel >= 0 && sel < (int)objs.size()) { pushUndo(); objs.erase(objs.begin() + sel); sel = -1; }
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
            float d = k == SDLK_PAGEUP ? gridStep : -gridStep;
            if (sel >= 0 && sel < (int)objs.size()) { pushUndo(); objs[sel].pos.y += d; syncGeo(objs[sel]); }
            else if (selRoad >= 0 && selPt >= 0) { pushUndo(); roads[selRoad].pts[selPt].lift = std::max(0.0f, roads[selRoad].pts[selPt].lift + d * 2); dirty = true; nextBuild = 0; }
        } else if (!ctrl && k >= SDLK_1 && k <= SDLK_4) { tool = k - SDLK_1; if (tool != 3) drawing = false; }
    }
}

void update(float dt, const Mat4& vp, float ww, float wh) {
    lastVP = vp; vw = ww; vh = wh;
    const bool* keys = SDL_GetKeyboardState(nullptr);
    if (rmb && !keys[SDL_SCANCODE_LCTRL] && !keys[SDL_SCANCODE_RCTRL]) { // fliegen nur mit gedrueckter rechter Maustaste (wie in Roblox Studio)
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
    bool rawClick = lmbPressed, click = lmbPressed && !ui.captured, released = lmbReleased;
    lmbPressed = false; lmbReleased = false;
    Vec3 ro, rd, hit;
    mouseRay(ro, rd);
    Obj* eo = editObj();
    if (mo.active && !eo) mo.active = false;
    if (editMode && !eo) editMode = false;

    if (mo.active) {
        modalApply(*eo);
        if (mo.drag ? released : rawClick) modalFinish(true);
        return;
    }
    if (eo) { // ---- Bearbeitungsmodus ----
        fitSel(*eo);
        if (click) {
            bool handled = false;
            std::vector<int> ids = activeVerts(*eo);
            if (!ids.empty()) { // Pfeil am Auswahlzentrum gegriffen?
                Vec3 c = selCenterWorld(*eo);
                float L = gizmoLen(c), sx, sy, tx, ty;
                for (int a = 0; a < 3 && !handled; a++)
                    if (project(c, sx, sy) && project(c + worldAxis(a) * L, tx, ty) && distToSeg(mouseX, mouseY, sx, sy, tx, ty) < 12) { modalBegin(0, a, true); handled = true; }
            }
            if (!handled && !editClick(*eo, ro, rd, shiftHeld())) { boxSelecting = true; boxX0 = mouseX; boxY0 = mouseY; }
        }
        if (released && boxSelecting) {
            boxSelecting = false;
            if (std::fabs(mouseX - boxX0) + std::fabs(mouseY - boxY0) > 6) boxSelect(*eo, boxX0, boxY0, mouseX, mouseY, shiftHeld());
            else if (!shiftHeld()) clearSel(*eo);
        }
        return;
    }
    boxSelecting = false;

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
            if (sel >= 0 && sel < (int)objs.size()) { // Achsengriff getroffen?
                Obj& o = objs[sel];
                Vec3 c = o.pos;
                float L = gizmoLen(c), sx, sy, tx, ty;
                for (int a = 0; a < 3 && !handled; a++)
                    if (project(c, sx, sy) && project(c + axisDir(a, &o) * L, tx, ty) && distToSeg(mouseX, mouseY, sx, sy, tx, ty) < 12) {
                        dragAxis = a; handled = true; pushUndo();
                        dragAcc = 0;
                        dragStart = tool == 0 ? o.pos : tool == 2 ? o.scale : o.rot;
                    }
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
    if (lmbDown && sel >= 0 && sel < (int)objs.size() && dragAxis >= 0 && (dmx != 0 || dmy != 0)) {
        Obj& o = objs[sel];
        Vec3 ax = axisDir(dragAxis, &o);
        float L = gizmoLen(o.pos), sx, sy, tx, ty;
        if (project(o.pos, sx, sy) && project(o.pos + ax * L, tx, ty)) {
            float ax2 = (tx - sx) * (tx - sx) + (ty - sy) * (ty - sy);
            float d = ax2 > 1 ? (dmx * (tx - sx) + dmy * (ty - sy)) / ax2 * L : 0;
            if (tool == 0) { dragAcc += d; o.pos = dragStart + ax * snapQ(dragAcc); }
            else if (tool == 2) {
                dragAcc += d;
                float base = dragAxis == 0 ? dragStart.x : dragAxis == 1 ? dragStart.y : dragStart.z;
                float* s = dragAxis == 0 ? &o.scale.x : dragAxis == 1 ? &o.scale.y : &o.scale.z;
                *s = std::max(0.1f, snapQ(base + dragAcc * 2));
            } else {
                dragAcc += dmx * 0.01f;
                float base = dragAxis == 1 ? dragStart.x : dragAxis == 0 ? dragStart.y : dragStart.z;
                float v = base + dragAcc;
                if (snapOn()) v = std::round(v / (PI / 12)) * (PI / 12);
                (dragAxis == 1 ? o.rot.x : dragAxis == 0 ? o.rot.y : o.rot.z) = v;
            }
            syncGeo(o);
        }
    } else if (lmbDown && dragBody && sel >= 0 && sel < (int)objs.size() && (dmx != 0 || dmy != 0) && rayGround(ro, rd, hit, false)) {
        Obj& o = objs[sel];
        if (snapOn()) { hit.x = snapQ(hit.x); hit.z = snapQ(hit.z); }
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
}

// ---------- Oberflaeche ----------
namespace {
int activeSlider = -1;
// Waagerechter Regler 0..255 (Farbkanal); true solange er veraendert wird
bool slider(int id, float x, float y, float w, float& v, Color tint) {
    ui.geo.rrect(x, y + 8, w, 8, 4, Color{36, 40, 50, 255});
    float fw = w * v / 255.0f;
    ui.geo.rrect(x, y + 8, std::max(8.0f, fw), 8, 4, tint);
    ui.geo.rrect(x + fw - 6, y + 4, 12, 16, 5, col::white);
    bool over = ui.over(x - 6, y, w + 12, 24);
    if (over) ui.captured = true;
    if (over && ui.clicked) { activeSlider = id; pushUndo(); ui.clicked = false; }
    if (!ui.down) activeSlider = -1;
    if (activeSlider != id) return false;
    v = clampv((ui.mx - x) / w * 255.0f, 0.0f, 255.0f);
    return true;
}
std::string hexOf(Color c) { char b[16]; snprintf(b, sizeof b, "#%02X%02X%02X", c.r, c.g, c.b); return b; }
// Farbblock: Vorschau, RGB-Regler, feste und eigene Farben. Gibt die naechste freie y-Position zurueck
float colorBlock(float px, float y) {
    ui.label(px + 14, y + 14, 14, col::gray, "Farbe");
    ui.geo.rrect(px + 70, y + 1, 60, 20, 5, brush);
    ui.label(px + 138, y + 16, 13, col::white, hexOf(brush));
    if (ui.button(px + 204, y - 2, 54, 24, "+ Merken", false, 12) && customPal.size() < 12) { pushUndo(); customPal.push_back(brush); }
    y += 28;
    float ch[3] = {(float)brush.r, (float)brush.g, (float)brush.b};
    const Color tints[3] = {{230, 70, 60, 255}, {80, 200, 90, 255}, {80, 140, 240, 255}};
    bool changed = false;
    for (int i = 0; i < 3; i++) { changed |= slider(i + 1, px + 34, y, 212, ch[i], tints[i]); ui.label(px + 14, y + 17, 13, col::gray, i == 0 ? "R" : i == 1 ? "G" : "B"); y += 24; }
    if (changed) applyColor(Color{(uint8_t)(ch[0] + 0.5f), (uint8_t)(ch[1] + 0.5f), (uint8_t)(ch[2] + 0.5f), 255});
    static const Color pal[12] = {{235, 235, 235, 255}, {150, 150, 150, 255}, {60, 60, 64, 255}, {200, 60, 50, 255}, {230, 140, 40, 255}, {235, 205, 60, 255},
                                  {80, 170, 80, 255}, {50, 130, 200, 255}, {110, 80, 180, 255}, {140, 95, 60, 255}, {215, 195, 160, 255}, {20, 20, 22, 255}};
    auto swatches = [&](const Color* cs, int n) {
        for (int i = 0; i < n; i++) {
            float bx = px + 14 + (i % 6) * 41.0f, by = y + (i / 6) * 26.0f;
            ui.geo.rrect(bx, by, 36, 22, 5, cs[i]);
            if (ui.over(bx, by, 36, 22)) { ui.captured = true; if (ui.clicked) { pushUndo(); applyColor(cs[i]); ui.clicked = false; } }
        }
        y += ((n + 5) / 6) * 26.0f;
    };
    y += 2;
    swatches(pal, 12);
    if (!customPal.empty()) { ui.label(px + 14, y + 12, 12, col::gray, "Eigene Farben"); y += 16; swatches(customPal.data(), (int)customPal.size()); }
    return y + 4;
}
} // namespace

void buildUI() {
    float W = ui.W, H = ui.H;
    char buf[160];
    Obj* eo = editObj();
    static int lastSel = -2;
    if (sel != lastSel) { lastSel = sel; if (sel >= 0 && sel < (int)objs.size()) brush = objs[sel].color; }
    if (explorerScroll < 0) explorerScroll = 0;

    // ---- Kopfleiste ----
    ui.panel(0, 0, W, 46, Color{14, 16, 22, 240}, 0);
    ui.label(14, 30, 20, col::accent, "Studio");
    float x = 100;
    if (ui.button(x, 7, 100, 32, modified ? "Speichern *" : "Speichern", false, 15)) save();
    x += 106;
    if (ui.button(x, 7, 90, 32, "Testfahrt", false, 15)) testDrive = true;
    x += 100;
    if (ui.button(x, 7, 72, 32, "Zur\xC3\xBC" "ck", false, 15)) undo();
    x += 78;
    if (ui.button(x, 7, 84, 32, "Wiederh.", false, 15)) redo();
    x += 100;
    if (!eo) {
        const char* tools[4] = {"Bewegen", "Drehen", "Skalieren", "Stra\xC3\x9F" "e"};
        for (int i = 0; i < 4; i++) {
            if (ui.button(x, 7, 100, 32, std::string(1, (char)('1' + i)) + "  " + tools[i], tool == i, 15)) { tool = i; if (i != 3) drawing = false; }
            x += 104;
        }
    } else {
        const char* pm[3] = {"1  Punkt", "2  Kante", "3  Fl\xC3\xA4" "che"};
        for (int i = 0; i < 3; i++) {
            if (ui.button(x, 7, 100, 32, pm[i], pickMode == i, 15)) setPickMode(i);
            x += 104;
        }
    }
    {
        static const float steps[4] = {0.25f, 0.5f, 1.0f, 2.0f};
        if (ui.button(W - 304, 7, 140, 32, gridLabel(), gridOn, 15)) {
            if (!gridOn) { gridOn = true; gridStep = 0.25f; }
            else {
                int i = 0;
                while (i < 4 && std::fabs(steps[i] - gridStep) > 1e-3f) i++;
                if (i >= 3) gridOn = false; else gridStep = steps[i + 1];
            }
        }
        bool canEdit = eo || (sel >= 0 && sel < (int)objs.size() && objs[sel].shape != 4);
        if (ui.button(W - 158, 7, 148, 32, eo ? "Fertig (Tab)" : "Bearbeiten (Tab)", eo != nullptr, 15) && (canEdit)) { if (eo) leaveEdit(); else enterEdit(); }
    }

    // ---- Linke Leiste: Einfuegen / Strassentyp, darunter der Explorer ----
    float lh;
    if (tool == 3 && !eo) lh = 56 + NTYPES * 38.0f + 30;
    else lh = 56 + 5 * 36.0f + (modelList.empty() ? 0 : 26 + modelList.size() * 34.0f) + 28;
    ui.panel(10, 58, 232, lh);
    if (tool == 3 && !eo) {
        ui.label(24, 82, 17, col::accent, "Stra\xC3\x9F" "entyp");
        float y = 94;
        for (int i = 0; i < NTYPES; i++) { if (ui.button(20, y, 212, 32, TYPES[i].name, roadType == i, 15)) roadType = i; y += 38; }
        ui.label(24, y + 14, 12, col::gray, "Klick = Punkt, Enter/Rechtsklick = fertig");
    } else {
        ui.label(24, 82, 17, col::accent, "Einf\xC3\xBC" "gen");
        static const char* gridNames[10] = {"Block", "Zylinder", "Keil", "Kugel", "Treppe", "Dach", "Pyramide", "Kegel", "Platte", "L-Winkel"};
        for (int i = 0; i < 10; i++) {
            float bx = 20 + (i % 2) * 108.0f, by = 94 + (i / 2) * 36.0f;
            if (ui.button(bx, by, 104, 30, gridNames[i], false, 14)) { if (i < 4) insertObject(i, ""); else insertObject(5, "", i - 4); }
        }
        float y = 94 + 5 * 36.0f;
        if (!modelList.empty()) {
            ui.label(24, y + 14, 13, col::gray, "3D-Modelle");
            y += 22;
            for (const std::string& m : modelList) { if (ui.button(20, y, 212, 28, m, false, 13)) insertObject(4, m); y += 34; }
        }
        ui.label(24, y + 14, 12, col::gray, "Modelle: .obj nach assets/models");
    }
    // Explorer (Objektliste)
    {
        float ey = 58 + lh + 8, eh = H - 36 - ey;
        if (eh > 110) {
            ui.panel(10, ey, 232, eh);
            snprintf(buf, sizeof buf, "Explorer  (%d)", (int)objs.size());
            ui.label(24, ey + 24, 15, col::accent, buf);
            int rows = std::max(1, (int)((eh - 44) / 25));
            int maxScroll = std::max(0, (int)objs.size() - rows);
            explorerScroll = std::min(explorerScroll, maxScroll);
            float yy = ey + 36;
            for (int i = explorerScroll; i < (int)objs.size() && i < explorerScroll + rows; i++) {
                if (ui.button(20, yy, 212, 22, objs[i].name, sel == i, 13)) { if (sel != i) leaveEdit(); sel = i; selRoad = selPt = -1; }
                yy += 25;
            }
            if (objs.empty()) ui.label(24, ey + 58, 13, col::gray, "Noch nichts gebaut");
            if (maxScroll > 0) ui.labelR(228, ey + 24, 12, col::gray, "Mausrad = scrollen");
        }
    }

    // ---- Rechte Leiste: Eigenschaften / Bearbeiten ----
    float px = W - 282;
    if (sel >= 0 && sel < (int)objs.size()) {
        Obj& o = objs[sel];
        float y = 58;
        if (eo) {
            // Info + Werkzeuge des Bearbeitungsmodus
            int ns = 0;
            for (char s : selF) if (s) ns++;
            std::vector<int> av = activeVerts(o);
            ui.panel(px, 58, 272, 420 + (customPal.empty() ? 0 : 16 + ((customPal.size() + 5) / 6) * 26), col::dark);
            ui.label(px + 14, 82, 17, col::accent, "Bearbeiten: " + o.name);
            snprintf(buf, sizeof buf, "%d Punkte, %d Fl\xC3\xA4" "chen  |  gew\xC3\xA4hlt: %d", (int)o.mesh.v.size(), (int)o.mesh.f.size(), pickMode == 2 ? ns : (int)av.size());
            ui.label(px + 14, 104, 13, col::white, buf);
            y = 114;
            struct Op { const char* t; int id; };
            static const Op ops[10] = {{"Verschieben (G)", 0}, {"Drehen (R)", 1}, {"Skalieren (S)", 2}, {"Extrudieren (E)", 3}, {"Teilen (U)", 4},
                                       {"L\xC3\xB6schen (X)", 5}, {"Fl\xC3\xA4" "che aus Punkten (F)", 6}, {"Umdrehen (V)", 7}, {"Alles / nichts (A)", 8}, {"Ansicht (H)", 9}};
            for (int i = 0; i < 10; i++) {
                float bx = px + 14 + (i % 2) * 124.0f, by = y + (i / 2) * 32.0f;
                if (ui.button(bx, by, 120, 28, ops[i].t, false, 12)) {
                    switch (ops[i].id) {
                    case 0: modalBegin(0, -1, false); break;
                    case 1: modalBegin(1, -1, false); break;
                    case 2: modalBegin(2, -1, false); break;
                    case 3: editExtrude(o); break;
                    case 4: editSubdivide(o); break;
                    case 5: editDelete(o); return;
                    case 6: editMakeFace(o); break;
                    case 7: editFlip(o); break;
                    case 8: editSelectAll(o); break;
                    default: frameSelection(); break;
                    }
                }
            }
            y += 5 * 32.0f + 14;
            ui.label(px + 14, y + 4, 12, col::gray, "Farbe gilt f\xC3\xBC" "r die gew\xC3\xA4hlten Fl\xC3\xA4" "chen");
            y += 12;
            colorBlock(px, y);
        } else {
            ui.panel(px, 58, 272, 510 + (customPal.empty() ? 0 : 16 + ((customPal.size() + 5) / 6) * 26), col::dark);
            ui.label(px + 14, 82, 17, col::accent, o.name);
            y = 92;
            // Zeilen mit Wert und -/+ (Schrittweite = Raster)
            auto rowf = [&](const char* name, float val, float step, float minV, float& target, const char* fmt, float shown) {
                char b[48];
                snprintf(b, sizeof b, fmt, shown);
                ui.label(px + 14, y + 19, 14, col::gray, name);
                ui.label(px + 92, y + 19, 14, col::white, b);
                if (ui.button(px + 196, y, 28, 26, "-", false, 15)) { pushUndo(); target = std::max(minV, val - step); syncGeo(o); }
                if (ui.button(px + 228, y, 28, 26, "+", false, 15)) { pushUndo(); target = val + step; syncGeo(o); }
                y += 29;
            };
            float st = gridOn ? gridStep : 0.1f;
            rowf("X", o.pos.x, st, -1e9f, o.pos.x, "%.2f m", o.pos.x);
            rowf("H\xC3\xB6he", o.pos.y, st, -1e9f, o.pos.y, "%.2f m", o.pos.y);
            rowf("Z", o.pos.z, st, -1e9f, o.pos.z, "%.2f m", o.pos.z);
            {   // Drehung in 15-Grad-Schritten
                snprintf(buf, sizeof buf, "%.0f\xC2\xB0", o.rot.x * 180 / PI);
                ui.label(px + 14, y + 19, 14, col::gray, "Drehung");
                ui.label(px + 92, y + 19, 14, col::white, buf);
                if (ui.button(px + 196, y, 28, 26, "-", false, 15)) { pushUndo(); o.rot.x -= PI / 12; }
                if (ui.button(px + 228, y, 28, 26, "+", false, 15)) { pushUndo(); o.rot.x += PI / 12; }
                y += 29;
            }
            const char* dn = o.shape == 5 ? "Skala B" : "Breite";
            rowf(dn, o.scale.x, st, 0.1f, o.scale.x, "%.2f", o.scale.x);
            rowf(o.shape == 5 ? "Skala H" : "H\xC3\xB6he", o.scale.y, st, 0.1f, o.scale.y, "%.2f", o.scale.y);
            rowf(o.shape == 5 ? "Skala L" : "L\xC3\xA4nge", o.scale.z, st, 0.1f, o.scale.z, "%.2f", o.scale.z);
            y += 6;
            y = colorBlock(px, y);
            if (ui.button(px + 14, y, 78, 28, o.solid ? "Fest: ja" : "Fest: nein", o.solid, 13)) { pushUndo(); o.solid = !o.solid; }
            if (ui.button(px + 96, y, 80, 28, "Duplizieren", false, 12)) {
                pushUndo();
                Obj c = o;
                c.pos += Vec3{std::max(1.0f, o.scale.x) + 0.5f, 0, 0};
                c.name = newName(o.shape == 4 ? o.model : o.shape == 5 ? "Mesh" : SHAPE_NAMES[o.shape]);
                syncGeo(c);
                objs.push_back(c);
                sel = (int)objs.size() - 1;
                return;
            }
            if (ui.button(px + 180, y, 78, 28, "L\xC3\xB6schen", false, 13)) { pushUndo(); objs.erase(objs.begin() + sel); sel = -1; return; }
            y += 36;
            // Textur: durchschalten
            int ti = -1;
            for (size_t i = 0; i < textureList.size(); i++) if (textureList[i] == o.texture) ti = (int)i;
            ui.label(px + 14, y + 14, 14, col::gray, "Textur");
            ui.label(px + 70, y + 14, 13, col::white, o.texture.empty() ? "keine" : o.texture);
            if (ui.button(px + 196, y - 2, 28, 24, "<", false, 14)) { pushUndo(); ti--; if (ti < -1) ti = (int)textureList.size() - 1; o.texture = ti < 0 ? "" : textureList[ti]; }
            if (ui.button(px + 228, y - 2, 28, 24, ">", false, 14)) { pushUndo(); ti++; if (ti >= (int)textureList.size()) ti = -1; o.texture = ti < 0 ? "" : textureList[ti]; }
            y += 26;
            snprintf(buf, sizeof buf, "Kachelgr\xC3\xB6\xC3\x9F" "e %.1f m", o.tile);
            ui.label(px + 14, y + 14, 13, col::gray, buf);
            if (ui.button(px + 196, y - 2, 28, 24, "-", false, 14)) o.tile = std::max(0.25f, o.tile * 0.5f);
            if (ui.button(px + 228, y - 2, 28, 24, "+", false, 14)) o.tile = std::min(64.0f, o.tile * 2);
        }
    } else if (selRoad >= 0 && selRoad < (int)roads.size() && selPt >= 0) {
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

    // ---- Auswahlrahmen und Anzeige waehrend Verschieben/Drehen/Skalieren ----
    if (boxSelecting && lmbDown) {
        float x0 = std::min(boxX0, mouseX), y0 = std::min(boxY0, mouseY), x1 = std::max(boxX0, mouseX), y1 = std::max(boxY0, mouseY);
        Color f{255, 210, 60, 40}, l{255, 210, 60, 220};
        ui.geo.rect(x0, y0, x1 - x0, y1 - y0, f);
        ui.geo.rect(x0, y0, x1 - x0, 1.5f, l); ui.geo.rect(x0, y1 - 1.5f, x1 - x0, 1.5f, l);
        ui.geo.rect(x0, y0, 1.5f, y1 - y0, l); ui.geo.rect(x1 - 1.5f, y0, 1.5f, y1 - y0, l);
    }
    if (mo.active) {
        ui.panel(W / 2 - 250, 56, 500, 54, col::dark);
        ui.labelC(W / 2, 80, 18, col::accent, mo.info);
        ui.labelC(W / 2, 100, 12, col::gray, "X / Y / Z Achse sperren  |  Zahl tippen  |  Klick / Enter = ok  |  Esc / Rechtsklick = abbrechen");
    }

    // ---- Fusszeile ----
    ui.panel(0, H - 30, W, 30, Color{14, 16, 22, 230}, 0);
    std::string hint;
    if (simTime < statusUntil) hint = statusMsg;
    else if (mo.active) hint = "G/R/S: Maus bewegen, X/Y/Z sperrt die Achse, N = Fl\xC3\xA4" "chennormale, Zahl tippen f\xC3\xBCr exakten Wert";
    else if (eo) hint = "Klick = w\xC3\xA4hlen  |  Shift = mehrere  |  Rahmen ziehen  |  G/R/S bewegen, drehen, skalieren  |  E extrudieren  |  X l\xC3\xB6schen  |  F Fl\xC3\xA4" "che  |  Tab fertig";
    else if (tool == 3) hint = "Klick = Stra\xC3\x9F" "enpunkt setzen  |  Enter / Rechtsklick = fertig  |  Entf = letzten Punkt entfernen";
    else hint = "Rechte Maustaste + WASD/QE fliegen  |  Mittlere Taste kreisen (+Shift schieben)  |  F auf Auswahl  |  Tab = Mesh bearbeiten  |  Strg+D  |  Strg+S";
    ui.label(12, H - 10, 13, simTime < statusUntil ? col::accent : col::gray, hint);
    snprintf(buf, sizeof buf, "%d Objekte, %d Stra\xC3\x9F" "en", (int)objs.size(), (int)roads.size());
    ui.labelR(W - 12, H - 10, 13, col::gray, buf);
}

void drawWorld() {
    gfxModel(identity());
    gfxMode(0, FLAG_LIT | FLAG_FOG);
    solidMesh.draw();
    for (const Obj& o : objs) {
        if (std::fabs(o.pos.x) > 12000 || std::fabs(o.pos.z) > 12000) continue;
        bool mesh = o.shape == 5 && o.emesh;
        gfxMode(mesh ? 9 : 7);
        gfxModel(matOf(o));
        if (mesh) gfxTint(1, 1, 1); else gfxTint(o.color.r / 255.0f, o.color.g / 255.0f, o.color.b / 255.0f);
        gfxTile(o.tile);
        gfxBindTexture(texOf(o.texture));
        if (o.shape == 4) modelOf(o.model).mesh.draw();
        else if (o.shape == 5) { if (o.emesh) o.emesh->draw(); }
        else shapes[o.shape].draw();
    }
    gfxModel(identity());
}

void drawGizmos(const Mat4& vp) {
    gizmo.clear();
    Mat4 I = identity();
    Color yellow{255, 210, 60, 255};
    const Color ac[3] = {{240, 70, 60, 255}, {90, 220, 90, 255}, {80, 140, 255, 255}};
    if (Obj* eo = editObj()) {
        const Obj& o = *eo;
        fitSel(*eo);
        float dist = length(toWorld(o, {0, 0, 0}) - camP);
        float t0 = std::max(0.006f, dist * 0.0022f);
        Color edge{22, 24, 30, 255}, vtx{250, 250, 252, 255}, face{255, 160, 40, 255};
        std::set<std::pair<int, int>> done;
        auto ekey = [](int a, int b) { return std::make_pair(std::min(a, b), std::max(a, b)); };
        // Gewaehlte Flaechen: Fuellung + gelbe Kanten
        for (size_t fi = 0; fi < o.mesh.f.size(); fi++) {
            const EFace& f = o.mesh.f[fi];
            if (pickMode == 2 && selF[fi]) {
                Vec3 n = qrot(rotOf(o), emesh::faceNormal(o.mesh, (int)fi));
                Vec3 off = n * (t0 * 1.5f);
                for (size_t k = 1; k + 1 < f.v.size(); k++)
                    gizmo.tri(toWorld(o, o.mesh.v[f.v[0]]) + off, toWorld(o, o.mesh.v[f.v[k]]) + off, toWorld(o, o.mesh.v[f.v[k + 1]]) + off, n, Color{face.r, face.g, face.b, 120});
                for (size_t k = 0; k < f.v.size(); k++) {
                    int a = f.v[k], b = f.v[(k + 1) % f.v.size()];
                    done.insert(ekey(a, b));
                    gizmo.beam(I, toWorld(o, o.mesh.v[a]), toWorld(o, o.mesh.v[b]), t0 * 1.8f, yellow);
                }
            }
        }
        for (const EFace& f : o.mesh.f)
            for (size_t k = 0; k < f.v.size(); k++) {
                int a = f.v[k], b = f.v[(k + 1) % f.v.size()];
                if (!done.insert(ekey(a, b)).second) continue;
                bool s = pickMode != 2 && selV[a] && selV[b];
                gizmo.beam(I, toWorld(o, o.mesh.v[a]), toWorld(o, o.mesh.v[b]), s ? t0 * 1.8f : t0, s ? yellow : edge);
            }
        for (size_t i = 0; i < o.mesh.v.size(); i++) {
            bool s = pickMode != 2 && selV[i];
            if (pickMode == 2 && !s) continue;
            float sz = t0 * (s ? 3.2f : 2.2f);
            gizmo.box(I, toWorld(o, o.mesh.v[i]), {sz, sz, sz}, s ? yellow : vtx);
        }
        std::vector<int> ids = activeVerts(o);
        if (!ids.empty()) {
            Vec3 c = selCenterWorld(o);
            float L = gizmoLen(c);
            if (mo.active) {
                Vec3 ax = modalAxis();
                if (mo.axis >= 0) gizmo.beam(I, c - ax * 400.0f, c + ax * 400.0f, L * 0.012f, mo.axis < 3 ? ac[mo.axis] : yellow);
            } else
                for (int a = 0; a < 3; a++) {
                    Vec3 d = worldAxis(a);
                    gizmo.beam(I, c, c + d * L, L * 0.022f, ac[a]);
                    gizmo.box(I, c + d * L, {L * 0.06f, L * 0.06f, L * 0.06f}, ac[a]);
                }
        }
    } else if (sel >= 0 && sel < (int)objs.size()) {
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
