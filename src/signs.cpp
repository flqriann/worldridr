// Verkehrsschilder: Tempolimit-Rundschilder, Autobahn-Hinweistafeln und Wegweiserbruecken mit Wechselanzeige (VMS).
// Positionen und Ziele kommen aus den Kartendaten (World::forSignsNear); die Anzeigeinhalte der Wechselanzeigen sind simuliert.
#include "game.h"

namespace signs {
namespace {
Batch geoLit, geoGlow, textB;

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
// Text: Grundlinie bei cy (y nach oben), horizontal zentriert; px = Schrifthoehe in Metern
void text(Batch& tb, const Mat4& M, float cx, float cy, float px, Color c, const std::string& s, float z) {
    if (s.empty()) return;
    size_t st = tb.v.size();
    font.draw(tb, -font.width(s, px) * 0.5f, 0, px, c, s);   // Schrift hat y nach unten
    tb.xform(st, mul(M, mul(translate({cx, cy, z}), scaleM(1, -1, 1))));
}
void bar(Batch& b, const Mat4& M, float x0, float y0, float x1, float y1, float half, Color c, float z) {
    float dx = x1 - x0, dy = y1 - y0, l = std::hypot(dx, dy);
    if (l < 1e-4f) return;
    float nx = -dy / l * half, ny = dx / l * half;
    size_t s = b.v.size();
    b.quad({x0 - nx, y0 - ny, 0}, {x1 - nx, y1 - ny, 0}, {x1 + nx, y1 + ny, 0}, {x0 + nx, y0 + ny, 0}, {0, 0, 1}, c);
    b.xform(s, mul(M, translate({0, 0, z})));
}

struct Frame { Vec3 R, U{0, 1, 0}, N; };
Frame frameOf(const SignDef& s) {
    Frame f;
    f.R = {-s.tz, 0, s.tx};
    f.N = {-s.tx, 0, -s.tz};
    return f;
}

void speedSign(const SignDef& s) {
    Frame f = frameOf(s);
    geoLit.box(identity(), {s.pos.x, s.pos.y + 1.2f, s.pos.z}, {0.04f, 1.2f, 0.04f}, Color{120, 124, 128, 255});
    Mat4 M = basis(f.R, f.U, f.N, {s.pos.x + f.N.x * 0.07f, s.pos.y + 2.45f, s.pos.z + f.N.z * 0.07f});
    disc(geoLit, M, 0, 0, 0.44f, Color{232, 232, 232, 255}, -0.004f);   // Rueckseite/Rand
    disc(geoLit, M, 0, 0, 0.42f, Color{205, 25, 30, 255}, 0.0f);
    disc(geoLit, M, 0, 0, 0.31f, Color{248, 248, 248, 255}, 0.004f);
    text(textB, M, 0, -0.11f, s.value >= 100 ? 0.26f : 0.32f, Color{15, 15, 15, 255}, std::to_string(s.value), 0.012f);
}

void exitPanel(const SignDef& s) {
    Frame f = frameOf(s);
    for (float x : {-0.7f, 0.7f}) geoLit.box(identity(), {s.pos.x + f.R.x * x, s.pos.y + 1.4f, s.pos.z + f.R.z * x}, {0.05f, 1.4f, 0.05f}, Color{120, 124, 128, 255});
    Mat4 M = basis(f.R, f.U, f.N, {s.pos.x + f.N.x * 0.08f, s.pos.y + 2.7f, s.pos.z + f.N.z * 0.08f});
    rect(geoLit, M, 0, 0, 1.9f, 1.3f, Color{240, 240, 240, 255}, 0.0f);
    rect(geoLit, M, 0, 0, 1.8f, 1.2f, Color{0, 84, 166, 255}, 0.004f);
    text(textB, M, 0, 0.30f, 0.20f, Color{255, 255, 255, 255}, s.ref.empty() ? "Ausfahrt" : "Ausfahrt " + s.ref, 0.012f);
    text(textB, M, 0, 0.04f, 0.22f, Color{255, 255, 255, 255}, s.l1, 0.012f);
    text(textB, M, 0, -0.22f, 0.22f, Color{255, 255, 255, 255}, s.l2, 0.012f);
    text(textB, M, 0, -0.48f, 0.22f, Color{255, 255, 255, 255}, s.l3, 0.012f);
}

void gantry(const SignDef& s, double now) {
    Frame f = frameOf(s);
    Mat4 M = basis(f.R, f.U, f.N, s.pos);
    float W = s.hw + 2.2f;
    Color steel{150, 154, 160, 255};
    for (float x : {-W, W}) geoLit.box(M, {x, 3.1f, 0}, {0.3f, 3.1f, 0.3f}, steel);
    geoLit.box(M, {0, 5.6f, 0}, {W, 0.6f, 0.35f}, steel);
    // Zieltafel (blau, wie auf Autobahnen) auf dem Balken
    float pw = std::min(2.0f * W - 1.2f, 5.2f), pcx = std::max(0.0f, s.hw * 0.25f);
    rect(geoLit, M, pcx, 7.55f, pw + 0.12f, 2.42f, Color{240, 240, 240, 255}, 0.37f);
    rect(geoLit, M, pcx, 7.55f, pw, 2.3f, Color{0, 84, 166, 255}, 0.374f);
    std::string head = s.ref.empty() ? "Ausfahrt" : "Ausfahrt " + s.ref;
    float zt = 0.385f;
    text(textB, M, pcx, 8.30f, 0.30f, Color{255, 255, 255, 255}, head, zt);
    text(textB, M, pcx, 7.76f, 0.42f, Color{255, 255, 255, 255}, s.l1, zt);
    text(textB, M, pcx, 7.24f, 0.42f, Color{255, 255, 255, 255}, s.l2, zt);
    text(textB, M, pcx, 6.72f, 0.42f, Color{255, 255, 255, 255}, s.l3, zt);
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
        text(textB, M, x, 5.50f, v.limit >= 100 ? 0.26f : 0.32f, Color{255, 250, 235, 255}, std::to_string(v.limit), 0.378f);
        // gelbe Blitzer wechselseitig
        disc(geoGlow, M, x - 0.42f, 6.08f, 0.07f, phase ? Color{255, 190, 20, 255} : Color{70, 50, 8, 255}, 0.368f);
        disc(geoGlow, M, x + 0.42f, 6.08f, 0.07f, phase ? Color{70, 50, 8, 255} : Color{255, 190, 20, 255}, 0.368f);
    }
    if (v.stau) { // Warnhinweis auf dem Balken (links)
        float cx = -s.hw * 0.55f;
        rect(geoGlow, M, cx, 6.75f, 1.9f, 0.62f, Color{6, 6, 8, 255}, 0.37f);
        text(textB, M, cx, 6.64f, 0.34f, phase ? Color{255, 190, 20, 255} : Color{255, 230, 140, 255}, "STAU", 0.378f);
    }
}
} // namespace

void draw(const Mat4& vp, Vec3 cam, double now, bool mirrorPass) {
    if (mirrorPass) return;
    geoLit.clear(); geoGlow.clear(); textB.clear();
    world.forSignsNear(cam.x, cam.z, 280.0f, [&](const SignDef& s) {
        float dx = s.pos.x - cam.x, dz = s.pos.z - cam.z;
        if (dx * dx + dz * dz > 280.0f * 280.0f) return;
        if (s.kind == 0) speedSign(s);
        else if (s.kind == 1) gantry(s, now);
        else exitPanel(s);
    });
    if (geoLit.v.empty() && geoGlow.v.empty() && textB.v.empty()) return;
    gfxModel(identity());
    gfxMVP(vp);
    gfxBias(0);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    gfxMode(0, FLAG_LIT | FLAG_FOG);
    geoLit.draw();
    gfxMode(0, FLAG_FOG);
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
