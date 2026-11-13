// Automodell: geloftete Karosserie, Raeder mit Felgen, Innenraum und analoges 3D-Armaturenbrett
#include "game.h"
#include "stb_image.h"
#include <cstring>
#include <filesystem>

Vec3 EYE{-0.36f, 0.60f, 0.22f};
const Vec3 MIRROR_POS[3] = {{-1.0f, 0.36f, -0.78f}, {0.0f, 0.745f, -0.20f}, {1.0f, 0.36f, -0.78f}};
const float MIRROR_HW[3] = {0.105f, 0.12f, 0.105f}, MIRROR_HH[3] = {0.065f, 0.034f, 0.065f};

static Mesh mBody, mTrim, mGlass, mInterior, mWheel, mSteer, mDashGeo, mDashText;
static Batch dynGeo, dynText, dynLit, dynGlow, shadow;
static Mat4 gaugeM, screenM;
static const Mat4 I4 = identity();

static Mat4 rX(float a) { return fromQuatPos(qaxis({1, 0, 0}, a), {}); }
static Mat4 rY(float a) { return fromQuatPos(qaxis({0, 1, 0}, a), {}); }
static Mat4 rZ(float a) { return fromQuatPos(qaxis({0, 0, 1}, a), {}); }

// Querschnitte von vorn nach hinten: z, halbe Breite, Unterkante, Guertellinie, Dach, halbe Dachbreite, Typ (H Haube, C Kabine, T Heck)
struct Station { float z, w, bot, belt, roof, rw; char t; };
static const Station ST[] = {
    {-1.98f, 0.58f, -0.20f, 0.00f, 0.03f, 0.44f, 'H'}, {-1.82f, 0.80f, -0.34f, 0.10f, 0.13f, 0.66f, 'H'},
    {-1.10f, 0.87f, -0.36f, 0.22f, 0.25f, 0.73f, 'H'}, {-0.95f, 0.88f, -0.36f, 0.28f, 0.31f, 0.74f, 'H'},
    {-0.25f, 0.88f, -0.36f, 0.31f, 0.88f, 0.62f, 'C'}, {1.00f, 0.88f, -0.36f, 0.33f, 0.90f, 0.62f, 'C'},
    {1.75f, 0.87f, -0.36f, 0.37f, 0.84f, 0.60f, 'C'},  {2.15f, 0.84f, -0.34f, 0.40f, 0.43f, 0.70f, 'T'},
    {2.26f, 0.74f, -0.22f, 0.30f, 0.33f, 0.60f, 'T'}};
static const int NS = sizeof ST / sizeof ST[0];

static void ringOf(const Station& s, Vec3* o) {
    float r4y = std::max(s.roof - 0.05f, s.belt + 0.005f);
    Vec3 r[7] = {{0, s.bot, s.z}, {s.w - 0.10f, s.bot, s.z}, {s.w, s.bot + 0.10f, s.z}, {s.w * 0.985f, s.belt, s.z},
                 {s.rw + 0.02f, r4y, s.z}, {s.rw - 0.10f, s.roof, s.z}, {0, s.roof + 0.012f, s.z}};
    for (int i = 0; i < 7; i++) o[i] = r[i];
    for (int i = 1; i <= 5; i++) o[6 + i] = {-r[6 - i].x, r[6 - i].y, r[6 - i].z};
}

static void buildBody() {
    Batch body, glass, trim;
    Vec3 P[NS][12], N[NS][12];
    for (int i = 0; i < NS; i++) { ringOf(ST[i], P[i]); for (int e = 0; e < 12; e++) N[i][e] = {0, 0, 0}; }
    auto faceN = [&](int i, int e) {
        int f = (e + 1) % 12;
        Vec3 a = P[i][e], b = P[i][f], c = P[i + 1][f], d = P[i + 1][e];
        Vec3 n = normalize(cross(c - a, d - b)), ctr = (a + b + c + d) * 0.25f;
        Vec3 ax{0, (ST[i].bot + ST[i].belt) * 0.5f, ctr.z};
        return dot(n, ctr - ax) < 0 ? -n : n;
    };
    for (int i = 0; i + 1 < NS; i++)
        for (int e = 0; e < 12; e++) {
            Vec3 n = faceN(i, e);
            int f = (e + 1) % 12;
            N[i][e] += n; N[i][f] += n; N[i + 1][e] += n; N[i + 1][f] += n;
        }
    Color white{255, 255, 255, 255}, gl{20, 28, 36, 218};
    for (int i = 0; i + 1 < NS; i++)
        for (int e = 0; e < 12; e++) {
            int f = (e + 1) % 12;
            char a = ST[i].t, b = ST[i + 1].t;
            bool upper = e >= 3 && e <= 8, side = e == 3 || e == 8;
            bool isGlass = (a == 'H' && b == 'C' && upper) || (a == 'C' && b == 'C' && side) || (a == 'C' && b == 'T' && upper);
            Batch& t = isGlass ? glass : body;
            Color c = isGlass ? gl : white;
            int ii[6] = {i, i, i + 1, i, i + 1, i + 1}, ee[6] = {e, f, f, e, f, e};
            for (int k = 0; k < 6; k++) t.vert(P[ii[k]][ee[k]], normalize(N[ii[k]][ee[k]]), 0, 0, c);
        }
    for (int e = 1; e < 11; e++) { // Front- und Heckabschluss
        body.tri(P[0][0], P[0][e], P[0][e + 1], {0, 0, -1}, white);
        body.tri(P[NS - 1][0], P[NS - 1][e], P[NS - 1][e + 1], {0, 0, 1}, white);
    }
    mBody.create(body.v.data(), (int)body.v.size());
    mGlass.create(glass.v.data(), (int)glass.v.size());

    // Anbauteile: Saeulen, Stossfaenger, Grill, Radhaeuser, Spiegelgehaeuse, Kennzeichen
    Color black{18, 18, 20, 255}, darkGray{34, 35, 38, 255};
    for (float s : {-1.0f, 1.0f}) {
        trim.beam(I4, {s * 0.865f, 0.29f, -0.95f}, {s * 0.63f, 0.875f, -0.25f}, 0.032f, black);
        trim.beam(I4, {s * 0.87f, 0.32f, 0.55f}, {s * 0.63f, 0.89f, 0.55f}, 0.035f, black);
        trim.beam(I4, {s * 0.86f, 0.37f, 1.75f}, {s * 0.61f, 0.835f, 1.75f}, 0.04f, black);
        trim.beam(I4, {s * 0.63f, 0.885f, -0.25f}, {s * 0.63f, 0.895f, 1.00f}, 0.02f, black);
        for (float z : {-1.10f, 1.52f}) // Radhaus als dunkle Scheibe hinter dem Rad
            for (int k = 0; k < 20; k++) {
                float a0 = k * 2 * PI / 20, a1 = (k + 1) * 2 * PI / 20, r = 0.385f, x = s * 0.884f, y = -0.223f;
                trim.tri({x, y, z}, {x, y + std::cos(a0) * r, z + std::sin(a0) * r}, {x, y + std::cos(a1) * r, z + std::sin(a1) * r}, {s, 0, 0}, black);
            }
        trim.box(I4, {s * 1.0f, 0.36f, -0.80f}, {0.115f, 0.075f, 0.02f}, black);
        trim.beam(I4, {s * 0.86f, 0.30f, -0.80f}, {s * 0.93f, 0.33f, -0.80f}, 0.02f, black);
    }
    trim.box(I4, {0, -0.30f, -1.84f}, {0.76f, 0.05f, 0.12f}, darkGray);
    trim.box(I4, {0, -0.11f, -1.95f}, {0.38f, 0.06f, 0.04f}, black);
    trim.box(I4, {0, -0.29f, 2.20f}, {0.79f, 0.06f, 0.07f}, darkGray);
    trim.box(I4, {0, 0.06f, 2.262f}, {0.26f, 0.055f, 0.006f}, Color{235, 235, 235, 255});
    trim.box(I4, {0, 0.745f, -0.225f}, {0.13f, 0.044f, 0.012f}, black);
    trim.beam(I4, {0, 0.79f, -0.23f}, {0, 0.87f, -0.26f}, 0.012f, black);
    mTrim.create(trim.v.data(), (int)trim.v.size());
}

static void buildWheel() {
    Batch b;
    Color tire{20, 20, 22, 255}, barrel{40, 41, 44, 255}, alloy{178, 182, 190, 255};
    const int N = 28;
    const float R = Vehicle::wheelRadius();
    auto band = [&](float x0, float r0, float x1, float r1, Color c, bool axial) {
        for (int i = 0; i < N; i++) {
            float a0 = i * 2 * PI / N, a1 = (i + 1) * 2 * PI / N, am = (a0 + a1) * 0.5f;
            Vec3 n = axial ? Vec3{x0 > 0 ? 1.0f : -1.0f, 0, 0} : Vec3{0, std::cos(am), std::sin(am)};
            b.quad({x0, std::cos(a0) * r0, std::sin(a0) * r0}, {x0, std::cos(a1) * r0, std::sin(a1) * r0}, {x1, std::cos(a1) * r1, std::sin(a1) * r1},
                   {x1, std::cos(a0) * r1, std::sin(a0) * r1}, n, c);
        }
    };
    band(-0.105f, R, 0.105f, R, tire, false);
    band(0.105f, R, 0.115f, R - 0.03f, tire, true);
    band(-0.105f, R, -0.115f, R - 0.03f, tire, true);
    band(0.115f, R - 0.03f, 0.115f, 0.205f, tire, true);
    band(-0.115f, R - 0.03f, -0.115f, 0.10f, tire, true);
    band(0.115f, 0.205f, 0.06f, 0.195f, barrel, false);
    band(0.06f, 0.195f, 0.06f, 0.0f, barrel, true);
    for (int i = 0; i < 5; i++) b.box(rX(i * 2 * PI / 5), {0.098f, 0.105f, 0}, {0.012f, 0.095f, 0.024f}, alloy);
    band(0.112f, 0.055f, 0.112f, 0.0f, alloy, true);
    band(0.112f, 0.055f, 0.06f, 0.06f, alloy, false);
    mWheel.create(b.v.data(), (int)b.v.size());
}

static float gaugeAngle(float frac) { return (135.0f + 270.0f * clampv(frac, 0.0f, 1.0f)) * PI / 180.0f; }

static void buildInterior() {
    Batch b;
    Color dash{30, 31, 34, 255}, dash2{44, 45, 50, 255}, seat{52, 54, 60, 255}, seat2{70, 72, 80, 255}, floorC{24, 24, 26, 255}, black{8, 8, 10, 255}, chrome{150, 152, 158, 255};
    b.box(I4, {0, 0.17f, -0.72f}, {0.845f, 0.13f, 0.24f}, dash);
    b.box(I4, {0, 0.305f, -0.78f}, {0.84f, 0.012f, 0.18f}, dash2);
    b.box(I4, {-0.36f, 0.345f, -0.555f}, {0.21f, 0.012f, 0.10f}, dash);       // Hutze ueber den Instrumenten
    b.box(I4, {-0.565f, 0.28f, -0.52f}, {0.012f, 0.07f, 0.06f}, dash);
    b.box(I4, {-0.155f, 0.28f, -0.52f}, {0.012f, 0.07f, 0.06f}, dash);
    b.box(mul(translate({-0.36f, 0.265f, -0.4775f}), rX(-0.26f)), {0, 0, -0.004f}, {0.195f, 0.072f, 0.003f}, black);
    b.box(I4, {0.05f, 0.24f, -0.476f}, {0.14f, 0.08f, 0.006f}, dash2);        // Rahmen Mitteldisplay
    b.box(I4, {0.05f, 0.24f, -0.4715f}, {0.13f, 0.07f, 0.003f}, black);
    for (float x : {-0.72f, 0.34f, 0.72f}) b.box(I4, {x, 0.24f, -0.478f}, {0.07f, 0.035f, 0.004f}, black); // Luftduesen
    b.box(I4, {0.05f, 0.10f, -0.475f}, {0.13f, 0.045f, 0.006f}, dash2);
    for (int i = 0; i < 3; i++) b.cylinderX(mul(translate({-0.03f + i * 0.08f, 0.10f, -0.462f}), rY(PI / 2)), {0, 0, 0}, 0.022f, 0.008f, 0, chrome, dash);
    b.box(I4, {0, -0.10f, 0.05f}, {0.115f, 0.13f, 0.62f}, dash);              // Mitteltunnel
    b.box(I4, {0, 0.045f, -0.12f}, {0.07f, 0.012f, 0.09f}, dash2);
    b.beam(I4, {-0.36f, 0.20f, -0.30f}, {-0.36f, 0.12f, -0.52f}, 0.035f, dash); // Lenksaeule
    b.beam(I4, {-0.36f, 0.19f, -0.36f}, {-0.60f, 0.21f, -0.36f}, 0.011f, black); // Blinkerhebel
    for (float sx : {-0.36f, 0.36f}) {
        b.box(I4, {sx, -0.10f, 0.42f}, {0.235f, 0.06f, 0.25f}, seat);
        b.box(I4, {sx, -0.045f, 0.42f}, {0.17f, 0.012f, 0.22f}, seat2);
        b.box(mul(translate({sx, 0.20f, 0.70f}), rX(-0.17f)), {0, 0, 0}, {0.225f, 0.31f, 0.055f}, seat);
        b.box(mul(translate({sx, 0.20f, 0.665f}), rX(-0.17f)), {0, 0, 0}, {0.15f, 0.27f, 0.025f}, seat2);
        b.box(I4, {sx, 0.61f, 0.77f}, {0.12f, 0.085f, 0.045f}, seat);
    }
    b.box(I4, {0, -0.08f, 1.32f}, {0.66f, 0.07f, 0.25f}, seat);
    b.box(mul(translate({0, 0.20f, 1.60f}), rX(-0.2f)), {0, 0, 0}, {0.66f, 0.28f, 0.055f}, seat);
    for (float s : {-1.0f, 1.0f}) {
        b.box(I4, {s * 0.85f, 0.0f, 0.25f}, {0.018f, 0.30f, 1.18f}, dash);
        b.box(I4, {s * 0.80f, 0.09f, 0.20f}, {0.035f, 0.02f, 0.30f}, dash2); // Armlehne
    }
    b.box(I4, {0, -0.32f, 0.3f}, {0.85f, 0.02f, 1.65f}, floorC);
    b.box(I4, {0, 0.862f, 0.75f}, {0.59f, 0.008f, 0.97f}, Color{92, 92, 96, 255}); // Dachhimmel
    b.box(I4, {0, 0.36f, 1.85f}, {0.80f, 0.01f, 0.28f}, dash);                    // Hutablage
    for (int i = 0; i < 3; i++) // Pedale: Kupplung, Bremse, Gas
        b.box(mul(translate({-0.52f + i * 0.13f, -0.14f, -0.60f}), rX(0.5f)), {0, 0, 0}, {i == 2 ? 0.03f : 0.04f, i == 2 ? 0.08f : 0.035f, 0.008f}, chrome);
    mInterior.create(b.v.data(), (int)b.v.size());

    Batch s;
    const float r = 0.185f;
    for (int i = 0; i < 32; i++) {
        float a0 = i * 2 * PI / 32, a1 = (i + 1) * 2 * PI / 32;
        s.beam(I4, {std::cos(a0) * r, std::sin(a0) * r, 0}, {std::cos(a1) * r, std::sin(a1) * r, 0}, 0.0165f, (i == 7 || i == 8) ? Color{210, 60, 50, 255} : Color{26, 26, 29, 255});
    }
    s.beam(I4, {-r, -0.01f, 0}, {-0.05f, -0.02f, -0.02f}, 0.016f, dash2);
    s.beam(I4, {r, -0.01f, 0}, {0.05f, -0.02f, -0.02f}, 0.016f, dash2);
    s.beam(I4, {0, -r, 0}, {0, -0.05f, -0.02f}, 0.016f, dash2);
    s.box(I4, {0, -0.02f, -0.02f}, {0.07f, 0.05f, 0.025f}, dash);
    s.box(I4, {0, -0.02f, 0.006f}, {0.02f, 0.02f, 0.002f}, chrome);
    mSteer.create(s.v.data(), (int)s.v.size());

    // Instrumente: Zifferblaetter in Millimetern, danach auf die geneigte Instrumententafel gelegt
    gaugeM = mul(translate({-0.36f, 0.265f, -0.4775f}), mul(rX(-0.26f), scaleM(0.001f, -0.001f, 0.001f)));
    screenM = mul(translate({0.05f, 0.24f, -0.468f}), scaleM(0.001f, -0.001f, 0.001f));
    Batch g, t;
    Color tick{225, 228, 235, 255}, redT{235, 60, 50, 255}, dim{120, 125, 135, 255};
    auto dial = [&](float cx, int steps, int majorEvery, float maxVal, int redFrom, float labelDiv) {
        g.disc(cx, 0, 58, Color{20, 21, 25, 255});
        g.disc(cx, 0, 55, Color{6, 6, 8, 255});
        for (size_t i = g.v.size() - 84; i < g.v.size(); i++) g.v[i].p[2] = 0.25f;
        for (int i = 0; i <= steps; i++) {
            float a = gaugeAngle((float)i / steps);
            bool major = i % majorEvery == 0;
            Vec3 d{std::cos(a), std::sin(a), 0}, p{-std::sin(a), std::cos(a), 0}, c{cx, 0, 0.5f};
            float r0 = major ? 43.0f : 48.0f, w = major ? 0.9f : 0.45f;
            g.quad(c + d * r0 - p * w, c + d * r0 + p * w, c + d * 53 + p * w, c + d * 53 - p * w, {0, 0, 1}, i >= redFrom ? redT : tick);
            if (major) {
                char buf[8];
                snprintf(buf, sizeof buf, "%d", (int)std::lround(maxVal * i / steps / labelDiv));
                float px = 9.5f, tw = font.width(buf, px);
                font.draw(t, cx + d.x * 33 - tw * 0.5f, d.y * 33 + px * 0.36f, px, i >= redFrom ? redT : tick, buf);
            }
        }
    };
    dial(-85, 44, 4, 220, 999, 1);    // Tacho 0..220 km/h
    dial(85, 28, 4, 7000, 24, 1000);  // Drehzahl 0..7 (x1000)
    font.draw(t, -85 - font.width("km/h", 7) * 0.5f, 30, 7, dim, "km/h");
    font.draw(t, 85 - font.width("x1000/min", 6) * 0.5f, 30, 6, dim, "x1000/min");
    g.xform(0, gaugeM);
    t.xform(0, mul(gaugeM, translate({0, 0, 0.8f})));
    mDashGeo.create(g.v.data(), (int)g.v.size());
    mDashText.create(t.v.data(), (int)t.v.size());
}

// ---------- .obj-Autos (assets/cars) ----------
namespace {
struct LampTri { Vec3 p[3]; Vec3 n; int kind; float x, z; };
struct ObjCar {
    std::string name;
    bool tried = false, ok = false, hasWheels = false, hasPaint = false, hasLamps = false;
    int tris = 0;
    std::vector<Vec3> surf;       // Dreiecke von Lack und Karosserie (zum Aufsetzen der Lampen)
    struct Pad { Vec3 c; Vec3 h; int kind; float side; bool rear; };
    std::vector<Pad> pads; bool padsInit = false;
    std::vector<LampTri> lamps;   // Leuchtflaechen (Brems-, Blink-, Rueckfahr-, Scheinwerferlicht)
    Mesh paint, body, wheel[4], glass;   // Karosserie-Lack (einfaerbbar), Rest, Raeder (VL, VR, HL, HR), Fensterglas (durchsichtig)
    Vec3 wc[4];                   // Radmittelpunkte
    Vec3 lo, hi;                  // Abmessungen im Autokoordinatensystem (Vorwaerts = -z, Boden = -0.525)
    Color ref{176, 28, 34, 255};  // Originalfarbe des Lacks
};
std::vector<ObjCar> objCars;

void rgbToHsv(Color c, float& h, float& s, float& v) {
    float r = c.r / 255.0f, g = c.g / 255.0f, b = c.b / 255.0f, mx = std::max(r, std::max(g, b)), mn = std::min(r, std::min(g, b)), d = mx - mn;
    v = mx; s = mx > 1e-4f ? d / mx : 0; h = 0;
    if (d < 1e-4f) return;
    if (mx == r) h = std::fmod((g - b) / d + 6.0f, 6.0f); else if (mx == g) h = (b - r) / d + 2; else h = (r - g) / d + 4;
    h *= 60.0f;
}

float targetLength(const std::string& n) {
    if (n.find("delivery") != std::string::npos) return 5.6f;
    if (n.find("suv-luxury") != std::string::npos) return 4.9f;
    if (n.find("suv") != std::string::npos) return 4.6f;
    if (n.find("hatchback") != std::string::npos) return 4.0f;
    return 4.5f;
}

struct FaceV { int v = 0, t = 0, n = 0; };
struct Face { int g, mat; FaceV a[3]; };
struct MatInfo { std::string name; bool hasKd = false, useTex = false; Color kd{200, 200, 200, 255}; };

std::string lower(std::string s) { for (char& c : s) c = (char)tolower((unsigned char)c); return s; }
bool has(const std::string& s, const char* w) { return s.find(w) != std::string::npos; }

// Farbe eines Materials ohne .mtl-Datei: aus dem Namen abgeleitet (crimson_paint, dark_grey, tire_black ...)
Color colorFromName(const std::string& nameIn) {
    std::string n = lower(nameIn);
    if (has(n, "paint") || has(n, "body")) {
        if (has(n, "blue") || has(n, "navy")) return {40, 90, 190, 255};
        if (has(n, "green")) return {40, 140, 80, 255};
        if (has(n, "yellow")) return {235, 190, 30, 255};
        if (has(n, "orange")) return {230, 110, 30, 255};
        if (has(n, "white")) return {235, 235, 238, 255};
        if (has(n, "black")) return {28, 30, 34, 255};
        if (has(n, "silver") || has(n, "grey") || has(n, "gray")) return {160, 164, 172, 255};
        return {190, 30, 40, 255}; // crimson, red, ...
    }
    if (has(n, "emissive") || has(n, "lamp") || has(n, "light")) {
        if (has(n, "ruby") || has(n, "red")) return {200, 24, 22, 255};
        if (has(n, "amber") || has(n, "orange") || has(n, "indicator")) return {255, 170, 30, 255};
        return {240, 242, 246, 255};
    }
    if (has(n, "lens") || has(n, "headlight")) return {235, 240, 246, 255};
    if (has(n, "glass") || has(n, "window")) return {52, 72, 92, 255};
    if (has(n, "tire") || has(n, "tyre") || has(n, "rubber")) return {22, 22, 24, 255};
    if (has(n, "rim") || has(n, "alloy") || has(n, "chrome") || has(n, "hub") || has(n, "spoke") || has(n, "metal")) return {176, 180, 188, 255};
    if (has(n, "caliper") || has(n, "gold") || has(n, "brass")) return {212, 170, 56, 255};
    if (has(n, "plate") || has(n, "white")) return {236, 236, 238, 255};
    if (has(n, "dark") || has(n, "black") || has(n, "trim") || has(n, "plastic")) return {38, 39, 42, 255};
    if (has(n, "grey") || has(n, "gray")) return {110, 112, 118, 255};
    return {190, 190, 190, 255};
}
// 1 Rueck-/Bremslicht, 2 Blinker, 3 weisses Licht (Rueckfahr-/Tagfahrlicht), 4 Scheinwerfer
int lampKind(const std::string& nameIn) {
    std::string n = lower(nameIn);
    if (has(n, "emissive") || has(n, "lamp")) {
        if (has(n, "ruby") || has(n, "red") || has(n, "brake") || has(n, "tail")) return 1;
        if (has(n, "amber") || has(n, "orange") || has(n, "indicator") || has(n, "blink")) return 2;
        return 3;
    }
    if (has(n, "headlight") && has(n, "lens")) return 4;
    return 0;
}
// Zu welchem Rad gehoert eine Gruppe? 0 VL, 1 VR, 2 HL, 3 HR, -1 keins. Teile wie tire_FL, rim_FL, spoke0_FL oder wheel-front-left.
int wheelCorner(const std::string& nameIn) {
    std::string n = lower(nameIn);
    if (!(has(n, "tire") || has(n, "tyre") || has(n, "rim") || has(n, "hub") || has(n, "spoke") || has(n, "caliper") || has(n, "wheel") || has(n, "brake_disc"))) return -1;
    bool fr = false, bk = false, lf = false, rt = false;
    if (has(n, "front")) fr = true;
    if (has(n, "back") || has(n, "rear")) bk = true;
    if (has(n, "left")) lf = true;
    if (has(n, "right")) rt = true;
    size_t us = n.find_last_of("_-");
    std::string suf = us == std::string::npos ? "" : n.substr(us + 1);
    if (suf.size() == 2 && (suf[0] == 'f' || suf[0] == 'r' || suf[0] == 'b') && (suf[1] == 'l' || suf[1] == 'r')) {
        fr = suf[0] == 'f'; bk = !fr; lf = suf[1] == 'l'; rt = !lf;
    }
    if ((fr == bk) || (lf == rt)) return -1;
    return (fr ? 0 : 2) + (lf ? 0 : 1);
}

bool loadObjCar(ObjCar& m) {
    std::string path = "assets/cars/" + m.name + ".obj";
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    std::string text;
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
    fclose(f);

    std::vector<Vec3> P, N, VC;
    std::vector<float> U, V;
    std::vector<std::string> groups;
    std::vector<MatInfo> mats;
    std::vector<Face> faces;
    std::string mtl;
    int cur = -1, curMat = -1;
    auto matIndex = [&](const std::string& nm) {
        for (size_t i = 0; i < mats.size(); i++) if (mats[i].name == nm) return (int)i;
        MatInfo mi;
        mi.name = nm;
        mats.push_back(mi);
        return (int)mats.size() - 1;
    };
    size_t pos = 0;
    while (pos < text.size()) {
        size_t e = text.find('\n', pos);
        if (e == std::string::npos) e = text.size();
        std::string line = text.substr(pos, e - pos);
        pos = e + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.size() < 2) continue;
        const char* s = line.c_str();
        if (!strncmp(s, "v ", 2)) {
            float x, y, z, r = 1, g = 1, b = 1;
            int k = sscanf(s + 2, "%f %f %f %f %f %f", &x, &y, &z, &r, &g, &b);
            if (k >= 3) { P.push_back({x, y, z}); VC.push_back(k >= 6 ? Vec3{r, g, b} : Vec3{1, 1, 1}); }
        } else if (!strncmp(s, "vn ", 3)) { float x, y, z; if (sscanf(s + 3, "%f %f %f", &x, &y, &z) == 3) N.push_back({x, y, z}); }
        else if (!strncmp(s, "vt ", 3)) { float u, v; if (sscanf(s + 3, "%f %f", &u, &v) == 2) { U.push_back(u); V.push_back(v); } }
        else if (!strncmp(s, "mtllib ", 7)) mtl = s + 7;
        else if (!strncmp(s, "usemtl ", 7)) curMat = matIndex(s + 7);
        else if (!strncmp(s, "g ", 2) || !strncmp(s, "o ", 2)) { groups.push_back(s + 2); cur = (int)groups.size() - 1; }
        else if (!strncmp(s, "f ", 2)) {
            std::vector<FaceV> fv;
            const char* p = s + 2;
            while (*p) {
                while (*p == ' ') p++;
                if (!*p) break;
                FaceV q;
                char* end;
                q.v = (int)strtol(p, &end, 10); p = end;
                if (*p == '/') { p++; if (*p != '/') { q.t = (int)strtol(p, &end, 10); p = end; } if (*p == '/') { p++; q.n = (int)strtol(p, &end, 10); p = end; } }
                if (q.v < 0) q.v = (int)P.size() + q.v + 1;
                if (q.t < 0) q.t = (int)U.size() + q.t + 1;
                if (q.n < 0) q.n = (int)N.size() + q.n + 1;
                if (q.v >= 1 && q.v <= (int)P.size()) fv.push_back(q);
                while (*p && *p != ' ') p++;
            }
            if (cur < 0) { groups.push_back("body"); cur = 0; }
            for (size_t k = 1; k + 1 < fv.size(); k++) faces.push_back({cur, curMat, {fv[0], fv[k], fv[k + 1]}});
        }
    }
    if (faces.empty()) return false;

    // .mtl (falls vorhanden): Kd-Farben und eine Farbtextur
    std::vector<uint8_t> tex;
    int tw = 0, th = 0;
    if (!mtl.empty()) {
        FILE* mf = fopen(("assets/cars/" + mtl).c_str(), "rb");
        if (mf) {
            std::string mt;
            while ((n = fread(buf, 1, sizeof buf, mf)) > 0) mt.append(buf, n);
            fclose(mf);
            size_t mp = 0;
            int mc = -1;
            while (mp < mt.size()) {
                size_t e = mt.find('\n', mp);
                if (e == std::string::npos) e = mt.size();
                std::string l = mt.substr(mp, e - mp);
                mp = e + 1;
                while (!l.empty() && (l.back() == '\r' || l.back() == ' ')) l.pop_back();
                if (!strncmp(l.c_str(), "newmtl ", 7)) mc = matIndex(l.substr(7));
                else if (mc >= 0 && !strncmp(l.c_str(), "Kd ", 3)) {
                    float r, g, b;
                    if (sscanf(l.c_str() + 3, "%f %f %f", &r, &g, &b) == 3) { mats[mc].hasKd = true; mats[mc].kd = Color{(uint8_t)clampv(r * 255, 0.0f, 255.0f), (uint8_t)clampv(g * 255, 0.0f, 255.0f), (uint8_t)clampv(b * 255, 0.0f, 255.0f), 255}; }
                } else if (mc >= 0 && !strncmp(l.c_str(), "map_Kd ", 7) && tex.empty()) {
                    int c;
                    uint8_t* px = stbi_load(("assets/cars/" + l.substr(7)).c_str(), &tw, &th, &c, 3);
                    if (px) { tex.assign(px, px + (size_t)tw * th * 3); stbi_image_free(px); mats[mc].useTex = true; }
                } else if (mc >= 0 && !strncmp(l.c_str(), "map_Kd ", 7)) mats[mc].useTex = !tex.empty();
            }
        }
    }

    // Raeder: Gruppen, die zu einer Radecke gehoeren (mehrere Teile pro Rad moeglich)
    std::vector<int> corner(groups.size(), -1);
    std::vector<Vec3> gsum(groups.size());
    std::vector<int> gcnt(groups.size());
    for (size_t g = 0; g < groups.size(); g++) corner[g] = wheelCorner(groups[g]);
    for (const Face& fc : faces)
        for (const FaceV& q : fc.a) { gsum[fc.g] += P[q.v - 1]; gcnt[fc.g]++; }
    Vec3 F{0, 0, -1};
    {
        Vec3 fr{}, bk{};
        int nf = 0, nb = 0;
        for (size_t g = 0; g < groups.size(); g++) {
            if (corner[g] < 0 || !gcnt[g]) continue;
            Vec3 c = gsum[g] * (1.0f / gcnt[g]);
            if (corner[g] < 2) { fr += c; nf++; } else { bk += c; nb++; }
        }
        if (nf && nb) {
            Vec3 d = fr * (1.0f / nf) - bk * (1.0f / nb);
            d.y = 0;
            if (length(d) > 1e-4f) F = normalize(d);
        }
    }
    Vec3 R = cross(F, Vec3{0, 1, 0});
    auto toCar = [&](Vec3 p) { return Vec3{dot(p, R), p.y, -dot(p, F)}; };
    auto toCarN = [&](Vec3 p) { return normalize(Vec3{dot(p, R), p.y, -dot(p, F)}); };

    Vec3 lo{1e9f, 1e9f, 1e9f}, hi{-1e9f, -1e9f, -1e9f};
    for (const Vec3& p : P) {
        Vec3 c = toCar(p);
        lo = {std::min(lo.x, c.x), std::min(lo.y, c.y), std::min(lo.z, c.z)};
        hi = {std::max(hi.x, c.x), std::max(hi.y, c.y), std::max(hi.z, c.z)};
    }
    float s = targetLength(m.name) / std::max(0.01f, hi.z - lo.z);
    // Spielzeugproportionen (breit/hoch im Verhaeltnis zur Laenge) werden gestaucht, realistische Modelle bleiben unveraendert
    float aspect = (hi.x - lo.x) / std::max(0.01f, hi.z - lo.z), hasp = (hi.y - lo.y) / std::max(0.01f, hi.z - lo.z);
    const float sx = s * clampv(0.42f / std::max(0.01f, aspect), 0.65f, 1.0f), sy = s * clampv(0.34f / std::max(0.01f, hasp), 0.65f, 1.0f);
    float zc = (lo.z + hi.z) * 0.5f, xc = (lo.x + hi.x) * 0.5f;
    auto place = [&](Vec3 c) { return Vec3{(c.x - xc) * sx, (c.y - lo.y) * sy - 0.525f, (c.z - zc) * s + 0.14f}; };

    // Radmittelpunkte: Mitte des Begrenzungsquaders aller Teile eines Rades (im Autokoordinatensystem)
    Vec3 wlo[4], whi[4];
    bool wany[4] = {false, false, false, false};
    for (int i = 0; i < 4; i++) { wlo[i] = {1e9f, 1e9f, 1e9f}; whi[i] = {-1e9f, -1e9f, -1e9f}; }
    for (const Face& fc : faces) {
        int k = corner[fc.g];
        if (k < 0) continue;
        wany[k] = true;
        for (const FaceV& q : fc.a) {
            Vec3 c = toCar(P[q.v - 1]);
            wlo[k] = {std::min(wlo[k].x, c.x), std::min(wlo[k].y, c.y), std::min(wlo[k].z, c.z)};
            whi[k] = {std::max(whi[k].x, c.x), std::max(whi[k].y, c.y), std::max(whi[k].z, c.z)};
        }
    }
    bool wheelsOk = wany[0] && wany[1] && wany[2] && wany[3];
    Vec3 wctr[4];
    for (int i = 0; i < 4; i++) { wctr[i] = (wlo[i] + whi[i]) * 0.5f; if (wheelsOk) m.wc[i] = place(wctr[i]); }

    // Farben je Dreieck: Textur, Kd oder Materialname
    auto sampleColor = [&](const Face& fc) {
        const MatInfo* mi = fc.mat >= 0 ? &mats[fc.mat] : nullptr;
        if (mi && mi->useTex && !tex.empty()) {
            float u = 0, v = 0; int k = 0;
            for (const FaceV& q : fc.a) if (q.t >= 1 && q.t <= (int)U.size()) { u += U[q.t - 1]; v += V[q.t - 1]; k++; }
            if (k) {
                u /= k; v /= k;
                int px = clampv((int)std::floor((u - std::floor(u)) * tw), 0, tw - 1), py = clampv((int)std::floor((1.0f - (v - std::floor(v))) * th), 0, th - 1);
                const uint8_t* t = &tex[((size_t)py * tw + px) * 3];
                return Color{t[0], t[1], t[2], 255};
            }
        }
        if (mi && mi->hasKd) return mi->kd;
        Vec3 vc{};
        for (const FaceV& q : fc.a) vc += VC[q.v - 1];
        vc = vc * (1.0f / 3);
        if (mi && vc.x > 0.97f && vc.y > 0.97f && vc.z > 0.97f) return colorFromName(mi->name); // keine Farbinformation: nach dem Namen
        if (!mi) return Color{200, 200, 200, 255};
        return Color{(uint8_t)(vc.x * 255), (uint8_t)(vc.y * 255), (uint8_t)(vc.z * 255), 255};
    };
    std::vector<Color> fcol(faces.size());
    std::vector<char> isPaint(faces.size(), 0);
    bool namedPaint = false;
    for (size_t i = 0; i < faces.size(); i++) {
        fcol[i] = sampleColor(faces[i]);
        if (faces[i].mat >= 0 && has(lower(mats[faces[i].mat].name), "paint")) { isPaint[i] = 1; namedPaint = true; }
    }
    if (namedPaint) { // Material heisst "...paint": das ist der einfaerbbare Lack
        float sr = 0, sg = 0, sb = 0, sw = 0;
        for (size_t i = 0; i < faces.size(); i++) {
            if (!isPaint[i]) continue;
            Vec3 a = P[faces[i].a[0].v - 1], b = P[faces[i].a[1].v - 1], c = P[faces[i].a[2].v - 1];
            float area = length(cross(b - a, c - a)) * 0.5f + 1e-6f;
            sr += fcol[i].r * area; sg += fcol[i].g * area; sb += fcol[i].b * area; sw += area;
        }
        if (sw > 0) { m.ref = Color{(uint8_t)(sr / sw), (uint8_t)(sg / sw), (uint8_t)(sb / sw), 255}; m.hasPaint = true; }
    } else { // sonst: haeufigster gesaettigter Farbton der Karosserie
        float hueW[24] = {}, total = 0;
        for (size_t i = 0; i < faces.size(); i++) {
            const Face& fc = faces[i];
            if (corner[fc.g] >= 0) continue;
            Vec3 a = P[fc.a[0].v - 1], b = P[fc.a[1].v - 1], c = P[fc.a[2].v - 1];
            float area = length(cross(b - a, c - a)) * 0.5f, h, sa, va;
            rgbToHsv(fcol[i], h, sa, va);
            total += area;
            if (sa > 0.35f && va > 0.25f) hueW[(int)(h / 15.0f) % 24] += area;
        }
        int best = 0;
        for (int i = 1; i < 24; i++) if (hueW[i] > hueW[best]) best = i;
        float bestH = best * 15.0f + 7.5f;
        if (total > 0 && hueW[best] > total * 0.08f) {
            float sr = 0, sg = 0, sb = 0, sw = 0;
            for (size_t i = 0; i < faces.size(); i++) {
                const Face& fc = faces[i];
                if (corner[fc.g] >= 0) continue;
                float h, sa, va;
                rgbToHsv(fcol[i], h, sa, va);
                float dh = std::fabs(h - bestH);
                if (dh > 180) dh = 360 - dh;
                if (sa > 0.3f && va > 0.25f && dh < 22) {
                    isPaint[i] = 1;
                    Vec3 a = P[fc.a[0].v - 1], b = P[fc.a[1].v - 1], c = P[fc.a[2].v - 1];
                    float area = length(cross(b - a, c - a)) * 0.5f;
                    sr += fcol[i].r * area; sg += fcol[i].g * area; sb += fcol[i].b * area; sw += area;
                }
            }
            if (sw > 0) { m.ref = Color{(uint8_t)(sr / sw), (uint8_t)(sg / sw), (uint8_t)(sb / sw), 255}; m.hasPaint = true; }
        }
    }

    Batch paint, body, wheel[4], glassB;
    m.lamps.clear();
    for (size_t i = 0; i < faces.size(); i++) {
        const Face& fc = faces[i];
        int k = corner[fc.g];
        Batch* dst = &body;
        bool inWheel = k >= 0 && wheelsOk;
        if (inWheel) dst = &wheel[k];
        else if (isPaint[i] && m.hasPaint) dst = &paint;
        {   // Fensterglas: Materialname oder helles, kaum gesaettigtes Blau der Farbtextur; wird durchsichtig gezeichnet
            float gh, gs, gv;
            rgbToHsv(fcol[i], gh, gs, gv);
            bool nameGlass = fc.mat >= 0 && (has(lower(mats[fc.mat].name), "glass") || has(lower(mats[fc.mat].name), "window"));
            bool colGlass = gv > 0.8f && gs > 0.06f && gs < 0.4f && gh > 195 && gh < 240;
            if (!inWheel && (nameGlass || colGlass)) dst = &glassB;
        }
        Color c = fcol[i];
        if (dst == &glassB) c = Color{58, 78, 98, 120};
        if (dst == &paint) {
            c = Color{(uint8_t)std::min(255.0f, c.r / (m.ref.r * 1.4f + 1.0f) * 255.0f), (uint8_t)std::min(255.0f, c.g / (m.ref.g * 1.4f + 1.0f) * 255.0f),
                      (uint8_t)std::min(255.0f, c.b / (m.ref.b * 1.4f + 1.0f) * 255.0f), 255};
        }
        Vec3 pp[3];
        for (int q = 0; q < 3; q++) {
            Vec3 cc = toCar(P[fc.a[q].v - 1]);
            if (inWheel) { // Rad: um den eigenen Mittelpunkt skalieren, damit es sich sauber dreht
                Vec3 d = cc - wctr[k];
                pp[q] = place(wctr[k]) + Vec3{d.x * sx, d.y * sy, d.z * sy};
            } else pp[q] = place(cc);
        }
        Vec3 flat = normalize(cross(pp[1] - pp[0], pp[2] - pp[0]));
        int lk = fc.mat >= 0 && !inWheel ? lampKind(mats[fc.mat].name) : 0;
        if (lk > 0) { // Leuchten werden zur Laufzeit je nach Zustand eingefaerbt
            LampTri lt;
            for (int q = 0; q < 3; q++) lt.p[q] = pp[q];
            lt.n = flat; lt.kind = lk;
            lt.x = (pp[0].x + pp[1].x + pp[2].x) / 3; lt.z = (pp[0].z + pp[1].z + pp[2].z) / 3;
            m.lamps.push_back(lt);
            continue;
        }
        for (int q = 0; q < 3; q++) {
            Vec3 nn = fc.a[q].n >= 1 && fc.a[q].n <= (int)N.size() ? toCarN(N[fc.a[q].n - 1]) : flat;
            dst->vert(pp[q], nn, 0, 0, c);
        }
    }
    for (const Batch* bt : {&paint, &body})
        for (const auto& q : bt->v) m.surf.push_back({q.p[0], q.p[1], q.p[2]});
    m.paint.create(paint.v.data(), (int)paint.v.size());
    m.body.create(body.v.data(), (int)body.v.size());
    m.glass.create(glassB.v.data(), (int)glassB.v.size());
    for (int i = 0; i < 4; i++) m.wheel[i].create(wheel[i].v.data(), (int)wheel[i].v.size());
    m.hasWheels = wheelsOk;
    m.hasLamps = !m.lamps.empty();
    m.tris = (int)faces.size();
    m.lo = place(lo); m.hi = place(hi);
    float x0 = (lo.x - xc) * sx, x1 = (hi.x - xc) * sx;
    m.lo.x = x0; m.hi.x = x1;
    logMsg("Auto-Modell %s: %d Dreiecke, %.2f x %.2f x %.2f m, Raeder: %s, Lack: %s, Leuchten: %d", m.name.c_str(), (int)faces.size(), x1 - x0, m.hi.y - m.lo.y, m.hi.z - m.lo.z,
           m.hasWheels ? "ja" : "nein", m.hasPaint ? "ja" : "nein", (int)m.lamps.size());
    return true;
}

ObjCar* objCarAt(int i) {
    if (i < 0 || i >= (int)objCars.size()) return nullptr;
    ObjCar& m = objCars[i];
    if (!m.tried) { m.tried = true; m.ok = loadObjCar(m); if (!m.ok) logMsg("Auto-Modell %s konnte nicht geladen werden", m.name.c_str()); }
    return m.ok ? &m : nullptr;
}

void scanCars() {
    objCars.clear();
    std::error_code ec;
    std::vector<std::string> names;
    for (auto& e : std::filesystem::directory_iterator("assets/cars", ec))
        if (e.path().extension() == ".obj") names.push_back(e.path().stem().string());
    std::sort(names.begin(), names.end());
    for (auto& n : names) { ObjCar c; c.name = n; objCars.push_back(std::move(c)); }
    carModelSel = -1;
    if (cfg.carModel == "builtin") return;
    for (int i = 0; i < (int)objCars.size(); i++) if (objCars[i].name == cfg.carModel) carModelSel = i;
    if (carModelSel < 0 && !objCars.empty()) carModelSel = 0;
}
} // namespace

int carModelSel = -1;
int carModelCount() { return (int)objCars.size(); }
const std::string& carModelName(int i) { static const std::string builtin = "Eigenbau"; return i >= 0 && i < (int)objCars.size() ? objCars[i].name : builtin; }
int carModelFind(const std::string& name) { for (int i = 0; i < (int)objCars.size(); i++) if (objCars[i].name == name) return i; return -1; }
void carModelCycle() { carModelSel++; if (carModelSel >= (int)objCars.size()) carModelSel = -1; }

// ---------- Insassen: Fahrer (und Beifahrer) mit Kopf, Oberkoerper, Armen; wackeln mit der Fahrdynamik ----------
static Batch occB;
static void drawOccupants(const CarVisual& v) {
    if (!v.driver && !v.passenger) return;
    occB.clear();
    Color skin{232, 188, 158, 255}, hair{58, 40, 30, 255}, pants{40, 46, 66, 255};
    auto person = [&](float sx, Color shirt, float yaw, float pitch, bool hands) {
        float cx = sx + v.occX * 0.5f, cz = 0.33f + v.occZ * 0.5f;
        occB.box(I4, {sx, -0.03f, 0.30f}, {0.15f, 0.06f, 0.20f}, pants);          // Becken
        occB.box(I4, {sx, -0.16f, -0.02f}, {0.12f, 0.05f, 0.30f}, pants);         // Oberschenkel nach vorn
        occB.box(I4, {cx, 0.27f, cz}, {0.17f, 0.26f, 0.10f}, shirt);              // Oberkoerper
        Mat4 hm = mul(translate({sx + v.occX, 0.76f, 0.31f + v.occZ}), mul(rY(yaw), rX(pitch)));
        occB.box(hm, {0, 0, 0}, {0.10f, 0.115f, 0.10f}, skin);                      // Kopf
        occB.box(hm, {0, 0.085f, 0.005f}, {0.105f, 0.04f, 0.105f}, hair);
        occB.box(hm, {0, 0.0f, -0.10f}, {0.02f, 0.02f, 0.015f}, skin);              // Nase
        for (float s : {-1.0f, 1.0f}) {
            Vec3 sh{cx + s * 0.20f, 0.48f, cz - 0.02f};
            Vec3 hand = hands ? Vec3{sx + s * 0.15f, 0.20f, -0.28f} : Vec3{cx + s * 0.22f, 0.12f, cz - 0.20f};
            occB.beam(I4, sh, (sh + hand) * 0.5f + Vec3{s * 0.04f, -0.06f, 0}, 0.04f, shirt);
            occB.beam(I4, (sh + hand) * 0.5f + Vec3{s * 0.04f, -0.06f, 0}, hand, 0.035f, skin);
        }
    };
    if (v.driver) person(-0.38f, v.occShirt, v.headYaw, v.headPitch, true);
    if (v.passenger) person(0.38f, Color{(uint8_t)(255 - v.occShirt.r / 2), (uint8_t)(v.occShirt.b), (uint8_t)(v.occShirt.g), 255}, -v.headYaw * 0.5f, 0, false);
    gfxModel(v.M);
    gfxMode(0, FLAG_LIT | FLAG_FOG);
    occB.draw();
}


// Strahl entlang z durch (x,y): vorderste bzw. hinterste Flaeche der Karosserie
static bool surfZ(const ObjCar& m, float x, float y, bool rear, float& z) {
    bool found = false; float best = rear ? -1e9f : 1e9f;
    for (size_t i = 0; i + 2 < m.surf.size(); i += 3) {
        const Vec3 &a = m.surf[i], &b = m.surf[i + 1], &c = m.surf[i + 2];
        float d = (b.y - c.y) * (a.x - c.x) + (c.x - b.x) * (a.y - c.y);
        if (std::fabs(d) < 1e-9f) continue;
        float l1 = ((b.y - c.y) * (x - c.x) + (c.x - b.x) * (y - c.y)) / d, l2 = ((c.y - a.y) * (x - c.x) + (a.x - c.x) * (y - c.y)) / d, l3 = 1 - l1 - l2;
        if (l1 < -0.001f || l2 < -0.001f || l3 < -0.001f) continue;
        float zz = l1 * a.z + l2 * b.z + l3 * c.z;
        if (rear ? zz > best : zz < best) { best = zz; found = true; }
    }
    z = best;
    return found;
}
static void buildPads(ObjCar& m) {
    m.padsInit = true;
    float hw = std::max(std::fabs(m.lo.x), std::fabs(m.hi.x)), H = m.hi.y - m.lo.y, lw = hw * 0.62f;
    struct Spec { float xf, yf, hx, hy; int kind; bool rear; };
    const Spec spec[] = {{1.0f, 0.36f, 0.17f, 0.045f, 0, false}, {1.35f, 0.30f, 0.07f, 0.03f, 1, false},
                         {1.0f, 0.40f, 0.17f, 0.05f, 2, true}, {1.35f, 0.33f, 0.07f, 0.03f, 3, true}, {0.5f, 0.33f, 0.06f, 0.03f, 4, true}};
    for (const Spec& sp : spec)
        for (float sd : {-1.0f, 1.0f}) {
            if (sp.kind == 4 && sd > 0) continue; // Rueckfahrlicht nur einmal je Seite unten
            float x = sd * lw * sp.xf, y = m.lo.y + H * sp.yf, z = 0; bool ok = false;
            for (int t = 0; t < 12 && !ok; t++) {      // nach innen / oben wandern, bis die Karosserie getroffen wird
                float xx = x * (1.0f - 0.06f * (t % 6)), yy = y + 0.04f * (t / 6);
                if (surfZ(m, xx, yy, sp.rear, z) && (sp.rear ? z > m.hi.z - 0.45f : z < m.lo.z + 0.45f)) { x = xx; y = yy; ok = true; }
            }
            if (!ok) z = sp.rear ? m.hi.z : m.lo.z;
            m.pads.push_back({{x, y, z + (sp.rear ? -0.012f : 0.012f)}, {hw * sp.hx, H * sp.hy, 0.03f}, sp.kind, sd, sp.rear});
        }
}

static void drawObjCar(const CarVisual& v, const Mat4& vp, ObjCar& m) {
    gfxMVP(vp);
    gfxBias(0);
    float hw = std::max(std::fabs(m.lo.x), std::fabs(m.hi.x));
    shadow.clear();
    { float sw = hw * 0.92f; shadow.quad({-sw, -0.525f, m.lo.z + 0.1f}, {sw, -0.525f, m.lo.z + 0.1f}, {sw, -0.525f, m.hi.z - 0.1f}, {-sw, -0.525f, m.hi.z - 0.1f}, {0, 1, 0}, Color{0, 0, 0, 70}); }
    gfxModel(v.M);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);
    gfxMode(0, 0);
    gfxBias(2e-5f); shadow.draw(); gfxBias(0);   // Schatten leicht zur Kamera ziehen: kein Flackern gegen das Gelaende
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);

    if (m.hasPaint) {
        gfxMode(0, FLAG_LIT | FLAG_FOG | FLAG_GLOSS);
        Color p = v.tinted ? v.paint : m.ref;
        gfxTint(p.r / 255.0f * 1.4f, p.g / 255.0f * 1.4f, p.b / 255.0f * 1.4f);
        m.paint.draw();
    }
    gfxMode(0, FLAG_LIT | FLAG_FOG);
    m.body.draw();
    if (m.hasWheels)
        for (int i = 0; i < 4; i++) {
            bool front = i < 2;
            Vec3 c = m.wc[i];
            c.y += v.comp[i] - 0.127f;
            Mat4 W = mul(v.M, mul(translate(c), mul(rY(front ? -v.steer : 0.0f), mul(rX(-v.wheelRot[i]), translate(Vec3{-m.wc[i].x, -m.wc[i].y, -m.wc[i].z})))));
            gfxModel(W);
            m.wheel[i].draw();
        }
    // Leuchten
    dynGlow.clear();
    float H = m.hi.y - m.lo.y, lw = hw * 0.62f;
    float yHead = m.lo.y + H * 0.36f, yTail = m.lo.y + H * 0.40f, yBlink = m.lo.y + H * 0.30f;
    if (m.hasLamps) { // eigene Leuchtflaechen des Modells je nach Zustand einfaerben
        float zmid = (m.lo.z + m.hi.z) * 0.5f;
        for (const LampTri& lt : m.lamps) {
            bool rear = lt.z > zmid;
            Color c;
            switch (lt.kind) {
            case 1: c = v.brake ? Color{255, 36, 24, 255} : (v.lights ? Color{176, 16, 14, 255} : Color{112, 14, 14, 255}); break;
            case 2: c = (lt.x < 0 ? v.blinkL : v.blinkR) ? Color{255, 176, 24, 255} : Color{150, 100, 24, 255}; break;
            case 3: c = rear ? (v.reverse ? Color{255, 255, 255, 255} : Color{176, 176, 180, 255}) : (v.lights ? Color{255, 252, 235, 255} : Color{205, 208, 214, 255}); break;
            default: c = v.lights ? Color{255, 250, 225, 255} : Color{208, 216, 224, 255}; break;
            }
            for (int q = 0; q < 3; q++) dynGlow.vert(lt.p[q], lt.n, 0, 0, c);
        }
    }
    if (!m.hasLamps) {
        if (!m.padsInit) buildPads(m);
        for (const auto& pd : m.pads) {
            bool blink = pd.side < 0 ? v.blinkL : v.blinkR;
            Color c;
            switch (pd.kind) {
            case 0: c = v.lights ? Color{255, 250, 228, 255} : Color{214, 220, 226, 255}; break;
            case 1: case 3: c = blink ? Color{255, 170, 20, 255} : Color{150, 100, 24, 255}; break;
            case 2: c = v.brake ? Color{255, 34, 22, 255} : (v.lights ? Color{170, 14, 12, 255} : Color{110, 12, 12, 255}); break;
            default: if (!v.reverse) continue; c = Color{245, 245, 245, 255}; break;
            }
            dynGlow.box(I4, pd.c, pd.h, c);
        }
    }
    if (v.siren) { // Blaulicht-Balken auf dem Dach, wechselt im Takt
        bool a = std::fmod(simTime, 0.36) < 0.18;
        float zc = (m.lo.z + m.hi.z) * 0.5f + 0.1f;
        for (float s : {-1.0f, 1.0f}) {
            bool blue = s < 0;
            bool on = blue == a;
            Color c = blue ? (on ? Color{60, 110, 255, 255} : Color{20, 30, 70, 255}) : (on ? Color{255, 50, 40, 255} : Color{70, 14, 12, 255});
            dynGlow.box(I4, {s * hw * 0.3f, m.hi.y + 0.05f, zc}, {hw * 0.26f, 0.07f, 0.14f}, c);
        }
    }
    gfxModel(v.M);
    gfxMode(0, FLAG_FOG);
    dynGlow.draw();
    drawOccupants(v);
    if (m.glass.count > 0) { // Fensterglas durchsichtig ueber den Insassen
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDepthMask(GL_FALSE);
        gfxModel(v.M);
        gfxMode(0, FLAG_LIT | FLAG_FOG | FLAG_GLOSS);
        m.glass.draw();
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
    }
    gfxModel(I4);
}


// ---------- Moped / Motorrad (Derbi Senda 50 und ein 125er): selbst erzeugtes Modell, neigt sich in Kurven ----------
static Mesh mBikePaint, mBikeDark, mBikeFork, mBikeForkPaint, mBikeWheelM, mBikeRider;

static void buildBike() {
    Batch paint, dark, fork, forkPaint, rider;
    Color black{26, 27, 30, 255}, metal{158, 162, 170, 255}, rubber{18, 18, 20, 255};
    Color pw{255, 255, 255, 255};   // Lack: weiss, wird beim Zeichnen eingefaerbt
    // --- Lack: Tank, Seitenverkleidung, Heck ---
    paint.box(I4, {0, 0.16f, -0.20f}, {0.105f, 0.085f, 0.23f}, pw);
    paint.box(I4, {0, 0.215f, -0.20f}, {0.085f, 0.03f, 0.20f}, pw);
    for (float s : {-1.0f, 1.0f}) paint.box(I4, {s * 0.115f, 0.0f, 0.28f}, {0.012f, 0.10f, 0.22f}, pw);
    paint.box(I4, {0, 0.17f, 0.60f}, {0.075f, 0.055f, 0.17f}, pw);
    paint.box(I4, {0, 0.115f, 0.80f}, {0.06f, 0.012f, 0.14f}, pw);     // Schutzblech hinten
    // --- Motor, Rahmen, Sitz, Auspuff ---
    dark.box(I4, {0, -0.22f, 0.03f}, {0.115f, 0.14f, 0.20f}, Color{58, 60, 66, 255});
    dark.box(I4, {0, -0.12f, -0.15f}, {0.08f, 0.10f, 0.10f}, Color{44, 46, 50, 255});
    dark.box(I4, {0, 0.075f, 0.28f}, {0.095f, 0.04f, 0.32f}, black);                 // Sitz
    dark.beam(I4, {0, 0.30f, -0.50f}, {0, -0.02f, -0.08f}, 0.028f, black);           // Rahmenrohre
    dark.beam(I4, {0, -0.02f, -0.08f}, {0, 0.05f, 0.62f}, 0.026f, black);
    dark.beam(I4, {0.09f, -0.26f, 0.08f}, {0.115f, -0.20f, 0.70f}, 0.032f, metal);   // Auspuff
    dark.box(I4, {0.115f, -0.20f, 0.72f}, {0.04f, 0.04f, 0.16f}, metal);
    dark.beam(I4, {0, -0.34f, 0.60f}, {0, -0.24f, 0.62f}, 0.02f, black);            // Schwinge
    for (float s : {-1.0f, 1.0f}) {
        dark.beam(I4, {s * 0.07f, -0.28f, 0.10f}, {s * 0.07f, -0.225f, 0.62f}, 0.016f, metal); // Hinterradschwinge
        dark.box(I4, {s * 0.13f, -0.30f, 0.10f}, {0.02f, 0.012f, 0.07f}, metal);                // Fussraste
    }
    dark.box(I4, {0, 0.19f, 0.80f}, {0.06f, 0.035f, 0.01f}, Color{235, 235, 235, 255}); // Kennzeichen
    // --- Gabel, Lenker, Scheinwerfer, hohes Frontschutzblech (steht mit der Lenkung) ---
    for (float s : {-1.0f, 1.0f}) {
        fork.beam(I4, {s * 0.065f, 0.40f, -0.52f}, {s * 0.065f, -0.223f, -0.64f}, 0.022f, metal);
        fork.beam(I4, {s * 0.065f, 0.20f, -0.57f}, {s * 0.065f, -0.20f, -0.63f}, 0.03f, black);
        fork.box(I4, {s * 0.37f, 0.46f, -0.50f}, {0.05f, 0.022f, 0.022f}, rubber);         // Griffe
        fork.box(I4, {s * 0.33f, 0.52f, -0.52f}, {0.015f, 0.04f, 0.012f}, black);          // Spiegelarm
        fork.box(I4, {s * 0.36f, 0.58f, -0.52f}, {0.05f, 0.03f, 0.008f}, black);           // Spiegel
    }
    fork.beam(I4, {-0.33f, 0.46f, -0.50f}, {0.33f, 0.46f, -0.50f}, 0.014f, black);         // Lenker
    fork.box(I4, {0, 0.44f, -0.52f}, {0.04f, 0.04f, 0.04f}, black);
    fork.box(I4, {0, 0.34f, -0.68f}, {0.075f, 0.075f, 0.045f}, Color{232, 236, 240, 255}); // Scheinwerfer
    fork.box(I4, {0, 0.46f, -0.60f}, {0.06f, 0.04f, 0.03f}, black);                         // Tacho
    forkPaint.box(I4, {0, 0.115f, -0.66f}, {0.055f, 0.012f, 0.19f}, pw);                  // hohes Schutzblech
    forkPaint.box(I4, {0, 0.07f, -0.82f}, {0.055f, 0.045f, 0.012f}, pw);
    // --- Fahrer (nur in der Aussenansicht) ---
    Color jacket{48, 78, 150, 255}, pants{36, 42, 62, 255}, helmet{236, 236, 240, 255}, glove{30, 30, 34, 255};
    rider.box(I4, {0, 0.52f, 0.14f}, {0.16f, 0.25f, 0.10f}, jacket);
    rider.box(I4, {0, 0.88f, 0.05f}, {0.115f, 0.125f, 0.13f}, helmet);
    rider.box(I4, {0, 0.88f, -0.06f}, {0.10f, 0.04f, 0.04f}, Color{30, 34, 44, 255});     // Visier
    for (float s : {-1.0f, 1.0f}) {
        rider.beam(I4, {s * 0.19f, 0.68f, 0.10f}, {s * 0.30f, 0.52f, -0.20f}, 0.04f, jacket);
        rider.beam(I4, {s * 0.30f, 0.52f, -0.20f}, {s * 0.36f, 0.47f, -0.50f}, 0.035f, jacket);
        rider.box(I4, {s * 0.36f, 0.46f, -0.50f}, {0.04f, 0.035f, 0.04f}, glove);
        rider.beam(I4, {s * 0.10f, 0.20f, 0.24f}, {s * 0.20f, 0.12f, -0.14f}, 0.06f, pants);   // Oberschenkel
        rider.beam(I4, {s * 0.20f, 0.12f, -0.14f}, {s * 0.15f, -0.22f, -0.02f}, 0.05f, pants); // Unterschenkel
        rider.box(I4, {s * 0.15f, -0.30f, 0.0f}, {0.045f, 0.03f, 0.10f}, black);                // Schuh
    }
    mBikePaint.create(paint.v.data(), (int)paint.v.size());
    mBikeDark.create(dark.v.data(), (int)dark.v.size());
    mBikeFork.create(fork.v.data(), (int)fork.v.size());
    mBikeForkPaint.create(forkPaint.v.data(), (int)forkPaint.v.size());
    mBikeRider.create(rider.v.data(), (int)rider.v.size());
    // Rad: schmaler Reifen, helle Felge mit Speichen
    Batch wh;
    const int N = 24;
    const float Rw = 0.30f, hw = 0.045f;
    for (int i = 0; i < N; i++) {
        float a0 = i * 2 * PI / N, a1 = (i + 1) * 2 * PI / N, am = (a0 + a1) * 0.5f;
        auto P = [&](float x, float a, float r) { return Vec3{x, std::cos(a) * r, std::sin(a) * r}; };
        Vec3 n{0, std::cos(am), std::sin(am)};
        wh.quad(P(-hw, a0, Rw), P(hw, a0, Rw), P(hw, a1, Rw), P(-hw, a1, Rw), n, rubber);
        for (float s : {-1.0f, 1.0f}) {
            wh.quad(P(s * hw, a0, Rw), P(s * hw, a1, Rw), P(s * hw * 0.8f, a1, Rw - 0.05f), P(s * hw * 0.8f, a0, Rw - 0.05f), {s, 0, 0}, rubber);
            wh.quad(P(s * hw * 0.8f, a0, Rw - 0.05f), P(s * hw * 0.8f, a1, Rw - 0.05f), P(s * hw * 0.8f, a1, Rw - 0.075f), P(s * hw * 0.8f, a0, Rw - 0.075f), {s, 0, 0}, metal);
        }
    }
    for (int i = 0; i < 5; i++) { // Speichen
        float a = i * 2 * PI / 5;
        wh.box(rX(a), {0, 0.14f, 0}, {0.008f, 0.11f, 0.012f}, metal);
    }
    wh.box(I4, {0, 0, 0}, {0.05f, 0.03f, 0.03f}, black); // Nabe
    mBikeWheelM.create(wh.v.data(), (int)wh.v.size());
}

static void bikeDraw(const CarVisual& v, const Mat4& vp, bool cockpit) {
    gfxMVP(vp);
    gfxBias(0);
    Color paint = v.tinted ? v.paint : (v.kind == 1 ? Color{206, 34, 42, 255} : Color{38, 88, 180, 255});
    float sc = v.kind == 2 ? 1.12f : 1.0f;
    Mat4 lean = mul(translate({0, -0.525f, 0}), mul(rZ(v.lean), translate({0, 0.525f, 0})));
    Mat4 S = v.kind == 2 ? mul(translate({0, -0.525f, 0}), mul(scaleM(sc, sc, sc * 1.08f), translate({0, 0.525f, 0}))) : I4;
    Mat4 B = mul(v.M, mul(lean, S));
    // Schatten (schmal, nicht geneigt)
    shadow.clear();
    shadow.quad({-0.30f, -0.525f, -0.95f}, {0.30f, -0.525f, -0.95f}, {0.30f, -0.525f, 1.05f}, {-0.30f, -0.525f, 1.05f}, {0, 1, 0}, Color{0, 0, 0, 70});
    gfxModel(v.M);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);
    gfxMode(0, 0);
    gfxBias(2e-5f); shadow.draw(); gfxBias(0);   // Schatten leicht zur Kamera ziehen: kein Flackern gegen das Gelaende
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);

    float dyF = (v.comp[0] + v.comp[1]) * 0.5f - 0.127f, dyR = (v.comp[2] + v.comp[3]) * 0.5f - 0.127f;
    gfxModel(B);
    gfxMode(0, FLAG_LIT | FLAG_FOG | FLAG_GLOSS);
    gfxTint(paint.r / 255.0f, paint.g / 255.0f, paint.b / 255.0f);
    mBikePaint.draw();
    gfxMode(0, FLAG_LIT | FLAG_FOG);
    mBikeDark.draw();
    if (!cockpit) mBikeRider.draw();
    // Vorderbau: dreht mit der Lenkung
    Mat4 F = mul(B, mul(translate({0, dyF, -0.52f}), mul(rY(-v.steer), translate({0, 0, 0.52f}))));
    gfxModel(F);
    gfxMode(0, FLAG_LIT | FLAG_FOG | FLAG_GLOSS);
    gfxTint(paint.r / 255.0f, paint.g / 255.0f, paint.b / 255.0f);
    mBikeForkPaint.draw();
    gfxMode(0, FLAG_LIT | FLAG_FOG);
    mBikeFork.draw();
    gfxModel(mul(F, mul(translate({0, -0.223f, -0.64f}), rX(-v.wheelRot[0]))));
    mBikeWheelM.draw();
    gfxModel(mul(B, mul(translate({0, -0.223f + dyR, 0.64f}), rX(-v.wheelRot[2]))));
    mBikeWheelM.draw();
    // Leuchten
    dynGlow.clear();
    dynGlow.box(I4, {0, 0.34f, -0.715f}, {0.06f, 0.06f, 0.01f}, v.lights ? Color{255, 252, 230, 255} : Color{214, 220, 226, 255});
    dynGlow.box(I4, {0, 0.18f, 0.775f}, {0.04f, 0.022f, 0.01f}, v.brake ? Color{255, 36, 24, 255} : (v.lights ? Color{176, 16, 14, 255} : Color{112, 14, 14, 255}));
    for (float s : {-1.0f, 1.0f}) {
        bool blink = s < 0 ? v.blinkL : v.blinkR;
        Color c = blink ? Color{255, 176, 24, 255} : Color{150, 100, 24, 255};
        dynGlow.box(I4, {s * 0.30f, 0.40f, -0.52f}, {0.022f, 0.016f, 0.016f}, c);
        dynGlow.box(I4, {s * 0.09f, 0.14f, 0.82f}, {0.022f, 0.016f, 0.016f}, c);
    }
    gfxModel(B);
    gfxMode(0, FLAG_FOG);
    dynGlow.draw();
    gfxModel(I4);
}
void carModelInit() {
    buildBody();
    buildWheel();
    buildInterior();
    buildBike();
    scanCars();
}

static void textC(Batch& b, float cx, float y, float px, Color c, const std::string& s) { font.draw(b, cx - font.width(s, px) * 0.5f, y, px, c, s); }

static void buildDash(const CarVisual& v, const DashState& d) {
    dynGeo.clear(); dynText.clear(); dynLit.clear();
    Color red{240, 50, 40, 255}, green{70, 235, 110, 255}, amber{255, 185, 40, 255}, white{235, 238, 245, 255}, dim{110, 115, 125, 255};
    // --- Zeiger, Ganganzeige und Kontrollleuchten ---
    size_t g0 = dynGeo.v.size(), t0 = dynText.v.size();
    auto needle = [&](float cx, float frac) {
        float a = gaugeAngle(frac);
        Vec3 dd{std::cos(a), std::sin(a), 0}, p{-std::sin(a), std::cos(a), 0}, c{cx, 0, 1.6f};
        dynGeo.quad(c - dd * 9 - p * 1.5f, c - dd * 9 + p * 1.5f, c + dd * 50 + p * 0.5f, c + dd * 50 - p * 0.5f, {0, 0, 1}, red);
        dynGeo.disc(cx, 0, 6, Color{30, 30, 34, 255});
        for (size_t i = dynGeo.v.size() - 84; i < dynGeo.v.size(); i++) dynGeo.v[i].p[2] = 2.0f;
    };
    needle(-85, std::fabs(d.kmh) / 220.0f);
    needle(85, d.rpm / 7000.0f);
    std::string gear = d.automatic ? std::string(1, "PRND"[d.selector]) : (d.gear == -1 ? "R" : d.gear == 0 ? "N" : std::to_string(d.gear));
    textC(dynText, 0, 4, 26, amber, gear);
    if (d.automatic && d.selector == 3) textC(dynText, 0, 17, 9, dim, std::to_string(d.gear));
    auto arrow = [&](float x, float dir, bool on) {
        dynGeo.tri({x + dir * 9, -34, 1}, {x - dir * 3, -41, 1}, {x - dir * 3, -27, 1}, {0, 0, 1}, on ? green : Color{22, 40, 28, 255});
    };
    arrow(-15, -1, d.blinkL);
    arrow(15, 1, d.blinkR);
    float ly = 30;
    if (d.handbrake) { textC(dynText, -14, ly, 8, red, "(P)"); }
    if (!d.belt) { textC(dynText, 14, ly, 7, red, "GURT"); }
    if (!d.engineOn) textC(dynText, 0, 42, 6.5f, amber, "MOTOR AUS");
    else if (d.lights) textC(dynText, 0, 42, 6.5f, green, "LICHT");
    dynGeo.xform(g0, gaugeM);
    dynText.xform(t0, mul(gaugeM, translate({0, 0, 1.0f})));
    // --- Mitteldisplay: Tempolimit, Digitaltacho, Strassenname ---
    g0 = dynGeo.v.size(); t0 = dynText.v.size();
    char buf[64];
    if (d.limit > 0) {
        dynGeo.disc(-82, -8, 34, Color{215, 30, 35, 255});
        for (size_t i = dynGeo.v.size() - 84; i < dynGeo.v.size(); i++) dynGeo.v[i].p[2] = 0.15f;
        dynGeo.disc(-82, -8, 26, white);
        for (size_t i = dynGeo.v.size() - 84; i < dynGeo.v.size(); i++) dynGeo.v[i].p[2] = 0.35f;
        snprintf(buf, sizeof buf, "%d", d.limit);
        textC(dynText, -82, 1, 26, Color{10, 10, 10, 255}, buf);
    }
    snprintf(buf, sizeof buf, "%d", (int)std::lround(std::fabs(d.kmh)));
    bool fast = d.limit > 0 && std::fabs(d.kmh) > d.limit + 3;
    textC(dynText, 40, 8, 52, fast ? red : white, buf);
    textC(dynText, 40, 26, 11, dim, "km/h");
    if (!d.road.empty()) {
        std::string r = d.road;
        while (r.size() > 4 && font.width(r, 11) > 240) r.pop_back();
        textC(dynText, 0, 58, 11, Color{130, 190, 255, 255}, r);
    }
    snprintf(buf, sizeof buf, "%.1f km", d.odoKm);
    font.draw(dynText, 126 - font.width(buf, 9), -52, 9, dim, buf);
    { // Tankanzeige: Balken links unten im Mitteldisplay, rot bei wenig Sprit
        float ff = clampv(d.fuel, 0.0f, 1.0f);
        dynGeo.quad({-130, -50, 0.45f}, {-62, -50, 0.45f}, {-62, -45, 0.45f}, {-130, -45, 0.45f}, {0, 0, 1}, Color{36, 40, 48, 255});
        dynGeo.quad({-130, -50, 0.5f}, {-130 + 68 * ff, -50, 0.5f}, {-130 + 68 * ff, -45, 0.5f}, {-130, -45, 0.5f}, {0, 0, 1}, ff < 0.15f ? red : Color{110, 190, 255, 255});
        font.draw(dynText, -130, -52, 7, ff < 0.15f ? red : dim, "TANK");
    }
    dynGeo.xform(g0, mul(screenM, translate({0, 0, 0.3f})));
    dynText.xform(t0, mul(screenM, translate({0, 0, 1.4f})));
    // --- Schalthebel und Handbremse bewegen sich mit ---
    static const float gx[8] = {0.045f, 0, -0.03f, -0.03f, 0, 0, 0.03f, 0.03f}, gz[8] = {0.04f, 0, -0.04f, 0.04f, -0.04f, 0.04f, -0.04f, 0.04f};
    int gi = clampv(d.gear + 1, 0, 7);
    Vec3 base{0, 0.05f, -0.12f}, top = d.automatic ? Vec3{0, 0.17f, -0.16f + d.selector * 0.03f} : Vec3{gx[gi], 0.19f, -0.12f + gz[gi]};
    dynLit.beam(I4, base, top, 0.009f, Color{150, 152, 158, 255});
    dynLit.box(I4, top, {0.022f, 0.022f, 0.022f}, Color{20, 20, 22, 255});
    dynLit.beam(I4, {0.085f, 0.03f, 0.30f}, {0.085f, d.handbrake ? 0.15f : 0.05f, 0.10f}, 0.014f, Color{26, 26, 29, 255});
}

void carDraw(const CarVisual& vIn, const Mat4& vp, bool cockpit, const DashState* dash) {
    if (vIn.kind > 0) { bikeDraw(vIn, vp, cockpit); return; }
    int mi = vIn.model == MODEL_SELECTED ? carModelSel : vIn.model;
    ObjCar* oc = mi >= 0 ? objCarAt(mi) : nullptr;
    if (oc && !cockpit) { drawObjCar(vIn, vp, *oc); return; }
    CarVisual v = vIn;
    if (oc && oc->hasPaint && !v.tinted) v.paint = oc->ref; // Cockpit: Eigenbau-Haube in der Farbe des gewaehlten Autos
    gfxMVP(vp);
    gfxBias(0);
    // Schatten unter dem Auto
    shadow.clear();
    shadow.quad({-0.98f, -0.525f, -2.05f}, {0.98f, -0.525f, -2.05f}, {0.98f, -0.525f, 2.32f}, {-0.98f, -0.525f, 2.32f}, {0, 1, 0}, Color{0, 0, 0, 95});
    gfxModel(v.M);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);
    gfxMode(0, 0);
    gfxBias(2e-5f); shadow.draw(); gfxBias(0);   // Schatten leicht zur Kamera ziehen: kein Flackern gegen das Gelaende
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);

    gfxMode(0, FLAG_LIT | FLAG_FOG | FLAG_GLOSS);
    gfxTint(v.paint.r / 255.0f, v.paint.g / 255.0f, v.paint.b / 255.0f);
    mBody.draw();
    gfxMode(0, FLAG_LIT | FLAG_FOG);
    mTrim.draw();
    for (int i = 0; i < 4; i++) {
        bool left = i % 2 == 0, front = i < 2;
        Vec3 hub{left ? -0.80f : 0.80f, -0.05f - (0.30f - v.comp[i]), front ? -1.10f : 1.52f};
        Mat4 W = mul(v.M, mul(translate(hub), rY(front ? -v.steer : 0.0f)));
        W = left ? mul(W, mul(rY(PI), rX(v.wheelRot[i]))) : mul(W, rX(-v.wheelRot[i]));
        gfxModel(W);
        mWheel.draw();
    }
    // Leuchten
    dynGlow.clear();
    for (float s : {-1.0f, 1.0f}) {
        bool blink = s < 0 ? v.blinkL : v.blinkR;
        dynGlow.box(I4, {s * 0.55f, 0.015f, -1.87f}, {0.14f, 0.05f, 0.07f}, v.lights ? Color{255, 250, 228, 255} : Color{196, 202, 210, 255});
        dynGlow.box(I4, {s * 0.71f, -0.06f, -1.83f}, {0.045f, 0.03f, 0.05f}, blink ? Color{255, 170, 20, 255} : Color{130, 86, 22, 255});
        dynGlow.box(I4, {s * 0.63f, 0.27f, 2.215f}, {0.13f, 0.055f, 0.04f}, v.brake ? Color{255, 34, 22, 255} : (v.lights ? Color{160, 12, 10, 255} : Color{96, 10, 10, 255}));
        dynGlow.box(I4, {s * 0.66f, 0.17f, 2.225f}, {0.07f, 0.03f, 0.035f}, blink ? Color{255, 170, 20, 255} : Color{130, 86, 22, 255});
        if (v.reverse) dynGlow.box(I4, {s * 0.45f, 0.17f, 2.235f}, {0.05f, 0.03f, 0.03f}, Color{245, 245, 245, 255});
    }
    gfxModel(v.M);
    gfxMode(0, FLAG_FOG);
    dynGlow.draw();

    if (cockpit && dash) {
        buildDash(v, *dash);
        gfxMode(0, FLAG_LIT);
        mInterior.draw();
        dynLit.draw();
        gfxModel(mul(v.M, mul(translate({-0.36f, 0.20f, -0.30f}), mul(rX(-0.40f), rZ(-dash->steerInput * 7.85f)))));
        mSteer.draw();
        gfxModel(v.M);
        gfxMode(0, 0);
        gfxBias(1.2e-6f); // Zifferblaetter minimal vor die Instrumententafel legen (kein Flimmern)
        mDashGeo.draw();
        dynGeo.draw();
        glEnable(GL_BLEND);
        glDepthMask(GL_FALSE);
        gfxMode(5);
        gfxBindTexture(font.tex);
        mDashText.draw();
        dynText.draw();
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
        gfxBias(0);
    } else {
        drawOccupants(v);
        glEnable(GL_BLEND);
        gfxMode(0, FLAG_LIT | FLAG_FOG | FLAG_GLOSS);
        mGlass.draw();
        glDisable(GL_BLEND);
    }
    gfxModel(I4);
}

// ---------- Katalog: Infos, Spielerfarbe, 3D-Vorschau ----------
bool carColorSet = false;
uint8_t carColor[3] = {190, 30, 40};

void carOwnPaint(CarVisual& v) {
    if (carColorSet) { v.paint = Color{carColor[0], carColor[1], carColor[2], 255}; v.tinted = true; }
}

bool carModelInfo(int i, std::string& out) {
    ObjCar* m = objCarAt(i);
    if (!m) return false;
    char b[128];
    snprintf(b, sizeof b, "%.1f m lang  \xC2\xB7  %.1f m breit  \xC2\xB7  %d Dreiecke", m->hi.z - m->lo.z, m->hi.x - m->lo.x, m->tris);
    out = b;
    return true;
}

void carPreviewDraw(int x, int y, int w, int h, float yaw) {
    if (w < 8 || h < 8) return;
    glEnable(GL_SCISSOR_TEST);
    glScissor(x, y, w, h);
    glViewport(x, y, w, h);
    gfxEnv(1, 0, 1, 0, 0, {0.45f, 0.75f, 0.35f}); gfxHead({0, 0, 0}, {0, 0, 1}, 0);
    glClearColor(0.075f, 0.09f, 0.12f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glClearColor(0, 0, 0, 1);
    Vec3 cam{0, vehKind > 0 ? 1.1f : 1.9f, vehKind > 0 ? 6.2f : 11.0f};
    Mat4 proj = perspective(0.50f, (float)w / h, 0.1f, 200.0f);
    Mat4 view = rigidInverse(mul(translate(cam), rX(-0.17f)));
    Mat4 vp = mul(proj, view);
    gfxCamera(cam, invert(vp));
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    // Podest: dunkle Scheibe mit hellem Rand
    static Batch ground;
    ground.clear();
    for (int i = 0; i < 48; i++) {
        float a0 = i * 2 * PI / 48, a1 = (i + 1) * 2 * PI / 48;
        ground.tri({0, -0.53f, 0}, {std::cos(a0) * 3.6f, -0.53f, std::sin(a0) * 3.6f}, {std::cos(a1) * 3.6f, -0.53f, std::sin(a1) * 3.6f}, {0, 1, 0}, Color{34, 38, 48, 255});
        ground.tri({0, -0.529f, 0}, {std::cos(a0) * 3.3f, -0.529f, std::sin(a0) * 3.3f}, {std::cos(a1) * 3.3f, -0.529f, std::sin(a1) * 3.3f}, {0, 1, 0}, Color{22, 25, 32, 255});
    }
    gfxMVP(vp);
    gfxModel(I4);
    gfxBias(0);
    gfxMode(0, FLAG_LIT);
    ground.draw();
    CarVisual v;
    v.M = fromQuatPos(qaxis({0, 1, 0}, yaw), {0, 0, 0});
    v.kind = vehKind;
    v.lights = true;
    for (float& r : v.wheelRot) r = 0;
    carOwnPaint(v);
    carDraw(v, vp, false, nullptr);
    glDisable(GL_SCISSOR_TEST);
}

// Vorschaubild eines Katalog-Eintrags (modelIdx: Index in die Autoliste, -1 = Eigenbau; kind: 0 Auto, 1 Moped, 2 Motorrad) in ein Ziel rendern
void carThumbRender(Target& t, int modelIdx, int kind) {
    if (!t.fbo) t.create(192, 108);
    glBindFramebuffer(GL_FRAMEBUFFER, t.fbo);
    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, t.w, t.h);
    gfxEnv(1, 0, 1, 0, 0, {0.45f, 0.75f, 0.35f}); gfxHead({0, 0, 0}, {0, 0, 1}, 0);
    glClearColor(0.11f, 0.13f, 0.17f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glClearColor(0, 0, 0, 1);
    Vec3 cam{0, kind > 0 ? 1.0f : 1.7f, kind > 0 ? 5.6f : 10.2f};
    Mat4 vp = mul(perspective(0.50f, (float)t.w / t.h, 0.1f, 200.0f), rigidInverse(mul(translate(cam), rX(-0.17f))));
    gfxCamera(cam, invert(vp));
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    gfxMVP(vp);
    gfxModel(I4);
    gfxBias(0);
    CarVisual v;
    v.M = fromQuatPos(qaxis({0, 1, 0}, 0.75f), {0, 0, 0});
    v.kind = kind;
    v.model = modelIdx;
    v.lights = true;
    carOwnPaint(v);
    carDraw(v, vp, false, nullptr);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

int vehKind = 0;
