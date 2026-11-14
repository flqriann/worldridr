// Verkehrsschilder nach oesterreichischem/deutschem Vorbild: Stopp (Achteck), Vorrang geben (Dreieck), Tempolimit, Einbahn, Einfahrt verboten,
// Kreisverkehr, Autobahn, Fussgaengeruebergang; an Autobahnen Vorwegweiser, Wegweiserbruecken mit Wechselanzeige (VMS), Baken und Ausfahrtstafeln.
// Positionen und Ziele kommen aus den Kartendaten (World::forSignsNear / forControls); die Inhalte der Wechselanzeigen sind simuliert.
#include "game.h"

namespace signs {
namespace {
Batch geoLit, geoFace, geoGlow, textB;
float gK = 1.0f;   // Helligkeit der Schildflaechen (nachts gedimmt, Schilder reflektieren aber deutlich)

const Color WHITE{248, 248, 248, 255}, RED{205, 25, 30, 255}, BLUE{0, 84, 166, 255}, BLACK{15, 15, 15, 255}, STEEL{150, 154, 160, 255}, POLE{120, 124, 128, 255}, BACK{128, 132, 138, 255};
Color K(Color c) { return Color{(uint8_t)(c.r * gK), (uint8_t)(c.g * gK), (uint8_t)(c.b * gK), c.a}; }

struct Vms { int mode = 0; int limit = 0; bool stau = false; };   // mode 0 aus, 1 Tempolimit, 2 linke Spur gesperrt + Tempolimit

// Zustand der Anzeige einer Wegweiserbruecke: wechselt etwa alle 45 s, je Bruecke zeitversetzt
Vms vmsState(const SignDef& s, double now) {
    int slot = (int)std::floor(now / 45.0) + (int)(s.seed % 11);
    uint32_t h = (uint32_t)slot * 2654435761u ^ s.seed * 2246822519u;
    h ^= h >> 15; h *= 2246822519u; h ^= h >> 13;
    float r = (h & 0xFFFF) / 65535.0f;
    // Test: WR_VMS=1 Tempo 100, 2 linke Spur gesperrt, 3 Stau mit Tempo 60 erzwingen
    static const char* force = std::getenv("WR_VMS");
    if (force) {
        int m = atoi(force);
        return m == 1 ? Vms{1, 100, false} : (m == 2 ? Vms{2, 80, false} : (m == 3 ? Vms{1, 60, true} : Vms{}));
    }
    Vms v;
    if (r < 0.50f) return v;
    if (r < 0.72f) { v.mode = 1; v.limit = 100; }
    else if (r < 0.86f) { v.mode = 1; v.limit = 80; }
    else if (r < 0.94f) { v.mode = 1; v.limit = 60; v.stau = true; }
    else { v.mode = 2; v.limit = 80; }
    if (v.limit >= s.value) v = Vms{};   // gilt nur, wenn es strenger ist als das Grundtempo
    return v;
}

// Koordinatensystem einer Schildflaeche: x = rechts (aus Sicht des Fahrers), y = oben, z = zum Verkehr hin
Mat4 basis(Vec3 r, Vec3 u, Vec3 n, Vec3 p) {
    Mat4 m = identity();
    m.m[0] = r.x; m.m[1] = r.y; m.m[2] = r.z;
    m.m[4] = u.x; m.m[5] = u.y; m.m[6] = u.z;
    m.m[8] = n.x; m.m[9] = n.y; m.m[10] = n.z;
    m.m[12] = p.x; m.m[13] = p.y; m.m[14] = p.z;
    return m;
}
void rect(Batch& b, const Mat4& M, float cx, float cy, float w, float h, Color c, float z) {
    size_t s = b.v.size();
    b.rect(-w / 2, -h / 2, w, h, c);
    b.xform(s, mul(M, translate({cx, cy, z})));
}
void disc(Batch& b, const Mat4& M, float cx, float cy, float r, Color c, float z) {
    size_t s = b.v.size();
    b.disc(0, 0, r, c);
    b.xform(s, mul(M, translate({cx, cy, z})));
}
// konvexes Vieleck (Faecher), Punkte als x,y-Paare in der Schildebene
void poly(Batch& b, const Mat4& M, std::initializer_list<float> xy, Color c, float z) {
    const float* p = xy.begin();
    int n = (int)xy.size() / 2;
    size_t s = b.v.size();
    for (int i = 1; i + 1 < n; i++) b.quad({p[0], p[1], 0}, {p[i * 2], p[i * 2 + 1], 0}, {p[i * 2 + 2], p[i * 2 + 3], 0}, {p[i * 2 + 2], p[i * 2 + 3], 0}, {0, 0, 1}, c);
    b.xform(s, mul(M, translate({0, 0, z})));
}
void ngon(Batch& b, const Mat4& M, float cx, float cy, float r, int n, float rot, Color c, float z) {
    size_t s = b.v.size();
    for (int i = 0; i < n; i++) {
        float a0 = rot + i * 2 * PI / n, a1 = rot + (i + 1) * 2 * PI / n;
        Vec3 p0{cx + std::cos(a0) * r, cy + std::sin(a0) * r, 0}, p1{cx + std::cos(a1) * r, cy + std::sin(a1) * r, 0};
        b.quad({cx, cy, 0}, p0, p1, p1, {0, 0, 1}, c);
    }
    b.xform(s, mul(M, translate({0, 0, z})));
}
void bar(Batch& b, const Mat4& M, float x0, float y0, float x1, float y1, float half, Color c, float z) {
    float dx = x1 - x0, dy = y1 - y0, l = std::hypot(dx, dy);
    if (l < 1e-4f) return;
    float nx = -dy / l * half, ny = dx / l * half;
    size_t s = b.v.size();
    b.quad({x0 - nx, y0 - ny, 0}, {x1 - nx, y1 - ny, 0}, {x1 + nx, y1 + ny, 0}, {x0 + nx, y0 + ny, 0}, {0, 0, 1}, c);
    b.xform(s, mul(M, translate({0, 0, z})));
}
// Pfeil von (x0,y0) nach (x1,y1)
void arrow(Batch& b, const Mat4& M, float x0, float y0, float x1, float y1, float half, Color c, float z) {
    float dx = x1 - x0, dy = y1 - y0, l = std::hypot(dx, dy);
    if (l < 1e-4f) return;
    dx /= l; dy /= l;
    float hl = half * 4.2f, hw = half * 3.0f;
    bar(b, M, x0, y0, x1 - dx * hl * 0.8f, y1 - dy * hl * 0.8f, half, c, z);
    float bx = x1 - dx * hl, by = y1 - dy * hl;
    poly(b, M, {x1, y1, bx - dy * hw, by + dx * hw, bx + dy * hw, by - dx * hw}, c, z);
}
// Text: Grundlinie bei cy (y nach oben), horizontal zentriert; px = Schrifthoehe in Metern, wird verkleinert, bis er in maxW passt
void text(Batch& tb, const Mat4& M, float cx, float cy, float px, Color c, const std::string& s, float z, float maxW = 0) {
    if (s.empty()) return;
    if (maxW > 0) { float w = font.width(s, px); if (w > maxW) px *= maxW / w; }
    size_t st = tb.v.size();
    font.draw(tb, -font.width(s, px) * 0.5f, 0, px, K(c), s);   // Schrift hat y nach unten
    tb.xform(st, mul(M, mul(translate({cx, cy, z}), scaleM(1, -1, 1))));
}

struct Frame { Vec3 R, U{0, 1, 0}, N; };
Frame frameOf(const SignDef& s) {
    Frame f;
    f.R = {-s.tz, 0, s.tx};
    f.N = {-s.tx, 0, -s.tz};
    return f;
}
void pole(Vec3 p, float h, float r = 0.04f) { geoLit.box(identity(), {p.x, p.y + h * 0.5f, p.z}, {r, h * 0.5f, r}, POLE); }
// Schildebene in Hoehe h ueber dem Mastfuss, dem Verkehr zugewandt
Mat4 faceAt(const SignDef& s, float h) {
    Frame f = frameOf(s);
    return basis(f.R, f.U, f.N, {s.pos.x + f.N.x * 0.07f, s.pos.y + h, s.pos.z + f.N.z * 0.07f});
}

void speedFace(const Mat4& M, int value) {
    disc(geoFace, M, 0, 0, 0.44f, K(BACK), -0.012f);
    disc(geoFace, M, 0, 0, 0.42f, K(RED), 0.0f);
    disc(geoFace, M, 0, 0, 0.31f, K(WHITE), 0.004f);
    text(textB, M, 0, -0.11f, value >= 100 ? 0.26f : 0.32f, BLACK, std::to_string(value), 0.012f);
}
void stopFace(const Mat4& M) {
    ngon(geoFace, M, 0, 0, 0.47f, 8, PI / 8, K(BACK), -0.012f);
    ngon(geoFace, M, 0, 0, 0.46f, 8, PI / 8, K(WHITE), 0.0f);
    ngon(geoFace, M, 0, 0, 0.42f, 8, PI / 8, K(RED), 0.004f);
    text(textB, M, 0, -0.10f, 0.27f, WHITE, "STOP", 0.012f);
}
void giveWayFace(const Mat4& M) {
    const float a = 0.50f, h = a * 0.866f;
    poly(geoFace, M, {-a, h * 0.62f, 0, -h * 1.12f, a, h * 0.62f}, K(BACK), -0.012f);
    poly(geoFace, M, {-a, h * 0.62f, 0, -h * 1.12f, a, h * 0.62f}, K(RED), 0.0f);
    const float b = 0.30f, hb = b * 0.866f;
    poly(geoFace, M, {-b, hb * 0.62f + 0.035f, 0, -hb * 1.12f + 0.035f, b, hb * 0.62f + 0.035f}, K(WHITE), 0.004f);
}
void noEntryFace(const Mat4& M) {
    disc(geoFace, M, 0, 0, 0.44f, K(BACK), -0.012f);
    disc(geoFace, M, 0, 0, 0.42f, K(WHITE), 0.0f);
    disc(geoFace, M, 0, 0, 0.39f, K(RED), 0.004f);
    rect(geoFace, M, 0, 0, 0.56f, 0.13f, K(WHITE), 0.008f);
}
void roundaboutFace(const Mat4& M) {
    disc(geoFace, M, 0, 0, 0.42f, K(BACK), -0.012f);
    disc(geoFace, M, 0, 0, 0.40f, K(WHITE), 0.0f);
    disc(geoFace, M, 0, 0, 0.37f, K(BLUE), 0.004f);
    for (int i = 0; i < 3; i++) { // drei Pfeile im Gegenuhrzeigersinn
        float a0 = PI / 2 + i * 2 * PI / 3, r = 0.21f;
        for (int q = 0; q < 5; q++) {
            float b0 = a0 + q * 0.26f, b1 = a0 + (q + 1) * 0.26f;
            bar(geoFace, M, std::cos(b0) * r, std::sin(b0) * r, std::cos(b1) * r, std::sin(b1) * r, 0.035f, K(WHITE), 0.008f);
        }
        float e = a0 + 5 * 0.26f, tx = -std::sin(e), ty = std::cos(e), ex = std::cos(e) * r, ey = std::sin(e) * r;
        poly(geoFace, M, {ex + tx * 0.13f, ey + ty * 0.13f, ex - ty * 0.085f, ey + tx * 0.085f, ex + ty * 0.085f, ey - tx * 0.085f}, K(WHITE), 0.008f);
    }
}
void onewayFaces(const SignDef& s) { // Tafel parallel zur Strasse, Pfeil in Fahrtrichtung; von beiden Seiten lesbar
    Vec3 T{s.tx, 0, s.tz}, Nr{-s.tz, 0, s.tx};
    for (float side : {1.0f, -1.0f}) {
        Mat4 M = basis(T * side, {0, 1, 0}, Nr * (-side), {s.pos.x - Nr.x * side * 0.03f, s.pos.y + 2.45f, s.pos.z - Nr.z * side * 0.03f});
        rect(geoFace, M, 0, 0, 1.0f, 0.34f, K(WHITE), 0.0f);
        rect(geoFace, M, 0, 0, 0.96f, 0.30f, K(BLUE), 0.004f);
        arrow(geoFace, M, -0.36f * side, 0, 0.40f * side, 0, 0.045f, K(WHITE), 0.008f);
    }
}
void motorwayFace(const Mat4& M) {
    rect(geoFace, M, 0, 0, 0.92f, 0.92f, K(BACK), -0.012f);
    rect(geoFace, M, 0, 0, 0.90f, 0.90f, K(WHITE), 0.0f);
    rect(geoFace, M, 0, 0, 0.84f, 0.84f, K(BLUE), 0.004f);
    bar(geoFace, M, -0.20f, -0.32f, -0.07f, 0.30f, 0.035f, K(WHITE), 0.008f);   // zwei Fahrbahnen, die in die Ferne laufen
    bar(geoFace, M, 0.20f, -0.32f, 0.07f, 0.30f, 0.035f, K(WHITE), 0.008f);
    bar(geoFace, M, -0.33f, 0.07f, 0.33f, 0.07f, 0.04f, K(WHITE), 0.008f);      // Bruecke darueber
    bar(geoFace, M, -0.30f, 0.07f, -0.30f, -0.08f, 0.022f, K(WHITE), 0.008f);
    bar(geoFace, M, 0.30f, 0.07f, 0.30f, -0.08f, 0.022f, K(WHITE), 0.008f);
}
void crossingFace(const Mat4& M) {
    rect(geoFace, M, 0, 0, 0.82f, 0.82f, K(BACK), -0.012f);
    rect(geoFace, M, 0, 0, 0.80f, 0.80f, K(WHITE), 0.0f);
    rect(geoFace, M, 0, 0, 0.76f, 0.76f, K(BLUE), 0.004f);
    poly(geoFace, M, {-0.30f, -0.27f, 0.30f, -0.27f, 0, 0.30f}, K(WHITE), 0.008f);
    const float z = 0.012f;
    disc(geoFace, M, 0.01f, 0.115f, 0.038f, K(BLACK), z);                       // Kopf
    bar(geoFace, M, 0.0f, 0.07f, -0.01f, -0.07f, 0.026f, K(BLACK), z);          // Rumpf
    bar(geoFace, M, -0.01f, -0.07f, -0.08f, -0.20f, 0.02f, K(BLACK), z);        // Beine im Schritt
    bar(geoFace, M, -0.01f, -0.07f, 0.07f, -0.20f, 0.02f, K(BLACK), z);
    bar(geoFace, M, 0.0f, 0.04f, 0.08f, -0.03f, 0.014f, K(BLACK), z);           // Arme
    bar(geoFace, M, 0.0f, 0.04f, -0.07f, -0.02f, 0.014f, K(BLACK), z);
    for (float x : {-0.18f, -0.06f, 0.06f, 0.18f}) rect(geoFace, M, x, -0.235f, 0.07f, 0.035f, K(BLACK), z);   // Zebrastreifen
}

void bake(const SignDef& s) { // blaue Bake mit 3/2/1 weissen Schraegstreifen: 300/200/100 m vor der Ausfahrt
    pole(s.pos, 0.6f, 0.03f);
    Mat4 M = faceAt(s, 1.25f);
    rect(geoFace, M, 0, 0, 0.52f, 1.32f, K(WHITE), 0.0f);
    rect(geoFace, M, 0, 0, 0.48f, 1.28f, K(BLUE), 0.004f);
    int n = clampv(s.value, 1, 3);
    for (int i = 0; i < n; i++) {
        float cy = (i - (n - 1) * 0.5f) * 0.36f;
        bar(geoFace, M, -0.20f, cy - 0.12f, 0.20f, cy + 0.12f, 0.06f, K(WHITE), 0.008f);
    }
}

// blaue Tafel auf zwei Stehern: Kopfzeile, bis zu drei Ziele, Fusszeile (z. B. "1000 m")
void panel(const SignDef& s, float w, float h, float bottom, const std::string& head, const std::string& foot, bool arrowUpRight) {
    Frame f = frameOf(s);
    for (float x : {-w * 0.36f, w * 0.36f}) geoLit.box(identity(), {s.pos.x + f.R.x * x, s.pos.y + (bottom + h) * 0.5f, s.pos.z + f.R.z * x}, {0.06f, (bottom + h) * 0.5f, 0.06f}, POLE);
    Mat4 M = faceAt(s, bottom + h * 0.5f);
    rect(geoFace, M, 0, 0, w + 0.04f, h + 0.04f, K(BACK), -0.012f);
    rect(geoFace, M, 0, 0, w, h, K(WHITE), 0.0f);
    rect(geoFace, M, 0, 0, w - 0.10f, h - 0.10f, K(BLUE), 0.004f);
    const float zt = 0.012f, tw = w - 0.36f - (arrowUpRight ? 0.5f : 0.0f), tx = arrowUpRight ? -0.25f : 0.0f;
    std::vector<std::string> lines;
    for (const std::string* l : {&s.l1, &s.l2, &s.l3}) if (!l->empty()) lines.push_back(*l);
    float rows = (float)lines.size() + (head.empty() ? 0.0f : 0.85f) + (foot.empty() ? 0.0f : 0.85f);
    float lh = std::min(0.40f, (h - 0.30f) / std::max(1.0f, rows)), y = h * 0.5f - 0.18f;
    if (!head.empty()) { y -= lh * 0.80f; text(textB, M, tx, y + lh * 0.12f, lh * 0.62f, WHITE, head, zt, tw); }
    for (const std::string& l : lines) { y -= lh; text(textB, M, tx, y + lh * 0.14f, lh * 0.78f, WHITE, l, zt, tw); }
    if (!foot.empty()) { y -= lh * 0.85f; text(textB, M, tx, y + lh * 0.12f, lh * 0.66f, WHITE, foot, zt, tw); }
    if (arrowUpRight) arrow(geoFace, M, w * 0.5f - 0.62f, -0.25f, w * 0.5f - 0.22f, 0.25f, 0.05f, K(WHITE), 0.008f);
}
std::string exitHead(const SignDef& s) {
    if (s.head == "Knoten") return "Knoten";
    std::string h = s.ref.empty() ? "Ausfahrt" : "Ausfahrt " + s.ref;
    if (!s.head.empty()) h = s.head + (s.ref.empty() ? "" : "  (" + s.ref + ")");
    return h;
}

void gantry(const SignDef& s, double now) {
    Frame f = frameOf(s);
    Mat4 M = basis(f.R, f.U, f.N, s.pos);
    float W = s.hw + 2.2f;
    for (float x : {-W, W}) geoLit.box(M, {x, 3.1f, 0}, {0.3f, 3.1f, 0.3f}, STEEL);
    geoLit.box(M, {0, 5.6f, 0}, {W, 0.6f, 0.35f}, STEEL);
    const float zt = 0.386f;
    // rechts: Ausfahrt mit Zielen und Pfeil nach rechts oben; links: durchgehende Fahrbahn mit Pfeilen nach unten
    bool two = !s.thru.empty() && W > 5.0f;
    float pw = two ? std::min(W - 0.5f, 4.6f) : std::min(2.0f * W - 1.2f, 5.2f);
    float pcx = two ? W - pw * 0.5f - 0.3f : std::max(0.0f, s.hw * 0.25f);
    rect(geoFace, M, pcx, 7.55f, pw + 0.12f, 2.42f, K(WHITE), 0.37f);
    rect(geoFace, M, pcx, 7.55f, pw, 2.3f, K(BLUE), 0.374f);
    {
        std::vector<std::string> lines;
        for (const std::string* l : {&s.l1, &s.l2, &s.l3}) if (!l->empty()) lines.push_back(*l);
        float tw = pw - 1.0f, tcx = pcx - 0.3f, y = 8.52f;
        text(textB, M, tcx, y - 0.30f, 0.28f, WHITE, exitHead(s), zt, tw);
        float lh = lines.size() >= 3 ? 0.50f : 0.60f;
        y -= 0.40f;
        for (const std::string& l : lines) { y -= lh; text(textB, M, tcx, y + 0.08f, lh * 0.78f, WHITE, l, zt, tw); }
        arrow(geoFace, M, pcx + pw * 0.5f - 0.70f, 7.20f, pcx + pw * 0.5f - 0.22f, 7.85f, 0.06f, K(WHITE), 0.38f);
    }
    if (two) {
        float lw2 = std::min(W - 0.5f, 4.6f), lcx = -W + lw2 * 0.5f + 0.3f;
        rect(geoFace, M, lcx, 7.55f, lw2 + 0.12f, 2.42f, K(WHITE), 0.37f);
        rect(geoFace, M, lcx, 7.55f, lw2, 2.3f, K(BLUE), 0.374f);
        text(textB, M, lcx, 7.75f, 0.52f, WHITE, s.thru, zt, lw2 - 0.5f);
        int na = clampv(s.lanes - 1, 1, 3);
        for (int i = 0; i < na; i++) { float ax = lcx + (i - (na - 1) * 0.5f) * 1.1f; arrow(geoFace, M, ax, 7.35f, ax, 6.62f, 0.055f, K(WHITE), 0.38f); }
    }
    // Wechselanzeige je Fahrspur
    Vms v = vmsState(s, now);
    float lw = 2.0f * s.hw / (float)s.lanes;
    bool phase = std::fmod(now, 0.9) < 0.45;
    for (int j = 0; j < s.lanes; j++) {
        float x = -s.hw + (j + 0.5f) * lw;
        rect(geoLit, M, x, 5.6f, 1.12f, 1.12f, Color{30, 32, 36, 255}, 0.36f);
        rect(geoGlow, M, x, 5.6f, 0.98f, 0.98f, Color{4, 4, 6, 255}, 0.364f);
        if (v.mode == 0) { disc(geoGlow, M, x, 5.6f, 0.04f, Color{30, 90, 40, 255}, 0.368f); continue; }
        if (v.mode == 2 && j == 0) { // Spur gesperrt: rotes Kreuz
            bar(geoGlow, M, x - 0.32f, 5.28f, x + 0.32f, 5.92f, 0.07f, Color{255, 40, 30, 255}, 0.368f);
            bar(geoGlow, M, x - 0.32f, 5.92f, x + 0.32f, 5.28f, 0.07f, Color{255, 40, 30, 255}, 0.368f);
            continue;
        }
        disc(geoGlow, M, x, 5.6f, 0.45f, Color{235, 30, 30, 255}, 0.368f);
        disc(geoGlow, M, x, 5.6f, 0.34f, Color{4, 4, 6, 255}, 0.372f);
        { float keep = gK; gK = 1.0f; text(textB, M, x, 5.50f, v.limit >= 100 ? 0.26f : 0.32f, Color{255, 250, 235, 255}, std::to_string(v.limit), 0.378f); gK = keep; }
        // gelbe Blitzer wechselseitig
        disc(geoGlow, M, x - 0.42f, 6.08f, 0.07f, phase ? Color{255, 190, 20, 255} : Color{70, 50, 8, 255}, 0.368f);
        disc(geoGlow, M, x + 0.42f, 6.08f, 0.07f, phase ? Color{70, 50, 8, 255} : Color{255, 190, 20, 255}, 0.368f);
    }
    if (v.stau) { // Warnhinweis auf dem Balken (links)
        float cx = -s.hw * 0.55f;
        rect(geoGlow, M, cx, 6.75f, 1.9f, 0.62f, Color{6, 6, 8, 255}, 0.39f);
        float keep = gK; gK = 1.0f;
        text(textB, M, cx, 6.64f, 0.34f, phase ? Color{255, 190, 20, 255} : Color{255, 230, 140, 255}, "STAU", 0.40f);
        gK = keep;
    }
}

void drawSign(const SignDef& s, double now) {
    switch (s.kind) {
    case 0: pole(s.pos, 2.4f); speedFace(faceAt(s, 2.45f), s.value); break;
    case 1: gantry(s, now); break;
    case 2: panel(s, 2.3f, 1.25f, 1.5f, "Ausfahrt", "", true); break;
    case 5: pole(s.pos, 2.6f); onewayFaces(s); break;
    case 6: pole(s.pos, 2.4f); noEntryFace(faceAt(s, 2.45f)); break;
    case 7: pole(s.pos, 3.2f); giveWayFace(faceAt(s, 2.95f)); roundaboutFace(faceAt(s, 2.05f)); break;
    case 8: pole(s.pos, 2.5f); motorwayFace(faceAt(s, 2.55f)); break;
    case 10: pole(s.pos, 2.5f); crossingFace(faceAt(s, 2.55f)); break;
    case 11: bake(s); break;
    case 12: {
        char foot[32];
        snprintf(foot, sizeof foot, "%d m", s.value);
        panel(s, 3.6f, 2.5f, 1.7f, exitHead(s), foot, true);
        break;
    }
    default: break;
    }
}
} // namespace

void draw(const Mat4& vp, Vec3 cam, double now, bool mirrorPass) {
    if (mirrorPass) return;
    geoLit.clear(); geoFace.clear(); geoGlow.clear(); textB.clear();
    gK = 0.34f + 0.66f * envir::dayFactor();
    const float R = 320.0f;
    static const bool dbgOn = std::getenv("WR_SIGNDBG") != nullptr;
    static double dbgT = 0;
    bool dbg = dbgOn && now - dbgT > 2.0;
    if (dbg) dbgT = now;
    world.forSignsNear(cam.x, cam.z, R, [&](const SignDef& s) {
        float dx = s.pos.x - cam.x, dz = s.pos.z - cam.z, d2 = dx * dx + dz * dz;
        bool big = s.kind == 1 || s.kind == 12 || s.kind == 2;
        if (d2 > (big ? R * R : 170.0f * 170.0f)) return;
        drawSign(s, now);
        if (dbg && s.kind == 1) logMsg("Schild-Test: Wegweiserbruecke %.0f m entfernt, Hoehe %.1f (Kamera %.1f), Spuren %d, hw %.1f", std::sqrt(d2), s.pos.y, cam.y, s.lanes, s.hw);
    });
    // Stopp und Vorrang geben stehen bei den Ampeln/Haltelinien (Control)
    world.forControls(cam, 170.0f, [&](const Control& c) {
        if (c.type != 1 && c.type != 2) return;
        SignDef s;
        s.tx = c.dx; s.tz = c.dz;
        s.pos = {c.pos.x - c.dz * (c.hw + 0.9f), c.pos.y, c.pos.z + c.dx * (c.hw + 0.9f)};
        pole(s.pos, 2.4f);
        if (c.type == 1) stopFace(faceAt(s, 2.45f)); else giveWayFace(faceAt(s, 2.5f));
    });
    if (geoLit.v.empty() && geoFace.v.empty() && geoGlow.v.empty() && textB.v.empty()) return;
    gfxModel(identity());
    gfxMVP(vp);
    gfxBias(0);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    gfxMode(0, FLAG_LIT | FLAG_FOG);
    geoLit.draw();
    gfxMode(0, FLAG_FOG);
    geoFace.draw();
    geoGlow.draw();
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);
    gfxMode(5);
    gfxBindTexture(font.tex);
    textB.draw();
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
}

// Tempolimit, das die naechste Wechselanzeige vor dem Fahrzeug gerade vorgibt (0 = keine)
int dynamicLimit(Vec3 pos, Vec3 fwd) {
    int best = 0;
    float bestD = 1e9f;
    world.forSignsNear(pos.x, pos.z, 1300.0f, [&](const SignDef& s) {
        if (s.kind != 1) return;
        float dx = pos.x - s.pos.x, dz = pos.z - s.pos.z;
        float along = dx * s.tx + dz * s.tz, lat = -dx * s.tz + dz * s.tx;
        if (along < 0 || along > 1200.0f || std::fabs(lat) > s.hw + 12.0f || std::fabs(pos.y - s.pos.y) > 14.0f) return;
        if (fwd.x * s.tx + fwd.z * s.tz < 0.7f) return;
        if (along < bestD) { bestD = along; Vms v = vmsState(s, simTime); best = v.mode ? v.limit : 0; }
    });
    return best;
}

} // namespace signs
