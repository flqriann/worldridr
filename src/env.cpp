// Tageszeit und Wetter: Sonnenstand, Himmelsfarben, Nebel, Regen/Schnee (Partikel), Nasse Strasse (Haftung), Gewitter.
// Der Host gibt Zeit und Wetter vor, Mitspieler uebernehmen sie (alle sehen dieselbe Tageszeit).
#include "game.h"
#include <atomic>
#include <cmath>
#include <ctime>
#include <thread>
#include <nlohmann/json.hpp>
#include "net.h"

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
bool realEnv = false;              // Tageszeit, Sonnenstand und Wetter vom Ort der Karte (Einstellung)
static Vec3 realSun{0, 1, 0};
static double realT = -100, wxT = -1e9, wxLat = 1e9, wxLon = 1e9;
static std::atomic<int> wxCode{-1};
static std::atomic<bool> wxBusy{false};

// Sonnenstand (NOAA-Naeherung) aus UTC-Zeit und Ort; Ortszeit naeherungsweise aus der Laenge (+ Sommerzeit in Europa)
static void computeReal() {
    std::time_t t = std::time(nullptr);
    std::tm u{};
#ifdef _WIN32
    gmtime_s(&u, &t);
#else
    gmtime_r(&t, &u);
#endif
    double lat = world.geo().lat0, lon = world.geo().lon0;
    double hUTC = u.tm_hour + u.tm_min / 60.0 + u.tm_sec / 3600.0;
    double gm = 2 * 3.14159265358979 / 365.0 * (u.tm_yday + (hUTC - 12) / 24.0);
    double eqt = 229.18 * (0.000075 + 0.001868 * std::cos(gm) - 0.032077 * std::sin(gm) - 0.014615 * std::cos(2 * gm) - 0.040849 * std::sin(2 * gm));
    double decl = 0.006918 - 0.399912 * std::cos(gm) + 0.070257 * std::sin(gm) - 0.006758 * std::cos(2 * gm) + 0.000907 * std::sin(2 * gm) - 0.002697 * std::cos(3 * gm) + 0.00148 * std::sin(3 * gm);
    double tst = hUTC * 60 + eqt + 4 * lon;                 // wahre Sonnenzeit in Minuten
    double ha = (tst / 4 - 180) * 3.14159265358979 / 180, la = lat * 3.14159265358979 / 180;
    double sinEl = std::sin(la) * std::sin(decl) + std::cos(la) * std::cos(decl) * std::cos(ha);
    double el = std::asin(clampv(sinEl, -1.0, 1.0));
    double az = std::atan2(std::sin(ha), std::cos(ha) * std::sin(la) - std::tan(decl) * std::cos(la)) + 3.14159265358979;   // von Norden im Uhrzeigersinn
    realSun = normalize(Vec3{(float)(std::sin(az) * std::cos(el)), (float)std::sin(el), (float)(-std::cos(az) * std::cos(el))});
    int tz = (int)std::lround(lon / 15.0);
    bool europe = lat > 34 && lat < 72 && lon > -25 && lon < 45;
    bool dst = false;
    if (europe) { // letzter Sonntag im Maerz bis letzter Sonntag im Oktober
        int m = u.tm_mon, dom = u.tm_mday, wd = u.tm_wday;
        int lastSun = dom - wd; while (lastSun + 7 <= 31) lastSun += 7;   // letzter Sonntag des Monats (Maerz/Oktober haben 31 Tage)
        dst = (m > 2 && m < 9) || (m == 2 && dom >= lastSun) || (m == 9 && dom < lastSun);
        tz = lon >= 22.5 ? 2 : 1;                                       // MEZ, weiter oestlich OEZ
        if ((lat > 49.8 && lon < 2.0) || (lat < 42.2 && lon < -6.2)) tz = 0;   // Grossbritannien/Irland, Portugal: WEZ
    }
    hour = (float)std::fmod(hUTC + tz + (dst ? 1 : 0) + 48.0, 24.0);
}
// Aktuelles Wetter am Ort (open-meteo.com, ohne Schluessel; gesendet werden nur die Kartenkoordinaten), alle 15 min
static void fetchWeather(double lat, double lon) {
    if (wxBusy.exchange(true)) return;
    std::thread([lat, lon]() {
        char url[256];
        char la[32], lo[32];   // Dezimalpunkt erzwingen (Gebietsschema koennte ein Komma liefern)
        snprintf(la, sizeof la, "%.3f", lat); snprintf(lo, sizeof lo, "%.3f", lon);
        for (char* q = la; *q; q++) if (*q == ',') *q = '.';
        for (char* q = lo; *q; q++) if (*q == ',') *q = '.';
        snprintf(url, sizeof url, "https://api.open-meteo.com/v1/forecast?latitude=%s&longitude=%s&current=weather_code,cloud_cover", la, lo);
        std::vector<uint8_t> d; int st = 0;
        bool got = false;
        for (int attempt = 0; attempt < 3 && !got; attempt++) got = (httpRequest(url, d, st) || !d.empty()) && st == 200 && !d.empty();   // manche Antworten melden beim Lesen das Ende als Fehler
        if (got) {
            auto j = nlohmann::json::parse(d.begin(), d.end(), nullptr, false);
            if (!j.is_discarded() && j.contains("current")) {
                int code = j["current"].value("weather_code", -1), cc = j["current"].value("cloud_cover", 0);
                int w = 0;
                if (code >= 95) w = 4;
                else if ((code >= 71 && code <= 77) || code == 85 || code == 86) w = 5;
                else if ((code >= 51 && code <= 67) || (code >= 80 && code <= 82)) w = 2;
                else if (code == 45 || code == 48) w = 3;
                else if (code >= 2 || cc > 60) w = 1;
                wxCode = w;
                logMsg("Echtes Wetter: Code %d, Bewoelkung %d%% -> %s", code, cc, weatherName[w]);
            }
        } else logMsg("Echtes Wetter nicht abrufbar (HTTP %d, %d Bytes: %.80s)", st, (int)d.size(), d.empty() ? "" : std::string(d.begin(), d.begin() + std::min<size_t>(80, d.size())).c_str());
        wxBusy = false;
    }).detach();
}

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
    if (realEnv && !remoteControlled()) return realSun;
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
    if (realEnv && !remoteControlled()) { // Ort und Uhrzeit bestimmen Sonne und Wetter
        if (simTime - realT > 1.0) { realT = simTime; computeReal(); }
        double lat = world.geo().lat0, lon = world.geo().lon0;
        if (simTime - wxT > 900 || std::hypot(lat - wxLat, lon - wxLon) > 0.2) { wxT = simTime; wxLat = lat; wxLon = lon; fetchWeather(lat, lon); }
        if (wxCode >= 0) weather = wxCode;
    } else hour += dt * timeScale() / 3600.0f;
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
