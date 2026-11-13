// Navigation: Route per OSRM (OpenStreetMap-Routing), Abbiegehinweise, Minimap (Fahrtrichtung oben) und Routenband in der 3D-Welt.
#include "game.h"
#include "net.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

namespace nav {
namespace {
using json = nlohmann::json;
typedef std::pair<double, double> LL;

struct Step { double lat = 0, lon = 0; std::string type, mod, name; int exit = 0; float at = 0, len = 0; };
// air[i] = 1: Segment pts[i] -> pts[i+1] ist Luftlinie (keine Strasse); net = Routendienst war nicht erreichbar (spaeter erneut versuchen)
struct Pending { int id = 0; bool ok = false, net = false; std::vector<LL> pts; std::vector<char> air; std::vector<Step> steps; float dist = 0, dur = 0; };

std::mutex mx;
std::vector<Pending> pend;
std::atomic<int> reqSeq{0};
int curReq = 0;

bool hasDest = false, routed = false, requesting = false, approx = false, netFail = false; // approx: Route enthaelt Luftlinie
double dLat = 0, dLon = 0, lastReq = -100;
std::vector<LL> ll;
std::vector<char> air;          // je Segment: Luftlinie?
std::vector<Step> steps;
std::vector<float> rx, rz, cum, cumAir;   // cumAir: Luftlinien-Laenge bis zum Punkt
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

double hav(const LL& a, const LL& b) { // Entfernung in Metern
    const double R = 6371000.0, d = 3.14159265358979323846 / 180.0;
    double s1 = std::sin((b.first - a.first) * d / 2), s2 = std::sin((b.second - a.second) * d / 2);
    double h = s1 * s1 + std::cos(a.first * d) * std::cos(b.first * d) * s2 * s2;
    return 2 * R * std::asin(std::min(1.0, std::sqrt(h)));
}

// Ein Routenabschnitt per OSRM. kind: 0 gefunden, 1 keine Route moeglich, 2 Dienst nicht erreichbar.
// Bei Serverfehlern (Zeitueberschreitung, 429, 5xx) wird mit dem anderen Server erneut versucht.
struct Piece { int kind = 2; std::vector<LL> pts; std::vector<Step> steps; float dist = 0, dur = 0; };

Piece routePiece(const LL& a, const LL& b, bool limit, int& srv) {
    static const char* BASE[2] = {"https://router.project-osrm.org/route/v1/driving/", "https://routing.openstreetmap.de/routed-car/route/v1/driving/"};
    Piece r;
    for (int attempt = 0; attempt < 4; attempt++) {
        char url[512];
        // limit: Start und Ende muessen innerhalb von 2,5 km an einer Strasse liegen, sonst gilt der Abschnitt als nicht routbar
        snprintf(url, sizeof url, "%s%.6f,%.6f;%.6f,%.6f?overview=full&geometries=geojson&steps=true%s", BASE[srv], a.second, a.first, b.second, b.first,
                 limit ? "&radiuses=2500;2500" : "");
        std::vector<uint8_t> out;
        int st = 0;
        std::string code;
        try {
            if (httpRequest(url, out, st)) {
                json j = json::parse(out.begin(), out.end(), nullptr, false);
                if (j.is_object()) {
                    code = j.value("code", "");
                    if (code == "Ok" && j.contains("routes") && !j["routes"].empty()) {
                        const json& rt = j["routes"][0];
                        r.dist = (float)rt.value("distance", 0.0);
                        r.dur = (float)rt.value("duration", 0.0);
                        for (const json& c : rt["geometry"]["coordinates"]) r.pts.push_back({c[1].get<double>(), c[0].get<double>()});
                        for (const json& leg : rt["legs"])
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
                                r.steps.push_back(q);
                            }
                        r.kind = r.pts.size() >= 2 ? 0 : 1;
                        return r;
                    }
                }
            }
        } catch (...) { code.clear(); r.pts.clear(); r.steps.clear(); }
        if (!code.empty() && code != "Ok" && st != 429 && st < 500) { r.kind = 1; return r; } // NoRoute / NoSegment: der Server hat geantwortet
        srv ^= 1;
        std::this_thread::sleep_for(std::chrono::milliseconds(250 * (attempt + 1)));
    }
    r.kind = 2;
    return r;
}

// Berechnet die Route. Geht keine durchgehende Route, wird abschnittsweise geroutet: wo es Strassen gibt, echte Route,
// dazwischen Luftlinie, am Ende wieder echte Route. Feinere Stuetzpunkte nahe Start und Ziel halten die Luftlinie dort kurz.
Pending computeRoute(int id, double lat, double lon, double tl, double tn) {
    Pending p;
    p.id = id;
    LL start{lat, lon}, dest{tl, tn};
    int srv = 0;
    // Test: WR_NAV_SEGMENTED=1 ueberspringt die Gesamtroute und erzwingt die abschnittsweise Berechnung
    Piece full;
    if (std::getenv("WR_NAV_SEGMENTED")) full.kind = 1; else full = routePiece(start, dest, false, srv);
    if (full.kind == 0) {
        p.ok = true; p.pts = std::move(full.pts); p.steps = std::move(full.steps); p.dist = full.dist; p.dur = full.dur;
        p.air.assign(p.pts.size() - 1, 0);
        return p;
    }
    if (full.kind == 2) { p.net = true; return p; } // Dienst nicht erreichbar: reine Luftlinie, spaeter erneut versuchen
    double D = hav(start, dest);
    if (D < 150) return p;
    std::vector<double> head, tail, mid;
    for (double d : {300.0, 1000.0, 3000.0})
        if (d < D * 0.25) { head.push_back(d); tail.insert(tail.begin(), D - d); }
    double lo = head.empty() ? 0.0 : head.back(), hi = tail.empty() ? D : tail.front(), span = hi - lo;
    if (span > 6000) {
        int cnt = (int)std::ceil(span / std::max(3000.0, span / 10.0));
        for (int k = 1; k < cnt; k++) mid.push_back(lo + span * k / cnt);
    }
    std::vector<double> ds{0.0};
    ds.insert(ds.end(), head.begin(), head.end());
    ds.insert(ds.end(), mid.begin(), mid.end());
    ds.insert(ds.end(), tail.begin(), tail.end());
    ds.push_back(D);
    std::vector<LL> W;
    for (size_t i = 0; i < ds.size(); i++) {
        double f = ds[i] / D;
        W.push_back(i == 0 ? start : (i + 1 == ds.size() ? dest : LL{lat + (tl - lat) * f, lon + (tn - lon) * f}));
    }

    std::vector<LL> pts;
    std::vector<char> air; // air[i] = Segment pts[i] -> pts[i+1] ist Luftlinie
    std::vector<Step> steps;
    bool any = false, lastOk = false;
    auto push = [&](const LL& q, char a) { if (!pts.empty()) air.push_back(a); pts.push_back(q); };
    auto airStep = [&](const char* type, const LL& at) { Step s; s.type = type; s.lat = at.first; s.lon = at.second; steps.push_back(s); };
    for (size_t i = 0; i + 1 < W.size(); i++) {
        if (i) std::this_thread::sleep_for(std::chrono::milliseconds(250)); // oeffentlicher Dienst: nicht haemmern
        Piece r = routePiece(W[i], W[i + 1], true, srv);
        srv ^= 1;
        lastOk = r.kind == 0;
        if (r.kind == 2) p.net = true;
        if (!lastOk) continue;
        bool firstOk = !any;
        any = true;
        p.dist += r.dist; p.dur += r.dur;
        const LL& first = r.pts.front();
        if (pts.empty()) {
            if (hav(start, first) > 40) { airStep("air", start); push(start, 0); push(first, 1); airStep("resume", first); }
            else push(first, 0);
        } else {
            double gap = hav(pts.back(), first);
            if (gap > 40) { airStep("air", pts.back()); push(first, 1); airStep("resume", first); }
            else if (gap >= 0.5) push(first, 0);
        }
        for (size_t j = 1; j < r.pts.size(); j++) push(r.pts[j], 0);
        for (const Step& s : r.steps) { // Zwischenziele der Abschnitte sind keine Abbiegehinweise
            if (s.type == "depart" && !firstOk) continue;
            if (s.type == "arrive" && i + 2 != W.size()) continue;
            steps.push_back(s);
        }
    }
    if (!any) return p; // nirgends eine Strasse in der Naehe: reine Luftlinie
    if (hav(pts.back(), dest) > 40) { airStep("air", pts.back()); push(dest, 1); }
    p.ok = true; p.pts = std::move(pts); p.air = std::move(air); p.steps = std::move(steps);
    (void)lastOk;
    return p;
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
        auto t0 = std::chrono::steady_clock::now();
        try { p = computeRoute(id, lat, lon, tl, tn); } catch (...) { p = Pending{}; p.id = id; p.net = true; }
        logMsg("Routenberechnung: %.1f s", std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
        if (!p.ok) logMsg("Routenberechnung fehlgeschlagen (%s)", p.net ? "Dienst nicht erreichbar" : "keine Route");
        std::lock_guard<std::mutex> lk(mx);
        pend.push_back(std::move(p));
    }).detach();
}

// Route in lokale Koordinaten umrechnen (nach Ortswechsel neu) und Abbiegepunkte auf der Route verorten
void rebuildLocal() {
    int n = (int)ll.size();
    rx.resize(n); rz.resize(n); cum.assign(n, 0); cumAir.assign(n, 0);
    air.resize(n > 0 ? n - 1 : 0, 0);
    for (int i = 0; i < n; i++) {
        world.toLocal(ll[i].first, ll[i].second, rx[i], rz[i]);
        if (i) {
            float sl = (float)hav(ll[i - 1], ll[i]); // echte Meter (die lokale Karte ist weit weg vom Ursprung verzerrt)
            cum[i] = cum[i - 1] + sl;
            cumAir[i] = cumAir[i - 1] + (air[i - 1] ? sl : 0.0f);
        }
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
    Vec3 p = playerPos();
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
    if (s.type == "air") return "Luftlinie folgen (keine Stra\xC3\x9F" "e)";
    if (s.type == "resume") return "Wieder der Stra\xC3\x9F" "e folgen";
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
    Vec3 fw = playerForward();
    float fl = std::hypot(fw.x, fw.z);
    float fx = fl > 1e-4f ? fw.x / fl : 0, fz = fl > 1e-4f ? fw.z / fl : -1;
    float rxv = -fz, rzv = fx;
    auto toS = [&](float wx, float wz, float& sx, float& sy) {
        float dx = wx - playerPos().x, dz = wz - playerPos().z;
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
    world.forBuildingsNear(playerPos().x, playerPos().z, reach, [&](const Building& b) {
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
    world.forSegmentsNear(playerPos().x, playerPos().z, reach, [&](const RoadSeg& s) {
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
        const Color airC{255, 150, 40, 255};
        // Luftlinie: gestrichelt, nur der Teil im Kartenausschnitt (die Segmente koennen hunderte km lang sein)
        auto airLine = [&](float ax, float az, float bx, float bz, int pass) {
            float px = playerPos().x, pz = playerPos().z, dx = bx - ax, dz = bz - az;
            float a2 = dx * dx + dz * dz, b2 = 2 * ((ax - px) * dx + (az - pz) * dz), c2 = (ax - px) * (ax - px) + (az - pz) * (az - pz) - reach * reach;
            float disc = b2 * b2 - 4 * a2 * c2;
            if (a2 < 1e-6f || disc < 0) return;
            float sq = std::sqrt(disc), t0 = std::max(0.0f, (-b2 - sq) / (2 * a2)), t1 = std::min(1.0f, (-b2 + sq) / (2 * a2));
            if (t0 >= t1) return;
            float len = std::sqrt(a2) * (t1 - t0), dash = 26.0f / std::sqrt(a2);
            int cnt = std::min(160, (int)(len / 26.0f));
            for (int k = 0; k <= cnt; k += 2) {
                float s0 = t0 + dash * k, s1 = std::min(t1, s0 + dash);
                if (s0 >= t1) break;
                if (pass == 0) line(ax + dx * s0, az + dz * s0, ax + dx * s1, az + dz * s1, 8.5f, Color{255, 255, 255, 255});
                else line(ax + dx * s0, az + dz * s0, ax + dx * s1, az + dz * s1, 5.5f, airC);
            }
        };
        for (int pass = 0; pass < 2; pass++)
            for (size_t i = 0; i + 1 < rx.size(); i++) {
                if (cum[i + 1] < along - 40) continue;
                if (i < air.size() && air[i]) { airLine(rx[i], rz[i], rx[i + 1], rz[i + 1], pass); continue; }
                if (std::fabs(rx[i] - playerPos().x) > reach * 1.5f && std::fabs(rx[i + 1] - playerPos().x) > reach * 1.5f) continue;
                if (pass == 0) line(rx[i], rz[i], rx[i + 1], rz[i + 1], 8.5f, Color{255, 255, 255, 255});
                else line(rx[i], rz[i], rx[i + 1], rz[i + 1], 5.5f, Color{38, 132, 255, 255});
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
    // Restzeit: Strassenanteil nach der berechneten Fahrzeit, Luftlinie mit 13 m/s (47 km/h)
    int ci = clampv(segHint, 0, std::max(0, (int)cumAir.size() - 1));
    float airRemain = cumAir.empty() ? 0.0f : std::max(0.0f, cumAir.back() - cumAir[ci]);
    float roadRemain = std::max(0.0f, remain - airRemain);
    float eta = airRemain / 13.0f + (osrmDur > 0 && osrmDist > 0 ? osrmDur * roadRemain / std::max(1.0f, osrmDist) : roadRemain / 13.0f);
    bool inAir = segHint >= 0 && segHint < (int)air.size() && air[segHint];
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
        ui.label(cx - bw / 2 + 24, y + 38, 26, col::white, inAir ? "Luftlinie zum Ziel" : "Geradeaus weiter");
        ui.label(cx - bw / 2 + 24, y + 62, 16, col::gray, inAir ? "Keine Stra\xC3\x9F" "e: der gestrichelten Linie folgen" : "Folge der Route");
    }
    ui.geo.rrect(cx - 150, y + bh + 6, 300, 28, 10, col::dark);
    ui.labelC(cx, y + bh + 25, 15, col::gray, fmtDist(remain) + "  \xC2\xB7  " + fmtDur(eta) + "  \xC2\xB7  Ziel");
}
} // namespace

void setDestination(double lat, double lon) {
    hasDest = true; dLat = lat; dLon = lon;
    routed = false; approx = false; netFail = false; ll.clear(); air.clear(); steps.clear(); rx.clear(); rz.clear(); cum.clear(); cumAir.clear();
    segHint = 0; along = 0; offTimer = 0;
    requestRoute();
    hud.msgs.push_back({"Navigation gestartet", 3.0f});
}
void clear() {
    hasDest = false; routed = false; requesting = false; approx = false; netFail = false; curReq = ++reqSeq;
    ll.clear(); air.clear(); steps.clear(); rx.clear(); rz.clear(); cum.clear(); cumAir.clear();
}
void cancel() {
    if (!hasDest) return;
    clear();
    logMsg("Navigation beendet");
    hud.msgs.push_back({"Navigation beendet", 3.0f});
}
bool active() { return hasDest; }
bool destination(double& lat, double& lon) { lat = dLat; lon = dLon; return hasDest; }
const std::vector<LL>& routeLatLon() { return ll; }
const std::vector<char>& routeAir() { return air; }
void zoomCycle() { zoomIdx = (zoomIdx + 1) % 4; }

void update(float dt) {
    std::vector<Pending> got;
    { std::lock_guard<std::mutex> lk(mx); got.swap(pend); }
    for (Pending& p : got) {
        if (p.id != curReq || !hasDest) continue;
        requesting = false;
        netFail = p.net;
        if (p.ok) {
            ll = std::move(p.pts); air = std::move(p.air); steps = std::move(p.steps);
            air.resize(ll.size() - 1, 0);
            osrmDist = p.dist; osrmDur = p.dur;
            routed = true;
            builtGen = -1;
            double airM = 0;
            for (size_t i = 0; i + 1 < ll.size(); i++)
                if (air[i]) {
                    airM += hav(ll[i], ll[i + 1]);
                    logMsg("  Luftlinie %.5f,%.5f -> %.5f,%.5f (%.1f km)", ll[i].first, ll[i].second, ll[i + 1].first, ll[i + 1].second, hav(ll[i], ll[i + 1]) / 1000.0);
                }
            bool wasApprox = approx;
            approx = airM > 0;
            logMsg("Route: %.1f km Strasse, %.1f km Luftlinie, %d Hinweise", p.dist / 1000.0f, airM / 1000.0, (int)steps.size());
            if (approx && !wasApprox) hud.msgs.push_back({"Teils ohne Stra\xC3\x9F" "e: " + fmtDist((float)airM) + " Luftlinie (gestrichelt)", 5.0f});
        } else {
            double lat, lon;
            gamePosition(lat, lon);
            ll = {{lat, lon}, {dLat, dLon}};
            air = {1};
            steps.clear();
            Step s; s.type = "air"; s.lat = lat; s.lon = lon; steps.push_back(s);
            routed = true; approx = true; osrmDist = osrmDur = 0;
            builtGen = -1;
            hud.msgs.push_back({p.net ? "Routendienst nicht erreichbar - Luftlinie" : "Keine Route gefunden - Luftlinie", 4.0f});
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
    // Neu berechnen: nach Verlassen der Route, oder wenn der Routendienst nicht erreichbar war (Luftlinie ist dann nur ein Notbehelf)
    if (!requesting && ((offTimer > 3 && simTime - lastReq > 8) || (netFail && simTime - lastReq > 20))) {
        if (!netFail) hud.msgs.push_back({"Route wird neu berechnet", 3.0f});
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
        auto height = [&](float x, float z) { float h; return world.height(x, z, h) ? h : playerPos().y - 0.5f; };
        // Punkte alle ~4 m von knapp hinter dem Auto bis 450 m voraus
        std::vector<Vec3> pts;
        std::vector<char> pa;   // Punkt liegt auf einem Luftlinien-Segment
        float d = std::max(0.0f, along - 6), end = std::min(total, along + 450);
        int i = std::max(0, segHint - 2);
        while (d <= end && i < n - 1) {
            while (i < n - 2 && cum[i + 1] < d) i++;
            float l = cum[i + 1] - cum[i], t = l > 1e-4f ? clampv((d - cum[i]) / l, 0.0f, 1.0f) : 0.0f;
            float x = rx[i] + (rx[i + 1] - rx[i]) * t, z = rz[i] + (rz[i + 1] - rz[i]) * t;
            pts.push_back({x, height(x, z) + 0.14f, z});
            pa.push_back(i < (int)air.size() && air[i]);
            d += 4.0f;
        }
        for (size_t q = 0; q + 1 < pts.size(); q++) {
            Vec3 a = pts[q], b = pts[q + 1];
            Vec3 dir = normalize({b.x - a.x, 0, b.z - a.z}), side{-dir.z, 0, dir.x};
            auto strip = [&](float hw, Color c) {
                ribbon.quad(a - side * hw, a + side * hw, b + side * hw, b - side * hw, {0, 1, 0}, c);
            };
            if (pa[q] || pa[q + 1]) { // Luftlinie: orange und gestrichelt
                if (q % 2) continue;
                strip(1.25f, Color{255, 140, 30, 130});
                strip(0.32f, Color{255, 225, 170, 200});
                continue;
            }
            strip(1.25f, Color{20, 110, 255, 120});
            strip(0.32f, Color{190, 225, 255, 190});
        }
    }
    float h = 0;
    if (!world.height(tx, tz, h)) h = playerPos().y;
    if (std::hypot(tx - playerPos().x, tz - playerPos().z) < 2500) { // Zielsaeule
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
