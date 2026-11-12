// Automodell: geloftete Karosserie, Raeder mit Felgen, Innenraum und analoges 3D-Armaturenbrett
#include "game.h"

const Vec3 EYE{-0.36f, 0.60f, 0.22f};
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

void carModelInit() {
    buildBody();
    buildWheel();
    buildInterior();
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
        dynGeo.disc(-82, -8, 26, white);
        for (size_t i = dynGeo.v.size() - 84; i < dynGeo.v.size(); i++) dynGeo.v[i].p[2] = 0.3f;
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

void carDraw(const CarVisual& v, const Mat4& vp, bool cockpit, const DashState* dash) {
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
    shadow.draw();
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
    } else {
        glEnable(GL_BLEND);
        gfxMode(0, FLAG_LIT | FLAG_FOG | FLAG_GLOSS);
        mGlass.draw();
        glDisable(GL_BLEND);
    }
    gfxModel(I4);
}
