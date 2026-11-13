// Navigation: Route per OSRM (OpenStreetMap-Routing), Abbiegehinweise, Minimap (Fahrtrichtung oben) und Routenband in der 3D-Welt.
#include "game.h"
#include "net.h"
#include <nlohmann/json.hpp>
#include <atomic>
#include <mutex>
#include <thread>

namespace nav {
namespace {
using json = nlohmann::json;
typedef std::pair<double, double> LL;

struct Step { double lat = 0, lon = 0; std::string type, mod, name; int exit = 0; float at = 0, len = 0; };
struct Pending { int id = 0; bool ok = false; std::vector<LL> pts; std::vector<Step> steps; float dist = 0, dur = 0; };

std::mutex mx;
std::vector<Pending> pend;
std::atomic<int> reqSeq{0};
int curReq = 0;

bool hasDest = false, routed = false, requesting = false, approx = false;
double dLat = 0, dLon = 0, lastReq = -100;
std::vector<LL> ll;
std::vector<Step> steps;
std::vector<float> rx, rz, cum;
int builtGen = -1, segHint = 0;
float total = 0, osrmDist = 0, osrmDur = 0, along = 0, offDist = 0, offTimer = 0;
int zoomIdx = 1;
const float RANGES[4] = {150, 300, 700, 0};
Batch ribbon;

std::string fmtDist(float m) {
    char b[32];
    if (m >= 1000) { snprintf(b, sizeof b, "%.1f km", m / 1000.0f); for (char* p = b; *p; p++) if (*p == '.') *p = ','; }
    else if (m >= 100) snprintf(b, sizeof b, "%d m", (int)(std::round(m / 50.0f) * 50));
    else snprintf(b, sizeof b, "%d m", (int)(std::round(m / 10.0f) * 10));
    return b;
}
std::string fmtDur(float s) {
    int min = (int)std::round(s / 60.0f);
    char b[32];
    if (min >= 60) snprintf(b, sizeof b, "%d h %02d min", min / 60, min % 60); else snprintf(b, sizeof b, "%d min", std::max(1, min));
    return b;
}

void requestRoute() {
    double lat, lon;
    gamePosition(lat, lon);
    requesting = true;
    lastReq = simTime;
    int id = ++reqSeq;
    curReq = id;
    double tl = dLat, tn = dLon;
    std::thread([id, lat, lon, tl, tn] {
        Pending p;
        p.id = id;
        char url[400];
        snprintf(url, sizeof url, "https://router.project-osrm.org/route/v1/driving/%.6f,%.6f;%.6f,%.6f?overview=full&geometries=geojson&steps=true", lon, lat, tn, tl);
        std::vector<uint8_t> out;
        int st = 0;
        try {
            if (httpRequest(url, out, st) && st == 200) {
                json j = json::parse(out.begin(), out.end(), nullptr, false);
                if (!j.is_discarded() && j.value("code", "") == "Ok" && j.contains("routes") && !j["routes"].empty()) {
                    const json& r = j["routes"][0];
                    p.dist = (float)r.value("distance", 0.0);
                    p.dur = (float)r.value("duration", 0.0);
                    for (const json& c : r["geometry"]["coordinates"]) p.pts.push_back({c[1].get<double>(), c[0].get<double>()});
                    for (const json& leg : r["legs"])
                        for (const json& s : leg["steps"]) {
                            Step q;
                            const json& m = s["maneuver"];
                            q.type = m.value("type", "");
                            q.mod = m.value("modifier", "");
                            q.exit = m.value("exit", 0);
                            q.lon = m["location"][0].get<double>();
                            q.lat = m["location"][1].get<double>();
                            q.name = s.value("name", "");
                            q.len = (float)s.value("distance", 0.0);
                            p.steps.push_back(q);
                        }
                    p.ok = p.pts.size() >= 2;
                }
            }
        } catch (...) { p.ok = false; }
        if (!p.ok) logMsg("Routenberechnung fehlgeschlagen (HTTP %d)", st);
        std::lock_guard<std::mutex> lk(mx);
        pend.push_back(std::move(p));
    }).detach();
}

// Route in lokale Koordinaten umrechnen (nach Ortswechsel neu) und Abbiegepunkte auf der Route verorten
void rebuildLocal() {
    int n = (int)ll.size();
    rx.resize(n); rz.resize(n); cum.assign(n, 0);
    for (int i = 0; i < n; i++) {
        world.toLocal(ll[i].first, ll[i].second, rx[i], rz[i]);
        if (i) cum[i] = cum[i - 1] + std::hypot(rx[i] - rx[i - 1], rz[i] - rz[i - 1]);
    }
    total = n ? cum[n - 1] : 0;
    int prev = 0;
    for (Step& s : steps) {
        float x, z;
        world.toLocal(s.lat, s.lon, x, z);
        int best = prev;
        float bd = 1e18f;
        for (int j = prev; j < n; j++) {
            float d = (rx[j] - x) * (rx[j] - x) + (rz[j] - z) * (rz[j] - z);
            if (d < bd) { bd = d; best = j; }
        }
        s.at = cum[best];
        prev = best;
    }
    builtGen = world.geo().gen;
    segHint = 0;
}

void project() {
    int n = (int)rx.size();
    if (n < 2) return;
    Vec3 p = car.pos;
    auto scan = [&](int a, int b, float& bd, int& bi, float& bt) {
        for (int i = a; i < b; i++) {
            float vx = rx[i + 1] - rx[i], vz = rz[i + 1] - rz[i], l2 = vx * vx + vz * vz;
            float t = l2 > 1e-6f ? clampv(((p.x - rx[i]) * vx + (p.z - rz[i]) * vz) / l2, 0.0f, 1.0f) : 0.0f;
            float d = std::hypot(p.x - (rx[i] + vx * t), p.z - (rz[i] + vz * t));
            if (d < bd) { bd = d; bi = i; bt = t; }
        }
    };
    float bd = 1e18f, bt = 0;
    int bi = segHint;
    scan(std::max(0, segHint - 4), std::min(n - 1, segHint + 150), bd, bi, bt);
    if (bd > 60) scan(0, n - 1, bd, bi, bt);
    segHint = bi;
    offDist = bd;
    along = cum[bi] + bt * (cum[bi + 1] - cum[bi]);
}

std::string instruction(const Step& s) {
    std::string on = s.name.empty() ? "" : " auf " + s.name;
    const std::string& m = s.mod;
    auto dirText = [&](const char* verb) -> std::string {
        if (m == "uturn") return "Wenden";
        if (m == "sharp left") return std::string("Scharf links ") + verb;
        if (m == "sharp right") return std::string("Scharf rechts ") + verb;
        if (m == "slight left") return std::string("Leicht links ") + verb;
        if (m == "slight right") return std::string("Leicht rechts ") + verb;
        if (m == "left") return std::string("Links ") + verb;
        if (m == "right") return std::string("Rechts ") + verb;
        return "Geradeaus weiter";
    };
    if (s.type == "arrive") return m == "left" ? "Ziel liegt links" : (m == "right" ? "Ziel liegt rechts" : "Ziel erreicht");
    if (s.type == "depart") return "Losfahren" + on;
    if (s.type == "roundabout" || s.type == "rotary" || s.type == "roundabout turn" || s.type == "exit roundabout" || s.type == "exit rotary")
        return s.exit > 0 ? "Im Kreisverkehr die " + std::to_string(s.exit) + ". Ausfahrt nehmen" + on : "Kreisverkehr" + on;
    if (s.type == "merge") return "Einf\xC3\xA4" "deln" + on;
    if (s.type == "on ramp") return "Auffahrt nehmen" + on;
    if (s.type == "off ramp") return "Ausfahrt nehmen" + on;
    if (s.type == "fork") return std::string(m.find("left") != std::string::npos ? "Links" : "Rechts") + " halten" + on;
    if (s.type == "new name" || s.type == "continue") return (m == "straight" || m.empty() ? "Weiter" : dirText("abbiegen")) + on;
    if (s.type == "end of road") return dirText("abbiegen") + on;
    return dirText("abbiegen") + on;
}

float arrowAngle(const Step& s) {
    const std::string& m = s.mod;
    if (m == "left") return -PI / 2;
    if (m == "right") return PI / 2;
    if (m == "sharp left") return -PI * 0.78f;
    if (m == "sharp right") return PI * 0.78f;
    if (m == "slight left") return -PI / 5;
    if (m == "slight right") return PI / 5;
    if (m == "uturn") return PI;
    return 0;
}

// ---------- Zeichenhilfen (Oberflaeche) ----------
void quadLine(float x0, float y0, float x1, float y1, float w, Color c) {
    float dx = x1 - x0, dy = y1 - y0, l = std::hypot(dx, dy);
    if (l < 1e-4f) return;
    float nx = -dy / l * w * 0.5f, ny = dx / l * w * 0.5f;
    ui.geo.quad({x0 - nx, y0 - ny, 0}, {x0 + nx, y0 + ny, 0}, {x1 + nx, y1 + ny, 0}, {x1 - nx, y1 - ny, 0}, {0, 0, 1}, c);
}
void circle(float cx, float cy, float r, Color c, int n = 64) {
    for (int i = 0; i < n; i++) {
        float a0 = i * 2 * PI / n, a1 = (i + 1) * 2 * PI / n;
        ui.geo.tri({cx, cy, 0}, {cx + std::cos(a0) * r, cy + std::sin(a0) * r, 0}, {cx + std::cos(a1) * r, cy + std::sin(a1) * r, 0}, {0, 0, 1}, c);
    }
}
void ring(float cx, float cy, float r, float w, Color c, int n = 32) {
    for (int i = 0; i < n; i++) {
        float a0 = i * 2 * PI / n, a1 = (i + 1) * 2 * PI / n;
        quadLine(cx + std::cos(a0) * r, cy + std::sin(a0) * r, cx + std::cos(a1) * r, cy + std::sin(a1) * r, w, c);
    }
}
void head(float x, float y, float dx, float dy, float s, Color c) { // Pfeilspitze in Richtung (dx,dy)
    float l = std::hypot(dx, dy);
    if (l < 1e-4f) return;
    dx /= l; dy /= l;
    ui.geo.tri({x + dx * s, y + dy * s, 0}, {x - dy * s * 0.8f - dx * s * 0.2f, y + dx * s * 0.8f - dy * s * 0.2f, 0}, {x + dy * s * 0.8f - dx * s * 0.2f, y - dx * s * 0.8f - dy * s * 0.2f, 0}, {0, 0, 1}, c);
}
void turnIcon(float cx, float cy, float s, const Step& st, Color c) {
    if (st.type == "arrive") {
        circle(cx, cy - s * 0.2f, s * 0.42f, Color{240, 70, 60, 255});
        circle(cx, cy - s * 0.2f, s * 0.16f, Color{255, 255, 255, 255});
        return;
    }
    bool round = st.type.find("roundabout") != std::string::npos || st.type.find("rotary") != std::string::npos;
    float a = arrowAngle(st);
    if (round) { ring(cx, cy - s * 0.05f, s * 0.38f, s * 0.12f, c); a = st.mod.empty() ? 0 : a; }
    float w = s * 0.2f;
    if (st.mod == "uturn") {
        float xl = cx - s * 0.32f, xr = cx + s * 0.32f, yt = cy - s * 0.55f;
        quadLine(xr, cy + s * 0.9f, xr, yt, w, c);
        quadLine(xr + w * 0.5f, yt, xl - w * 0.5f, yt, w, c);
        quadLine(xl, yt, xl, cy + s * 0.2f, w, c);
        head(xl, cy + s * 0.2f, 0, 1, s * 0.5f, c);
        return;
    }
    float ex = cx + std::sin(a) * s * 0.85f, ey = cy - std::cos(a) * s * 0.85f;
    quadLine(cx, cy + s * 0.9f, cx, cy, w, c);
    quadLine(cx - w * 0.5f * (a == 0 ? 0 : 1), cy, ex - std::sin(a) * s * 0.25f, ey + std::cos(a) * s * 0.25f, w, c);
    head(ex, ey, std::sin(a), -std::cos(a), s * 0.5f, c);
}

bool clipCircle(float& x0, float& y0, float& x1, float& y1, float R) { // Koordinaten relativ zum Mittelpunkt
    float dx = x1 - x0, dy = y1 - y0, a = dx * dx + dy * dy;
    float c = x0 * x0 + y0 * y0 - R * R;
    if (a < 1e-6f) return c <= 0;
    float b = 2 * (x0 * dx + y0 * dy), disc = b * b - 4 * a * c;
    if (disc < 0) return false;
    float sq = std::sqrt(disc), t0 = std::max(0.0f, (-b - sq) / (2 * a)), t1 = std::min(1.0f, (-b + sq) / (2 * a));
    if (t0 >= t1) return false;
    float nx0 = x0 + dx * t0, ny0 = y0 + dy * t0, nx1 = x0 + dx * t1, ny1 = y0 + dy * t1;
    x0 = nx0; y0 = ny0; x1 = nx1; y1 = ny1;
    return true;
}

void buildMinimap(float W, float H) {
    float range = RANGES[zoomIdx];
    if (range <= 0) return;
    const float R = 108;
    float cx = W - R - 24, cy = H - R - 34, k = R / range;
    Vec3 fw = car.forward();
    float fl = std::hypot(fw.x, fw.z);
    float fx = fl > 1e-4f ? fw.x / fl : 0, fz = fl > 1e-4f ? fw.z / fl : -1;
    float rxv = -fz, rzv = fx;
    auto toS = [&](float wx, float wz, float& sx, float& sy) {
        float dx = wx - car.pos.x, dz = wz - car.pos.z;
        sx = (dx * rxv + dz * rzv) * k;
        sy = -(dx * fx + dz * fz) * k;
    };
    circle(cx, cy, R + 5, Color{10, 12, 18, 235});
    circle(cx, cy, R, Color{27, 33, 44, 245});
    auto line = [&](float ax, float az, float bx, float bz, float w, Color c) {
        float x0, y0, x1, y1;
        toS(ax, az, x0, y0); toS(bx, bz, x1, y1);
        if (!clipCircle(x0, y0, x1, y1, R)) return;
        quadLine(cx + x0, cy + y0, cx + x1, cy + y1, w, c);
    };
    // Gebaeude
    float reach = range * 1.15f;
    int drawn = 0;
    world.forBuildingsNear(car.pos.x, car.pos.z, reach, [&](const Building& b) {
        if (drawn > 900) return;
        size_t n = b.poly.size() / 2;
        for (size_t i = 0; i < n; i++) {
            size_t j = (i + 1) % n;
            line(b.poly[i * 2], b.poly[i * 2 + 1], b.poly[j * 2], b.poly[j * 2 + 1], 1.3f, Color{58, 66, 82, 255});
        }
        drawn++;
    });
    // Strassen: erst alle Raender, dann alle Flaechen (kleine zuerst)
    std::vector<RoadSeg> cls[8];
    int cnt = 0;
    world.forSegmentsNear(car.pos.x, car.pos.z, reach, [&](const RoadSeg& s) {
        if (cnt > 9000) return;
        int kl = std::min<int>(s.klass, 7);
        if (range > 300 && kl >= 7) return;
        if (range > 500 && kl >= 6) return;
        cls[kl].push_back(s);
        cnt++;
    });
    static const Color fill[8] = {{255, 170, 70, 255}, {255, 190, 90, 255}, {255, 226, 150, 255}, {250, 236, 190, 255}, {232, 234, 240, 255}, {208, 212, 222, 255}, {150, 158, 172, 255}, {110, 112, 104, 255}};
    static const float wpx[8] = {5.2f, 5.0f, 4.6f, 4.2f, 3.6f, 3.2f, 2.0f, 1.6f};
    float sc = range <= 150 ? 1.4f : (range <= 300 ? 1.1f : 0.85f);
    for (int pass = 0; pass < 2; pass++)
        for (int kl = 7; kl >= 0; kl--)
            for (const RoadSeg& s : cls[kl]) {
                float w = wpx[kl] * sc;
                if (pass == 0) line(s.ax, s.az, s.bx, s.bz, w + 2.0f, Color{12, 14, 20, 255});
                else line(s.ax, s.az, s.bx, s.bz, w, fill[kl]);
            }
    // Route
    if (routed && rx.size() >= 2) {
        for (int pass = 0; pass < 2; pass++)
            for (size_t i = 0; i + 1 < rx.size(); i++) {
                if (cum[i + 1] < along - 40) continue;
                if (std::fabs(rx[i] - car.pos.x) > reach * 1.5f && std::fabs(rx[i + 1] - car.pos.x) > reach * 1.5f) continue;
                if (pass == 0) line(rx[i], rz[i], rx[i + 1], rz[i + 1], 8.5f, Color{255, 255, 255, 255});
                else line(rx[i], rz[i], rx[i + 1], rz[i + 1], 5.5f, approx ? Color{255, 170, 60, 255} : Color{38, 132, 255, 255});
            }
    }
    // Ziel
    if (hasDest) {
        float tx, tz;
        world.toLocal(dLat, dLon, tx, tz);
        float sx, sy;
        toS(tx, tz, sx, sy);
        float dl = std::hypot(sx, sy);
        if (dl < R - 10) {
            circle(cx + sx, cy + sy - 9, 7.5f, Color{255, 255, 255, 255});
            circle(cx + sx, cy + sy - 9, 5.5f, Color{238, 70, 60, 255});
            ui.geo.tri({cx + sx - 5, cy + sy - 6, 0}, {cx + sx + 5, cy + sy - 6, 0}, {cx + sx, cy + sy, 0}, {0, 0, 1}, Color{238, 70, 60, 255});
        } else {
            float ux = sx / dl, uy = sy / dl;
            head(cx + ux * (R - 8), cy + uy * (R - 8), ux, uy, 9, Color{238, 70, 60, 255});
        }
    }
    // Polizei: im Einsatz blinkend blau/rot, auch am Rand als Pfeil, wenn ausserhalb der Karte
    {
        std::vector<std::pair<Vec3, bool>> pol;
        traffic::policeMarks(pol);
        bool flash = std::fmod(simTime, 0.5) < 0.25;
        for (auto& pm : pol) {
            float sx, sy;
            toS(pm.first.x, pm.first.z, sx, sy);
            float dl = std::hypot(sx, sy);
            Color c = pm.second ? (flash ? Color{70, 120, 255, 255} : Color{255, 70, 60, 255}) : Color{150, 170, 210, 255};
            if (dl < R - 8) { circle(cx + sx, cy + sy, pm.second ? 6.5f : 4.5f, Color{255, 255, 255, 255}, 20); circle(cx + sx, cy + sy, pm.second ? 4.8f : 3.2f, c, 20); }
            else if (pm.second) head(cx + sx / dl * (R - 8), cy + sy / dl * (R - 8), sx / dl, sy / dl, 8, c);
        }
    }
    // Netzspieler
    for (const NetCar& c : netplay::cars()) {
        if (!c.placed) continue;
        float sx, sy;
        toS(c.pos.x, c.pos.z, sx, sy);
        if (std::hypot(sx, sy) < R - 6) circle(cx + sx, cy + sy, 4.5f, Color{80, 210, 110, 255}, 16);
    }
    // eigenes Auto (zeigt immer nach oben)
    ui.geo.tri({cx, cy - 11, 0}, {cx + 8, cy + 9, 0}, {cx, cy + 4, 0}, {0, 0, 1}, Color{255, 255, 255, 255});
    ui.geo.tri({cx, cy - 11, 0}, {cx, cy + 4, 0}, {cx - 8, cy + 9, 0}, {0, 0, 1}, Color{95, 170, 255, 255});
    // Rand und Norden
    ring(cx, cy, R + 1, 3.0f, Color{70, 80, 100, 255}, 72);
    float nsx, nsy;
    { float dx = 0, dz = -1; nsx = dx * rxv + dz * rzv; nsy = -(dx * fx + dz * fz); }
    circle(cx + nsx * (R - 10), cy + nsy * (R - 10), 9, Color{200, 55, 50, 255}, 20);
    ui.labelC(cx + nsx * (R - 10), cy + nsy * (R - 10) + 5, 13, col::white, "N");
    ui.labelC(cx, cy + R - 7, 11, col::gray, std::to_string((int)range) + " m");
}

void buildBanner(float W) {
    if (!hasDest) return;
    float cx = W * 0.5f;
    float y = 14;
    if (!hud.title.empty() || !hud.instruction.empty()) y = 14 + 46 + wrapText(hud.instruction, 21, 700).size() * 27.0f + 10;
    if (!routed) {
        std::string s = requesting ? "Route wird berechnet ..." : "Keine Route";
        ui.geo.rrect(cx - 170, y, 340, 38, 12, col::dark);
        ui.labelC(cx, y + 25, 17, col::white, s);
        return;
    }
    float remain = std::max(0.0f, total - along);
    float eta = approx ? remain / 13.0f : (osrmDur > 0 && osrmDist > 0 ? osrmDur * remain / std::max(1.0f, osrmDist) : remain / 13.0f);
    const Step* next = nullptr;
    for (const Step& s : steps)
        if (s.at > along + 4 && s.type != "depart") { next = &s; break; }
    float bw = 400, bh = 86;
    ui.geo.rrect(cx - bw / 2, y, bw, bh, 14, Color{16, 18, 24, 235});
    ui.geo.rrect(cx - bw / 2, y, 6, bh, 3, Color{38, 132, 255, 255});
    if (next) {
        float d = std::max(0.0f, next->at - along);
        ui.geo.rrect(cx - bw / 2 + 16, y + 11, 64, 64, 12, Color{34, 40, 54, 255});
        turnIcon(cx - bw / 2 + 48, y + 43, 22, *next, d < 120 ? col::accent : col::white);
        ui.label(cx - bw / 2 + 96, y + 38, 30, d < 120 ? col::accent : col::white, fmtDist(d));
        std::string txt = instruction(*next);
        while (txt.size() > 4 && font.width(txt, 16) > bw - 110) txt.pop_back();
        ui.label(cx - bw / 2 + 96, y + 62, 16, col::white, txt);
    } else {
        ui.label(cx - bw / 2 + 24, y + 38, 26, col::white, approx ? "Luftlinie zum Ziel" : "Geradeaus weiter");
        ui.label(cx - bw / 2 + 24, y + 62, 16, col::gray, "Folge der Route");
    }
    ui.geo.rrect(cx - 150, y + bh + 6, 300, 28, 10, col::dark);
    ui.labelC(cx, y + bh + 25, 15, col::gray, fmtDist(remain) + "  \xC2\xB7  " + fmtDur(eta) + "  \xC2\xB7  Ziel");
}
} // namespace

void setDestination(double lat, double lon) {
    hasDest = true; dLat = lat; dLon = lon;
    routed = false; approx = false; ll.clear(); steps.clear(); rx.clear(); rz.clear(); cum.clear();
    segHint = 0; along = 0; offTimer = 0;
    requestRoute();
    hud.msgs.push_back({"Navigation gestartet", 3.0f});
}
void clear() {
    hasDest = false; routed = false; requesting = false; approx = false; curReq = ++reqSeq;
    ll.clear(); steps.clear(); rx.clear(); rz.clear(); cum.clear();
}
bool active() { return hasDest; }
bool destination(double& lat, double& lon) { lat = dLat; lon = dLon; return hasDest; }
const std::vector<LL>& routeLatLon() { return ll; }
void zoomCycle() { zoomIdx = (zoomIdx + 1) % 4; }

void update(float dt) {
    std::vector<Pending> got;
    { std::lock_guard<std::mutex> lk(mx); got.swap(pend); }
    for (Pending& p : got) {
        if (p.id != curReq || !hasDest) continue;
        requesting = false;
        if (p.ok) {
            ll = std::move(p.pts); steps = std::move(p.steps);
            osrmDist = p.dist; osrmDur = p.dur;
            routed = true; approx = false;
            builtGen = -1;
            logMsg("Route: %.1f km, %d Hinweise", p.dist / 1000.0f, (int)steps.size());
        } else {
            double lat, lon;
            gamePosition(lat, lon);
            ll = {{lat, lon}, {dLat, dLon}};
            steps.clear();
            routed = true; approx = true; osrmDist = osrmDur = 0;
            builtGen = -1;
            hud.msgs.push_back({"Keine Route gefunden - Luftlinie", 4.0f});
        }
    }
    if (!hasDest || !routed) return;
    if (builtGen != world.geo().gen) rebuildLocal();
    project();
    float remain = total - along;
    if (remain < 22 && (along > total * 0.5f || total < 80)) {
        hud.msgs.push_back({"Sie haben Ihr Ziel erreicht", 6.0f});
        econ::onArrived();
        clear();
        return;
    }
    if (offDist > 40) offTimer += dt; else offTimer = 0;
    if (!requesting && ((offTimer > 3 && simTime - lastReq > 8) || (approx && simTime - lastReq > 20))) {
        if (!approx) hud.msgs.push_back({"Route wird neu berechnet", 3.0f});
        offTimer = 0;
        requestRoute();
    }
}

void buildHUD() {
    float W = ui.W, H = ui.H;
    buildBanner(W);
    if (cfg.minimap) buildMinimap(W, H);
}

void drawWorld() {
    if (!hasDest) return;
    ribbon.clear();
    float tx, tz;
    world.toLocal(dLat, dLon, tx, tz);
    if (routed && rx.size() >= 2 && builtGen == world.geo().gen) {
        int n = (int)rx.size();
        auto height = [&](float x, float z) { float h; return world.height(x, z, h) ? h : car.pos.y - 0.5f; };
        // Punkte alle ~4 m von knapp hinter dem Auto bis 450 m voraus
        std::vector<Vec3> pts;
        float d = std::max(0.0f, along - 6), end = std::min(total, along + 450);
        int i = std::max(0, segHint - 2);
        while (d <= end && i < n - 1) {
            while (i < n - 2 && cum[i + 1] < d) i++;
            float l = cum[i + 1] - cum[i], t = l > 1e-4f ? clampv((d - cum[i]) / l, 0.0f, 1.0f) : 0.0f;
            float x = rx[i] + (rx[i + 1] - rx[i]) * t, z = rz[i] + (rz[i + 1] - rz[i]) * t;
            pts.push_back({x, height(x, z) + 0.14f, z});
            d += 4.0f;
        }
        for (size_t q = 0; q + 1 < pts.size(); q++) {
            Vec3 a = pts[q], b = pts[q + 1];
            Vec3 dir = normalize({b.x - a.x, 0, b.z - a.z}), side{-dir.z, 0, dir.x};
            auto strip = [&](float hw, Color c) {
                ribbon.quad(a - side * hw, a + side * hw, b + side * hw, b - side * hw, {0, 1, 0}, c);
            };
            strip(1.25f, Color{20, 110, 255, 120});
            strip(0.32f, Color{190, 225, 255, 190});
        }
    }
    float h = 0;
    if (!world.height(tx, tz, h)) h = car.pos.y;
    if (std::hypot(tx - car.pos.x, tz - car.pos.z) < 2500) { // Zielsaeule
        Mat4 I = identity();
        ribbon.box(I, {tx, h + 20, tz}, {0.5f, 20, 0.5f}, Color{238, 70, 60, 150});
        ribbon.box(I, {tx, h + 41, tz}, {1.6f, 1.0f, 1.6f}, Color{255, 255, 255, 190});
    }
    if (ribbon.v.empty()) return;
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);
    gfxModel(identity());
    gfxBias(6e-5f);
    gfxMode(0, FLAG_FOG);
    ribbon.draw();
    gfxBias(0);
    glDepthMask(GL_TRUE);
}

} // namespace nav
