// Weltkarte: verschiebbare, zoombare Satellitenkarte mit Ortsnamen. Klick setzt eine Markierung,
// Doppelklick oder Knopf teleportiert das Auto dorthin.
#include "game.h"
#include "net.h"
#include "stb_image.h"
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace {
const double DPI = 3.14159265358979323846;
struct Tile { GLuint tex = 0; bool loaded = false; double used = 0; };
struct Result { uint64_t key; int layer, w, h; std::vector<uint8_t> px; };
std::unordered_map<uint64_t, Tile> tiles[2]; // 0 Satellitenbild, 1 Beschriftung
std::deque<std::pair<int, uint64_t>> jobs;
std::vector<Result> results;
std::mutex mx;
std::condition_variable cv;
bool threadsUp = false, on = false;

double cx = 0.5, cy = 0.35, markX = 0, markY = 0;
float zf = 3;
bool hasMark = false, prevDown = false, panning = false;
float lastMX = 0, lastMY = 0, moved = 0;
double lastClickTime = -10;
Batch quad, bg;
const float TOP = 50;

double lonToMx(double lon) { return (lon + 180.0) / 360.0; }
double latToMy(double lat) { return (1.0 - std::asinh(std::tan(lat * DPI / 180.0)) / DPI) / 2.0; }
double mxToLon(double m) { return m * 360.0 - 180.0; }
double myToLat(double m) { return std::atan(std::sinh(DPI * (1.0 - 2.0 * m))) * 180.0 / DPI; }
uint64_t keyOf(int z, int x, int y) { return ((uint64_t)z << 56) | ((uint64_t)(uint32_t)x << 28) | (uint32_t)y; }

void worker() {
    for (;;) {
        std::pair<int, uint64_t> job;
        {
            std::unique_lock<std::mutex> lk(mx);
            cv.wait(lk, [] { return !jobs.empty(); });
            job = jobs.back(); // neueste Anfrage zuerst: das, was gerade sichtbar ist
            jobs.pop_back();
        }
        int z = (int)(job.second >> 56), x = (int)((job.second >> 28) & 0xFFFFFFF), y = (int)(job.second & 0xFFFFFFF);
        char url[256], cache[128];
        if (job.first == 0) {
            snprintf(url, sizeof url, "https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/%d/%d/%d", z, y, x);
            snprintf(cache, sizeof cache, "cache/img/%d/%d_%d.jpg", z, x, y);
        } else {
            snprintf(url, sizeof url, "https://server.arcgisonline.com/ArcGIS/rest/services/Reference/World_Boundaries_and_Places/MapServer/tile/%d/%d/%d", z, y, x);
            snprintf(cache, sizeof cache, "cache/lbl/%d/%d_%d.png", z, x, y);
        }
        Result r{job.second, job.first, 0, 0, {}};
        std::vector<uint8_t> data;
        if (cachedGet(url, cache, data)) {
            int w, h, c;
            uint8_t* px = stbi_load_from_memory(data.data(), (int)data.size(), &w, &h, &c, 4);
            if (px) { r.px.assign(px, px + (size_t)w * h * 4); r.w = w; r.h = h; stbi_image_free(px); }
        }
        std::lock_guard<std::mutex> lk(mx);
        results.push_back(std::move(r));
    }
}

void request(int layer, int z, int x, int y) {
    uint64_t k = keyOf(z, x, y);
    auto it = tiles[layer].find(k);
    if (it != tiles[layer].end() && (it->second.loaded || simTime - it->second.used < 20)) return; // fehlgeschlagene erst nach 20 s erneut
    tiles[layer][k] = Tile{};
    tiles[layer][k].used = simTime;
    { std::lock_guard<std::mutex> lk(mx); jobs.push_back({layer, k}); if (jobs.size() > 300) jobs.pop_front(); }
    cv.notify_one();
}

// Zeichnet eine Kachel; fehlt sie noch, wird der passende Ausschnitt einer groeberen Kachel verwendet
void drawTile(int layer, int z, int x, int y, float sx, float sy, float size) {
    for (int k = 0; k <= 6 && z - k >= 0; k++) {
        auto it = tiles[layer].find(keyOf(z - k, x >> k, y >> k));
        if (it == tiles[layer].end() || !it->second.loaded) { if (layer == 1) return; continue; }
        it->second.used = simTime;
        float n = (float)(1 << k), u0 = (x - ((x >> k) << k)) / n, v0 = (y - ((y >> k) << k)) / n, du = 1 / n;
        quad.clear();
        Vec3 nn{0, 0, 1};
        Color w{};
        quad.vert({sx, sy, 0}, nn, u0, v0, w); quad.vert({sx + size, sy, 0}, nn, u0 + du, v0, w); quad.vert({sx + size, sy + size, 0}, nn, u0 + du, v0 + du, w);
        quad.vert({sx, sy, 0}, nn, u0, v0, w); quad.vert({sx + size, sy + size, 0}, nn, u0 + du, v0 + du, w); quad.vert({sx, sy + size, 0}, nn, u0, v0 + du, w);
        gfxBindTexture(it->second.tex);
        quad.draw();
        return;
    }
}
void toScreen(double m_x, double m_y, float& sx, float& sy) {
    double S = 256.0 * std::pow(2.0, (double)zf);
    sx = (float)(ui.W * 0.5 + (m_x - cx) * S);
    sy = (float)((ui.H + TOP) * 0.5 + (m_y - cy) * S);
}
void fromScreen(float sx, float sy, double& m_x, double& m_y) {
    double S = 256.0 * std::pow(2.0, (double)zf);
    m_x = cx + (sx - ui.W * 0.5) / S;
    m_y = cy + (sy - (ui.H + TOP) * 0.5) / S;
}
void marker(float x, float y, Color c, const std::string& label) {
    ui.geo.disc(x, y, 9, Color{255, 255, 255, 255});
    ui.geo.disc(x, y, 6.5f, c);
    if (!label.empty()) {
        float w = font.width(label, 14) + 14;
        ui.geo.rrect(x + 12, y - 12, w, 24, 7, Color{16, 18, 24, 215});
        ui.label(x + 19, y + 5, 14, col::white, label);
    }
}
} // namespace

namespace worldmap {

bool active() { return on; }
void close() { on = false; panning = false; }
void setCenter(double lat, double lon, float zoom) { cx = lonToMx(lon); cy = latToMy(lat); zf = zoom; markX = cx; markY = cy; hasMark = true; }
void open() {
    on = true;
    if (!threadsUp) { threadsUp = true; for (int i = 0; i < 6; i++) std::thread(worker).detach(); }
    double lat, lon;
    gamePosition(lat, lon);
    cx = lonToMx(lon); cy = latToMy(lat);
    if (zf < 4) zf = 13;
    hasMark = false;
}

void event(const SDL_Event& e) {
    if (e.type == SDL_EVENT_MOUSE_WHEEL) { // um den Mauszeiger zoomen
        double m_x, m_y;
        fromScreen(ui.mx, ui.my, m_x, m_y);
        zf = clampv(zf + e.wheel.y * 0.5f, 1.5f, 18.0f);
        double S = 256.0 * std::pow(2.0, (double)zf);
        cx = m_x - (ui.mx - ui.W * 0.5) / S;
        cy = m_y - (ui.my - (ui.H + TOP) * 0.5) / S;
    } else if (e.type == SDL_EVENT_KEY_DOWN) {
        if (e.key.key == SDLK_RETURN && hasMark) { gameTeleport(myToLat(markY), mxToLon(markX)); close(); }
        else if (e.key.key == SDLK_PLUS || e.key.key == SDLK_KP_PLUS) zf = std::min(18.0f, zf + 1);
        else if (e.key.key == SDLK_MINUS || e.key.key == SDLK_KP_MINUS) zf = std::max(1.5f, zf - 1);
    }
}

void buildUI() {
    float W = ui.W, H = ui.H;
    char buf[160];
    ui.panel(0, 0, W, TOP, Color{14, 16, 22, 245}, 0);
    ui.label(14, 32, 20, col::accent, "Weltkarte");
    float x = 130;
    if (ui.button(x, 9, 96, 32, "Suchen")) gameSearchOnMap();
    x += 104;
    if (ui.button(x, 9, 210, 32, "Hierhin teleportieren", hasMark)) { if (hasMark) { gameTeleport(myToLat(markY), mxToLon(markX)); close(); return; } }
    x += 218;
    if (ui.button(x, 9, 130, 32, nav::active() ? "Neues Ziel" : "Navigation", hasMark)) { if (hasMark) { nav::setDestination(myToLat(markY), mxToLon(markX - std::floor(markX))); close(); return; } }
    x += 138;
    if (ui.button(x, 9, 150, 32, "Meine Position")) { double lat, lon; gamePosition(lat, lon); cx = lonToMx(lon); cy = latToMy(lat); zf = std::max(zf, 14.0f); }
    x += 158;
    if (ui.button(x, 9, 44, 32, "+")) zf = std::min(18.0f, zf + 1);
    if (ui.button(x + 50, 9, 44, 32, "-")) zf = std::max(1.5f, zf - 1);
    if (ui.button(W - 124, 9, 110, 32, "Schlie\xC3\x9F" "en")) { close(); return; }
    double m_x, m_y;
    fromScreen(ui.mx, ui.my, m_x, m_y);
    snprintf(buf, sizeof buf, "%.4f, %.4f", myToLat(clampv(m_y, 0.0, 1.0)), mxToLon(m_x - std::floor(m_x)));
    ui.labelR(W - 140, 31, 14, col::gray, buf);

    // Ziehen = verschieben, Klick = Markierung, Doppelklick = teleportieren
    bool inMap = ui.my > TOP && !ui.captured;
    if (ui.down && !prevDown && inMap) { panning = true; moved = 0; }
    if (panning && ui.down) {
        double S = 256.0 * std::pow(2.0, (double)zf);
        cx -= (ui.mx - lastMX) / S; cy -= (ui.my - lastMY) / S;
        moved += std::fabs(ui.mx - lastMX) + std::fabs(ui.my - lastMY);
    }
    if (panning && !ui.down) {
        panning = false;
        if (moved < 5) {
            markX = m_x; markY = clampv(m_y, 0.02, 0.98); hasMark = true;
            if (simTime - lastClickTime < 0.4) { gameTeleport(myToLat(markY), mxToLon(markX - std::floor(markX))); close(); return; }
            lastClickTime = simTime;
        }
    }
    prevDown = ui.down; lastMX = ui.mx; lastMY = ui.my;
    cy = clampv(cy, 0.02, 0.98);
    cx -= std::floor(cx);

    float sx, sy;
    double lat, lon;
    gamePosition(lat, lon);
    toScreen(lonToMx(lon), latToMy(lat), sx, sy);
    if (sy > TOP + 8) marker(sx, sy, Color{60, 140, 255, 255}, "Du");
    if (nav::active()) { // Route und Ziel der Navigation
        const auto& rt = nav::routeLatLon();
        for (int pass = 0; pass < 2; pass++)
            for (size_t i = 0; i + 1 < rt.size(); i++) {
                float ax, ay, bx, by;
                toScreen(lonToMx(rt[i].second), latToMy(rt[i].first), ax, ay);
                toScreen(lonToMx(rt[i + 1].second), latToMy(rt[i + 1].first), bx, by);
                if ((ax < -50 && bx < -50) || (ax > ui.W + 50 && bx > ui.W + 50) || (ay < TOP - 50 && by < TOP - 50) || (ay > ui.H + 50 && by > ui.H + 50)) continue;
                float dx = bx - ax, dy = by - ay, l = std::hypot(dx, dy);
                if (l < 0.01f) continue;
                float w = (pass == 0 ? 7.0f : 4.0f) * 0.5f, nx = -dy / l * w, ny = dx / l * w;
                Color c = pass == 0 ? Color{255, 255, 255, 235} : Color{38, 132, 255, 255};
                ui.geo.quad({ax - nx, ay - ny, 0}, {ax + nx, ay + ny, 0}, {bx + nx, by + ny, 0}, {bx - nx, by - ny, 0}, {0, 0, 1}, c);
            }
        double dl, dn;
        nav::destination(dl, dn);
        toScreen(lonToMx(dn), latToMy(dl), sx, sy);
        if (sy > TOP + 8) marker(sx, sy, Color{238, 70, 60, 255}, "Navigationsziel");
    }
    for (const NetCar& c : netplay::cars()) {
        toScreen(lonToMx(c.lon), latToMy(c.lat), sx, sy);
        if (sy > TOP + 8) marker(sx, sy, Color{80, 210, 110, 255}, c.name);
    }
    if (hasMark) {
        toScreen(markX, markY, sx, sy);
        snprintf(buf, sizeof buf, "Ziel  %.4f, %.4f  (Doppelklick = teleportieren)", myToLat(markY), mxToLon(markX - std::floor(markX)));
        if (sy > TOP + 8) marker(sx, sy, Color{240, 70, 60, 255}, buf);
    }
    ui.geo.rrect(10, H - 34, 520, 24, 7, Color{16, 18, 24, 200});
    ui.label(18, H - 17, 12, col::gray, "Ziehen = verschieben  |  Mausrad = Zoom  |  Karte: Esri, Maxar, Earthstar Geographics, HERE, Garmin, \xC2\xA9 OpenStreetMap");
}

void draw(const Mat4& mvp, int ww, int wh) {
    // fertig geladene Kacheln als Texturen anlegen
    std::vector<Result> res;
    { std::lock_guard<std::mutex> lk(mx); res.swap(results); }
    for (Result& r : res) {
        Tile& t = tiles[r.layer][r.key];
        if (r.px.empty()) { t.used = simTime; continue; } // spaeter erneut versuchen
        t.tex = gfxTexture(r.px.data(), r.w, r.h, 4, false);
        t.loaded = true; t.used = simTime;
    }
    for (auto& layer : tiles)
        if (layer.size() > 700)
            for (auto it = layer.begin(); it != layer.end();) {
                if (it->second.loaded && simTime - it->second.used > 20) { glDeleteTextures(1, &it->second.tex); it = layer.erase(it); } else ++it;
            }

    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    gfxModel(identity());
    gfxMVP(mvp);
    gfxBias(0);
    bg.clear();
    bg.rect(0, 0, ui.W, ui.H, Color{10, 14, 22, 255});
    gfxMode(0, 0);
    bg.draw();
    float k = (float)wh / ui.H;
    glEnable(GL_SCISSOR_TEST);
    glScissor(0, 0, ww, (GLsizei)(wh - TOP * k));
    int z = clampv((int)std::floor(zf + 0.5f), 1, 18);
    int n = 1 << z;
    float size = 256.0f * std::pow(2.0f, zf - z);
    double S = 256.0 * std::pow(2.0, (double)zf);
    double left = cx - ui.W * 0.5 / S, top = cy - (ui.H + TOP) * 0.5 / S;
    int x0 = (int)std::floor(left * n), y0 = std::max(0, (int)std::floor(top * n));
    int x1 = (int)std::floor((cx + ui.W * 0.5 / S) * n), y1 = std::min(n - 1, (int)std::floor((cy + (ui.H - TOP) * 0.5 / S) * n));
    gfxMode(8);
    for (int layer = 0; layer < 2; layer++) {
        int lz = layer == 1 ? std::min(z, 16) : z; // Beschriftungen gibt es nur bis Zoom 16
        if (layer == 1 && lz != z) continue;
        for (int y = y0; y <= y1; y++)
            for (int x = x0; x <= x1; x++) {
                int wx = ((x % n) + n) % n;
                request(layer, z, wx, y);
                float sx = (float)(ui.W * 0.5 + ((double)x / n - cx) * S), sy = (float)((ui.H + TOP) * 0.5 + ((double)y / n - cy) * S);
                drawTile(layer, z, wx, y, sx, sy, size);
            }
    }
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
}

} // namespace worldmap
