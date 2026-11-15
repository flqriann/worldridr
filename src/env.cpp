// Tageszeit und Wetter: Sonnenstand, Himmelsfarben, Nebel, Regen/Schnee (Partikel), Nasse Strasse (Haftung), Gewitter.
// Der Host gibt Zeit und Wetter vor, Mitspieler uebernehmen sie (alle sehen dieselbe Tageszeit).
#include "game.h"
#include <cmath>
#include <ctime>

namespace envir {
float hour = 12.0f;
int weather = 0;       // 0 Klar, 1 Wolkig, 2 Regen, 3 Nebel, 4 Gewitter, 5 Schnee
int speedIdx = 0;      // 0 Uhr steht, 1 Echtzeit, 2 x30, 3 x120
const char* weatherName[6] = {"Klar", "Wolkig", "Regen", "Nebel", "Gewitter", "Schnee"};
static const float speeds[4] = {0.0f, 1.0f, 30.0f, 120.0f};
static float oc = 0, fogK = 1, wet = 0, rain = 0, snow = 0, flash = 0, flashT = 8;
static double remoteT = -100;
static float remoteHour = 12; static int remoteWeather = 0, remoteSpeed = 0;
static std::vector<Vec3> dummy;

float timeScale() { return speeds[clampv(speedIdx, 0, 3)]; }
float systemHour() {
    std::time_t t = std::time(nullptr);
    std::tm lt{};
#ifdef _WIN32
    localtime_s(&lt, &t);
#else
    localtime_r(&t, &lt);
#endif
    return lt.tm_hour + lt.tm_min / 60.0f + lt.tm_sec / 3600.0f;
}
bool remoteControlled() { return simTime - remoteT < 4.0; }
void setRemote(float h, int w, int sp) { remoteT = simTime; remoteHour = h; remoteWeather = w; remoteSpeed = sp; }
void getState(float& h, int& w, int& sp) { h = hour; w = weather; sp = speedIdx; }

Vec3 sunDir() {
    float a = (hour - 6.0f) / 12.0f * PI;   // 6 Uhr Osten, 12 Uhr Sueden, 18 Uhr Westen
    return normalize({std::cos(a), std::sin(a) * 0.92f + 0.02f, 0.38f * std::sin(a)});
}
float dayFactor() { float e = sunDir().y; return clampv((e + 0.10f) / 0.32f, 0.0f, 1.0f); }
float night() { return 1.0f - dayFactor(); }
float grip() { return clampv(1.0f - 0.32f * wet - 0.50f * snow, 0.35f, 1.0f); }
float rainLevel() { return rain; }

void update(float dt) {
    if (remoteControlled()) { // Host bestimmt
        weather = remoteWeather;
        speedIdx = remoteSpeed;
        float d = std::remainder(remoteHour - hour, 24.0f);
        hour += std::fabs(d) > 0.5f ? d : d * std::min(1.0f, dt * 2.0f);
    }
    hour += dt * timeScale() / 3600.0f;
    if (hour >= 24.0f) hour -= 24.0f;
    if (hour < 0) hour += 24.0f;
    static const float tOc[6] = {0.0f, 0.62f, 0.9f, 0.55f, 1.0f, 0.8f};
    static const float tFog[6] = {1.0f, 1.25f, 2.2f, 9.0f, 3.0f, 2.4f};
    static const float tWet[6] = {0, 0.05f, 1.0f, 0.25f, 1.0f, 0.0f};
    static const float tRain[6] = {0, 0, 1.0f, 0, 1.6f, 0};
    static const float tSnow[6] = {0, 0, 0, 0, 0, 1.0f};
    int w = clampv(weather, 0, 5);
    float k = std::min(1.0f, dt * 0.25f);   // weicher Wechsel (ca. 4 s)
    oc += (tOc[w] - oc) * k; fogK += (tFog[w] - fogK) * k;
    wet += (tWet[w] - wet) * std::min(1.0f, dt * (tWet[w] > wet ? 0.15f : 0.03f));   // Strasse wird schnell nass, trocknet langsam
    rain += (tRain[w] - rain) * k; snow += (tSnow[w] - snow) * k;
    flash = std::max(0.0f, flash - dt * 3.5f);
    if (w == 4) {
        flashT -= dt;
        if (flashT <= 0) { flashT = 4.0f + (std::fmod((float)simTime * 7.31f, 1.0f)) * 12.0f; flash = 1.0f; }
    }
}

void apply() {
    float day = dayFactor();
    gfxEnv(std::min(1.0f, day + flash * 0.7f), std::min(1.0f, oc + flash * 0.2f), fogK, wet, snow, sunDir());
}

// Regen- und Schneepartikel rund um die Kamera
void drawPrecip(Vec3 cam, Vec3 camVel, double now) {
    if (rain < 0.03f && snow < 0.03f) return;
    static Batch pb;
    pb.clear();
    float day = dayFactor();
    float lum = 0.22f + 0.78f * day;
    const Mat4 I = identity();
    auto h1 = [](uint32_t i, uint32_t s) { uint32_t x = i * 747796405u + s * 2891336453u; x = ((x >> ((x >> 28) + 4)) ^ x) * 277803737u; x = (x >> 22) ^ x; return (x & 0xFFFF) / 65535.0f; };
    const float R = 16.0f, Hh = 10.0f;
    if (rain >= 0.03f) {
        int n = (int)(380 * std::min(1.0f, rain));
        Vec3 fall{0, -9.5f, 0};
        Vec3 rel = fall - camVel;
        float rl = length(rel);
        Vec3 dir = rel * (1.0f / std::max(rl, 0.01f));
        float len = clampv(rl * 0.055f, 0.25f, 3.0f);
        Color c{(uint8_t)(190 * lum), (uint8_t)(200 * lum), (uint8_t)(215 * lum), 110};
        for (int i = 0; i < n; i++) {
            float bx = h1(i, 1) * 2 * R, by = h1(i, 2) * 2 * Hh, bz = h1(i, 3) * 2 * R;
            float px = std::fmod(bx - (float)now * rel.x - cam.x + 5 * R, 2 * R) - R;
            float py = std::fmod(by - (float)now * rel.y - cam.y + 5 * Hh, 2 * Hh) - Hh;
            float pz = std::fmod(bz - (float)now * rel.z - cam.z + 5 * R, 2 * R) - R;
            Vec3 p = cam + Vec3{px, py, pz};
            pb.beam(I, p, p + dir * (-len), 0.006f, c);
        }
    }
    if (snow >= 0.03f) {
        int n = (int)(500 * std::min(1.0f, snow));
        Vec3 rel = Vec3{0.4f, -1.3f, 0.2f} - camVel;
        Color c{(uint8_t)(235 * lum), (uint8_t)(240 * lum), (uint8_t)(250 * lum), 200};
        for (int i = 0; i < n; i++) {
            float bx = h1(i, 4) * 2 * R, by = h1(i, 5) * 2 * Hh, bz = h1(i, 6) * 2 * R;
            float sw = std::sin((float)now * 0.9f + i) * 0.5f;
            float px = std::fmod(bx - (float)now * rel.x - cam.x + 5 * R, 2 * R) - R + sw;
            float py = std::fmod(by - (float)now * rel.y - cam.y + 5 * Hh, 2 * Hh) - Hh;
            float pz = std::fmod(bz - (float)now * rel.z - cam.z + 5 * R, 2 * R) - R;
            pb.box(I, cam + Vec3{px, py, pz}, {0.025f, 0.025f, 0.025f}, c);
        }
    }
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);
    gfxModel(I);
    gfxBias(0);
    gfxMode(0, 0);
    pb.draw();
    glDepthMask(GL_TRUE);
}
} // namespace envir
