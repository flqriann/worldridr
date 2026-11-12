// worldridr - Fahrschul-Simulator auf echten Karten
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <lua.hpp>
#include <nlohmann/json.hpp>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <thread>
#include <unordered_map>
#include "audio.h"
#include "game.h"
#include "net.h"
#include "stb_image_write.h"

Config cfg;
World world;
Vehicle car;
Input input;
Script script;
HudState hud;
VR vr;
Font font;
double simTime = 0;

static Batch propsLit, propsUnlit, stopLines, mirrorQuad[3];
static Target mirrorRT[3];
static Mesh skyMesh;

static bool spawned = false, belt = false, lights = false, debugOverlay = false, autodrive = false;
static int blinker = 0, camMode = 0; // blinker: -1 links, 1 rechts, 2 Warnblinker
static bool blinkArmed = false, blinkLit = false;
static int typing = 0;               // 0 nichts, 1 Ort, 2 Server-Adresse
static std::string typed, statusLine;
static float lookYaw = 0, lookPitch = 0, crashCooldown = 0;
static RoadHit roadHit;
static bool onRoad = true, wrongWay = false, running = true;
static float laneOffset = 0;
static bool menuOpen = false;
static int menuPage = 0;
static Mat4 mainVP = identity();
static SDL_Window* win = nullptr;

static std::mutex tpMutex;
static std::atomic<int> tpState{0}; // 0 nichts, 1 sucht, 2 gefunden, 3 Fehler
static double tpLat = 0, tpLon = 0;

// ---------- Konfiguration (config.lua) ----------
static void loadConfig() {
    lua_State* L = luaL_newstate();
    luaL_openlibs(L);
    if (luaL_dofile(L, "config.lua")) { logMsg("config.lua: %s", lua_tostring(L, -1)); lua_close(L); return; }
    auto num = [&](const char* k, double def) { lua_getglobal(L, k); double v = lua_isnumber(L, -1) ? lua_tonumber(L, -1) : def; lua_pop(L, 1); return v; };
    auto boolean = [&](const char* k, bool def) { lua_getglobal(L, k); bool v = lua_isboolean(L, -1) ? lua_toboolean(L, -1) != 0 : def; lua_pop(L, 1); return v; };
    auto str = [&](const char* k) { lua_getglobal(L, k); std::string v = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : ""; lua_pop(L, 1); return v; };
    lua_getglobal(L, "start");
    if (lua_istable(L, -1)) {
        lua_getfield(L, -1, "lat"); if (lua_isnumber(L, -1)) cfg.lat = lua_tonumber(L, -1); lua_pop(L, 1);
        lua_getfield(L, -1, "lon"); if (lua_isnumber(L, -1)) cfg.lon = lua_tonumber(L, -1); lua_pop(L, 1);
    } else if (lua_type(L, -1) == LUA_TSTRING) cfg.startPlace = lua_tostring(L, -1);
    lua_pop(L, 1);
    cfg.googleKey = str("google_maps_api_key");
    cfg.automatic = str("transmission") == "automatic";
    if (!str("player_name").empty()) cfg.playerName = str("player_name");
    lua_getglobal(L, "auto_clutch");
    if (lua_isboolean(L, -1)) cfg.autoClutch = lua_toboolean(L, -1) ? 1 : 0;
    lua_pop(L, 1);
    cfg.vr = boolean("vr", cfg.vr);
    cfg.fullscreen = boolean("fullscreen", cfg.fullscreen);
    cfg.mirrors = boolean("mirrors", cfg.mirrors);
    cfg.width = (int)num("width", cfg.width);
    cfg.height = (int)num("height", cfg.height);
    cfg.nearZoom = (int)num("near_zoom", cfg.nearZoom);
    cfg.port = (int)num("port", cfg.port);
    cfg.traffic = (int)num("traffic", cfg.traffic);
    cfg.fov = (float)num("fov", cfg.fov);
    cfg.volume = (float)num("volume", cfg.volume);
    lua_getglobal(L, "wheel");
    if (lua_istable(L, -1)) {
        WheelCfg& w = cfg.wheel;
        auto fi = [&](const char* k, int& v) { lua_getfield(L, -1, k); if (lua_isnumber(L, -1)) v = (int)lua_tointeger(L, -1); lua_pop(L, 1); };
        auto fb = [&](const char* k, bool& v) { lua_getfield(L, -1, k); if (lua_isboolean(L, -1)) v = lua_toboolean(L, -1) != 0; lua_pop(L, 1); };
        fi("steer_axis", w.steerAxis); fi("throttle_axis", w.throttleAxis); fi("brake_axis", w.brakeAxis); fi("clutch_axis", w.clutchAxis);
        fb("invert_pedals", w.invertPedals); fb("ffb_invert", w.ffbInvert); fb("h_shifter", w.hShifter);
        lua_getfield(L, -1, "ffb_strength"); if (lua_isnumber(L, -1)) w.ffbStrength = (float)lua_tonumber(L, -1); lua_pop(L, 1);
        fi("shift_up", w.bShiftUp); fi("shift_down", w.bShiftDown); fi("blinker_left", w.bBlinkL); fi("blinker_right", w.bBlinkR);
        fi("handbrake", w.bHandbrake); fi("ignition", w.bIgnition); fi("seatbelt", w.bBelt); fi("horn", w.bHorn);
        fi("lights", w.bLights); fi("camera", w.bCamera); fi("reset", w.bReset); fi("reverse_button", w.reverseButton);
        lua_getfield(L, -1, "gear_buttons");
        if (lua_istable(L, -1))
            for (int i = 0; i < 6; i++) { lua_rawgeti(L, -1, i + 1); if (lua_isnumber(L, -1)) w.gearButtons[i] = (int)lua_tointeger(L, -1); lua_pop(L, 1); }
        lua_pop(L, 1);
    }
    lua_close(L);
}

static bool geocode(const std::string& q, double& lat, double& lon) {
    std::vector<uint8_t> out;
    int st = 0;
    if (!httpRequest("https://nominatim.openstreetmap.org/search?format=json&limit=1&q=" + urlEncode(q), out, st) || st != 200) return false;
    auto j = nlohmann::json::parse(out.begin(), out.end(), nullptr, false);
    if (!j.is_array() || j.empty()) return false;
    try {
        lat = std::stod(j[0].value("lat", "0"));
        lon = std::stod(j[0].value("lon", "0"));
    } catch (...) { return false; }
    return true;
}

static void startTeleport(const std::string& place, bool onlyShowOnMap = false) {
    if (tpState == 1) return;
    tpState = 1;
    statusLine = "Suche \"" + place + "\" ...";
    std::thread([place, onlyShowOnMap] {
        double lat, lon;
        if (geocode(place, lat, lon)) {
            std::lock_guard<std::mutex> lk(tpMutex);
            tpLat = lat; tpLon = lon;
            tpState = onlyShowOnMap ? 4 : 2;
        } else tpState = 3;
    }).detach();
}
void gameTeleport(double lat, double lon) {
    std::lock_guard<std::mutex> lk(tpMutex);
    tpLat = lat; tpLon = lon;
    tpState = 2;
}
void gamePosition(double& lat, double& lon) { world.toLatLon(car.pos.x, car.pos.z, lat, lon); }

static void spawnCar(bool nearCurrent) {
    Vec3 p = nearCurrent ? car.pos : Vec3{};
    RoadHit h = world.nearestRoad(p.x, p.z, 600);
    float x = p.x, z = p.z, yaw = 0;
    if (h.found) {
        float dx = h.dx, dz = h.dz;
        if (h.seg->oneway == -1) { dx = -dx; dz = -dz; }
        float off = h.seg->oneway ? 0.0f : h.seg->hw * 0.5f;
        x = h.px - dz * off; z = h.pz + dx * off;
        yaw = std::atan2(-dx, -dz);
    }
    float y = 0;
    world.surface(x, z, 1e9f, y);
    car.place({x, y + 0.62f, z}, yaw);
}

static Mat4 rotY(float a) { return fromQuatPos(qaxis({0, 1, 0}, a), {}); }
static Mat4 rotX(float a) { return fromQuatPos(qaxis({1, 0, 0}, a), {}); }

static CarVisual ownVisual() {
    CarVisual v;
    v.M = car.transform();
    v.steer = car.w[0].steer;
    for (int i = 0; i < 4; i++) { v.wheelRot[i] = car.w[i].rot; v.comp[i] = car.w[i].comp; }
    v.brake = car.brake > 0.05f;
    v.blinkL = (blinker == -1 || blinker == 2) && blinkLit;
    v.blinkR = (blinker == 1 || blinker == 2) && blinkLit;
    v.lights = lights;
    v.reverse = car.gear == -1;
    return v;
}
static DashState ownDash() {
    DashState d;
    d.kmh = car.speed() * 3.6f; d.rpm = car.rpm(); d.steerInput = car.steer; d.odoKm = car.odo / 1000.0f;
    d.gear = car.gear; d.selector = car.selector; d.automatic = car.automatic;
    d.limit = roadHit.found && onRoad ? (int)roadHit.seg->maxspeed : 0;
    d.blinkL = (blinker == -1 || blinker == 2) && blinkLit; d.blinkR = (blinker == 1 || blinker == 2) && blinkLit;
    d.handbrake = car.handbrake; d.belt = belt; d.engineOn = car.engineOn; d.lights = lights;
    d.road = roadHit.found && onRoad ? roadHit.name : "";
    return d;
}
static Color paintOf(uint32_t id) {
    static const Color pal[8] = {{40, 90, 190, 255}, {230, 230, 235, 255}, {30, 32, 36, 255}, {230, 170, 30, 255}, {40, 140, 80, 255}, {130, 136, 146, 255}, {120, 50, 150, 255}, {220, 100, 30, 255}};
    return pal[(id >> 3) % 8];
}

// ---------- Ampeln und Schilder ----------
static void buildProps(Vec3 cam) {
    propsLit.clear(); propsUnlit.clear(); stopLines.clear();
    world.forControls(cam, 280, [&](const Control& c) {
        Vec3 dir{c.dx, 0, c.dz}, right{-c.dz, 0, c.dx};
        Vec3 pp = c.pos + right * (c.hw + 0.9f);
        Mat4 B = identity();
        B.m[0] = right.x; B.m[2] = right.z; B.m[8] = -dir.x; B.m[10] = -dir.z;
        B.m[12] = pp.x; B.m[13] = pp.y; B.m[14] = pp.z;
        Color gray{120, 124, 128, 255}, dark{26, 28, 30, 255};
        if (c.type == 0) {
            propsLit.box(B, {0, 1.7f, 0}, {0.06f, 1.7f, 0.06f}, gray);
            propsLit.box(B, {0, 3.0f, 0}, {0.17f, 0.52f, 0.12f}, dark);
            int s = World::signalState(c, simTime);
            bool red = s == 0 || s == 1, yel = s == 1 || s == 3, grn = s == 2;
            propsUnlit.box(B, {0, 3.33f, 0.125f}, {0.10f, 0.10f, 0.02f}, red ? Color{255, 40, 30, 255} : Color{70, 14, 12, 255});
            propsUnlit.box(B, {0, 3.0f, 0.125f}, {0.10f, 0.10f, 0.02f}, yel ? Color{255, 200, 30, 255} : Color{70, 55, 10, 255});
            propsUnlit.box(B, {0, 2.67f, 0.125f}, {0.10f, 0.10f, 0.02f}, grn ? Color{50, 255, 90, 255} : Color{12, 60, 22, 255});
        } else {
            propsLit.box(B, {0, 1.1f, 0}, {0.04f, 1.1f, 0.04f}, gray);
            propsUnlit.box(B, {0, 2.3f, 0.02f}, {0.34f, 0.34f, 0.02f}, Color{205, 22, 28, 255});
            if (c.type == 1) propsUnlit.box(B, {0, 2.3f, 0.05f}, {0.25f, 0.07f, 0.01f}, Color{245, 245, 245, 255});
            else propsUnlit.box(B, {0, 2.3f, 0.05f}, {0.22f, 0.22f, 0.01f}, Color{245, 245, 245, 255});
        }
        if (c.type <= 1) {
            Vec3 a = c.pos + right * (c.oneway ? -c.hw + 0.3f : 0.15f), b = c.pos + right * (c.hw - 0.3f);
            Vec3 up{0, 0.06f, 0}, back = dir * -0.45f;
            stopLines.quad(a + up, b + up, b + back + up, a + back + up, {0, 1, 0}, Color{225, 225, 225, 255});
        }
    });
}

// ---------- Szene ----------
static void drawScene(const Mat4& view, const Mat4& proj, Vec3 cam, bool mirrorPass = false) {
    Mat4 vp = mul(proj, view);
    gfxCamera(cam, invert(vp));
    gfxModel(identity());
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_BLEND);
    gfxBias(0);
    gfxMVP(identity());
    gfxMode(6);
    skyMesh.draw();
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_TRUE);
    gfxMVP(vp);
    gfxMode(1);
    for (int level = 0; level < 3; level++) { // fern -> nah; jede Stufe ueberdeckt die groebere
        world.drawTerrain(level);
        if (level < 2) glClear(GL_DEPTH_BUFFER_BIT);
    }
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);
    gfxBias(3e-5f);
    gfxMode(2);
    world.drawRoads();
    gfxMode(0, FLAG_FOG);
    stopLines.draw();
    glDepthMask(GL_TRUE);
    gfxBias(1e-5f);
    gfxMode(2);
    world.drawCustomRoads(); // selbst gebaute Strassen liegen ueber dem Gelaende und schreiben Tiefe
    glDisable(GL_BLEND);
    gfxBias(0);
    gfxMode(3);
    world.drawBuildings();
    editor::drawWorld();
    gfxMode(0, FLAG_LIT | FLAG_FOG);
    propsLit.draw();
    gfxMode(0, FLAG_FOG);
    propsUnlit.draw();
    for (const NetCar& c : netplay::cars()) {
        if (!c.placed) continue;
        CarVisual v;
        v.M = fromQuatPos(c.qs, c.pos);
        v.steer = c.steer;
        for (float& r : v.wheelRot) r = c.wheelRot;
        v.paint = paintOf(c.id);
        v.blinkL = (c.flags & 1) != 0; v.blinkR = (c.flags & 2) != 0; v.brake = (c.flags & 4) != 0; v.lights = (c.flags & 8) != 0;
        carDraw(v, vp, false, nullptr);
    }
    traffic::draw(vp, cam, mirrorPass ? 160.0f : 700.0f);
    gfxMVP(vp);
}

static void drawOwnCar(const Mat4& vp, bool cockpit) {
    CarVisual v = ownVisual();
    DashState d = ownDash();
    carDraw(v, vp, cockpit, &d);
    if (cockpit && cfg.mirrors) {
        gfxModel(v.M);
        gfxMode(4);
        gfxTint(0.9f, 0.92f, 0.95f);
        for (int i = 0; i < 3; i++) { gfxBindTexture(mirrorRT[i].tex); mirrorQuad[i].draw(); }
        gfxModel(identity());
    }
}

static void renderMirrors(const Mat4& M) {
    static const float yaw[3] = {PI - 0.17f, PI, PI + 0.17f}, fov[3] = {0.42f, 0.25f, 0.42f};
    for (int i = 0; i < 3; i++) {
        Mat4 camM = mul(M, mul(translate(MIRROR_POS[i]), rotY(yaw[i])));
        Mat4 view = rigidInverse(camM);
        Mat4 proj = mul(scaleM(-1, 1, 1), perspective(fov[i], MIRROR_HW[i] / MIRROR_HH[i], 0.5f, 40000.0f));
        glBindFramebuffer(GL_FRAMEBUFFER, mirrorRT[i].fbo);
        glViewport(0, 0, mirrorRT[i].w, mirrorRT[i].h);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        drawScene(view, proj, xformPoint(camM, {0, 0, 0}), true);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

// ---------- HUD ----------
static void buildHUD(bool cockpit, bool loading) {
    float W = ui.W, H = ui.H, cx = W * 0.5f;
    char buf[256];
    if (!cockpit && spawned) { // Aussenkamera: kompakte Anzeige (im Cockpit zeigt das Armaturenbrett alles)
        float by = H - 18;
        ui.geo.rrect(cx - 170, by - 96, 340, 96, 14, col::dark);
        snprintf(buf, sizeof buf, "%d", (int)std::lround(std::fabs(car.speed() * 3.6f)));
        ui.labelC(cx - 40, by - 30, 56, col::white, buf);
        ui.label(cx + 12, by - 30, 16, col::gray, "km/h");
        std::string g = car.automatic ? std::string(1, "PRND"[car.selector]) : (car.gear == -1 ? "R" : car.gear == 0 ? "N" : std::to_string(car.gear));
        ui.geo.rrect(cx + 86, by - 78, 54, 54, 9, Color{36, 40, 50, 255});
        ui.labelC(cx + 113, by - 38, 38, col::accent, g);
        float rf = clampv(car.rpm() / 7000.0f, 0.0f, 1.0f);
        ui.geo.rrect(cx - 150, by - 16, 300, 6, 3, Color{44, 48, 58, 255});
        ui.geo.rrect(cx - 150, by - 16, std::max(6.0f, 300 * rf), 6, 3, rf > 0.86f ? col::red : col::blue);
        for (float s : {-1.0f, 1.0f}) {
            bool on = (blinker == (int)s || blinker == 2) && blinkLit;
            float x = cx + s * 148;
            ui.geo.tri({x + s * 12, by - 54, 0}, {x - s * 6, by - 66, 0}, {x - s * 6, by - 42, 0}, {0, 0, 1}, on ? col::green : Color{40, 60, 46, 255});
        }
        std::string warn = !car.engineOn ? "MOTOR AUS" : car.handbrake ? "HANDBREMSE" : !belt ? "GURT" : "";
        if (!warn.empty()) ui.labelC(cx, by - 104, 15, col::red, warn);
    }
    if (roadHit.found && onRoad && spawned && !cockpit) { // im Cockpit steht das Limit im Mitteldisplay
        ui.geo.disc(W - 70, 70, 44, Color{215, 30, 35, 255});
        ui.geo.disc(W - 70, 70, 34, col::white);
        snprintf(buf, sizeof buf, "%d", (int)roadHit.seg->maxspeed);
        ui.labelC(W - 70, 82, 34, Color{15, 15, 15, 255}, buf);
        if (!roadHit.name.empty()) ui.labelR(W - 22, 140, 17, col::white, roadHit.name);
    }
    if (!hud.title.empty() || !hud.instruction.empty()) {
        auto lines = wrapText(hud.instruction, 21, 700);
        float h = 46 + lines.size() * 27.0f;
        ui.geo.rrect(cx - 380, 14, 760, h, 14, col::dark);
        ui.geo.rrect(cx - 380, 14, 6, h, 3, col::accent);
        ui.labelC(cx, 40, 18, col::accent, hud.title);
        for (size_t i = 0; i < lines.size(); i++) ui.labelC(cx, 70 + i * 27.0f, 21, col::white, lines[i]);
    }
    float my = H * 0.36f;
    for (auto& m : hud.msgs) {
        float w = font.width(m.text, 24) + 40;
        uint8_t a = (uint8_t)(255 * clampv(m.ttl, 0.0f, 1.0f));
        ui.geo.rrect(cx - w / 2, my - 27, w, 40, 12, Color{16, 18, 24, (uint8_t)(a * 0.8f)});
        ui.labelC(cx, my, 24, Color{255, 228, 140, a}, m.text);
        my += 46;
    }
    float ly = 14;
    if (!hud.faults.empty() || hud.points > 0) {
        float h = 40 + hud.faults.size() * 21.0f;
        ui.geo.rrect(14, ly, 380, h, 12, col::dark);
        snprintf(buf, sizeof buf, "Fehlerpunkte: %d", hud.points);
        ui.label(28, ly + 27, 19, hud.points > 0 ? col::red : col::white, buf);
        for (size_t i = 0; i < hud.faults.size(); i++) ui.label(28, ly + 50 + i * 21.0f, 15, col::gray, hud.faults[i]);
        ly += h + 8;
    }
    if (netplay::active()) {
        std::string s = "Mehrspieler: " + netplay::status();
        ui.geo.rrect(14, ly, font.width(s, 15) + 28, 30, 10, col::dark);
        ui.label(28, ly + 21, 15, col::blue, s);
        for (const NetCar& c : netplay::cars()) { // Namensschilder ueber den Mitspielern
            const float* m = mainVP.m;
            Vec3 p = c.pos + Vec3{0, 1.5f, 0};
            float w = m[3] * p.x + m[7] * p.y + m[11] * p.z + m[15];
            if (!c.placed || w < 1 || w > 400) continue;
            float sx = ((m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12]) / w * 0.5f + 0.5f) * W, sy = (1 - ((m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13]) / w * 0.5f + 0.5f)) * H;
            float tw = font.width(c.name, 15) + 16;
            ui.geo.rrect(sx - tw / 2, sy - 20, tw, 24, 8, Color{16, 18, 24, 190});
            ui.labelC(sx, sy - 3, 15, col::white, c.name);
        }
    }
    ui.label(14, H - 10, 12, Color{220, 220, 220, 190}, world.attribution());
    if (loading) {
        ui.geo.rrect(cx - 250, H * 0.5f - 46, 500, 80, 14, Color{16, 18, 24, 235});
        ui.labelC(cx, H * 0.5f - 10, 26, col::white, "Lade Welt ...");
        snprintf(buf, sizeof buf, "%d Kacheln unterwegs (Satellitenbilder, H\xC3\xB6hen, Stra\xC3\x9F" "en)", world.pendingJobs());
        ui.labelC(cx, H * 0.5f + 16, 15, col::gray, buf);
    }
    if (debugOverlay) {
        ui.geo.rrect(14, H - 240, 580, 204, 12, col::dark);
        float y = H - 216;
        ui.label(26, y, 15, col::white, input.wheelPresent ? "Lenkrad: " + input.wheelName + (input.ffbOk ? "  [FFB an]" : "  [kein FFB]") : "Kein Lenkrad erkannt (Tastatur)"); y += 20;
        std::string a = "Achsen:";
        for (int i = 0; i < input.naxes; i++) { snprintf(buf, sizeof buf, "  %d: %+.2f", i, input.axes[i]); a += buf; }
        ui.label(26, y, 15, col::gray, a); y += 20;
        std::string b = "Tasten:";
        for (int i = 0; i < 32; i++) if (input.buttons & (1u << i)) b += " " + std::to_string(i);
        ui.label(26, y, 15, col::gray, b); y += 20;
        snprintf(buf, sizeof buf, "Lenkung %+.2f  Gas %.2f  Bremse %.2f  Kupplung %.2f  FFB %+.2f Nm", input.steer, input.throttle, input.brake, input.clutch, car.ffb);
        ui.label(26, y, 15, col::gray, buf); y += 20;
        double lat, lon;
        world.toLatLon(car.pos.x, car.pos.z, lat, lon);
        snprintf(buf, sizeof buf, "Position %.5f, %.5f  H\xC3\xB6he %.0f m  Spur %+.1f m  %s", lat, lon, car.pos.y, laneOffset, onRoad ? "Stra\xC3\x9F" "e" : "Gel\xC3\xA4nde");
        ui.label(26, y, 15, col::gray, buf); y += 20;
        snprintf(buf, sizeof buf, "Schlupf VL %.2f VR %.2f HL %.2f HR %.2f   VR: %s", car.w[0].slip, car.w[1].slip, car.w[2].slip, car.w[3].slip,
                 vr.running ? "aktiv" : (vr.available ? "bereit (Headset aufsetzen)" : "aus"));
        ui.label(26, y, 15, col::gray, buf);
    }
}

// ---------- H-Schalthebel einrichten: jede Stellung einmal einlegen, die gemeldete Taste wird gemerkt ----------
static int calStep = -1, calCand = -1, calMap[7];
static float calHold = 0;
static uint32_t calIgnore = 0;
static bool calWaitRelease = false;
static const char* SHIFTER_FILE = "shifter.cfg";

static void loadShifter() {
    FILE* f = fopen(SHIFTER_FILE, "r");
    if (!f) return;
    int m[7];
    if (fscanf(f, "%d %d %d %d %d %d %d", &m[0], &m[1], &m[2], &m[3], &m[4], &m[5], &m[6]) == 7) {
        input.setShifter(m, m[6]);
        logMsg("Schalthebel: Tasten %d %d %d %d %d %d, R %d", m[0], m[1], m[2], m[3], m[4], m[5], m[6]);
    }
    fclose(f);
}
static void startShifterSetup() {
    if (!input.wheelPresent) { hud.msgs.push_back({"Kein Lenkrad erkannt - Schalthebel kann nicht eingerichtet werden", 5.0f}); return; }
    calStep = 0; calCand = -1; calHold = 0; calWaitRelease = false;
    calIgnore = input.buttons;
}
static void updateShifterSetup(float dt) {
    if (calStep < 0) return;
    static const char* names[7] = {"den 1. Gang", "den 2. Gang", "den 3. Gang", "den 4. Gang", "den 5. Gang", "den 6. Gang", "den R\xC3\xBC" "ckw\xC3\xA4rtsgang"};
    if (calWaitRelease) {
        statusLine = "Erkannt (Taste " + std::to_string(calMap[calStep - 1]) + "). Jetzt den Hebel zur\xC3\xBC" "ck in den Leerlauf.";
        if (input.buttons & (1u << calMap[calStep - 1])) return;
        calWaitRelease = false;
        if (calStep == 7) {
            FILE* f = fopen(SHIFTER_FILE, "w");
            if (f) { fprintf(f, "%d %d %d %d %d %d %d\n", calMap[0], calMap[1], calMap[2], calMap[3], calMap[4], calMap[5], calMap[6]); fclose(f); }
            input.setShifter(calMap, calMap[6]);
            calStep = -1;
            statusLine.clear();
            hud.msgs.push_back({"Schalthebel eingerichtet", 4.0f});
            return;
        }
    }
    statusLine = "Schalthebel einrichten (" + std::to_string(calStep + 1) + "/7): Lege " + names[calStep] + " ein und halte ihn kurz.  Esc = abbrechen";
    int cand = -1;
    uint32_t b = input.buttons & ~calIgnore;
    for (int i = 0; i < calStep; i++) b &= ~(1u << calMap[i]);
    for (int i = 0; i < 32 && cand < 0; i++) if (b & (1u << i)) cand = i;
    if (cand >= 0 && cand == calCand) calHold += dt; else { calCand = cand; calHold = 0; }
    if (cand >= 0 && calHold > 0.45f) { calMap[calStep++] = cand; calWaitRelease = true; calHold = 0; calCand = -1; }
}

static void buildPrompt() {
    float cx = ui.W * 0.5f, H = ui.H;
    if (!typing && statusLine.empty()) return;
    std::string s = typing == 1 ? "Ort eingeben (Enter = hinfahren, Esc = abbrechen): " + typed + "_"
                  : typing == 2 ? "Adresse des Hosts (z. B. 192.168.0.12): " + typed + "_"
                  : typing == 3 ? "Ort auf der Karte suchen: " + typed + "_" : statusLine;
    float w = font.width(s, 22) + 44;
    ui.geo.rrect(cx - w / 2, H * 0.66f - 30, w, 46, 12, Color{16, 18, 24, 240});
    ui.labelC(cx, H * 0.66f, 22, col::white, s);
}

static void beginTyping(int mode) { typing = mode; typed.clear(); statusLine.clear(); SDL_StartTextInput(win); }

void gameSearchOnMap() { beginTyping(3); }

static void buildMenu() {
    float W = ui.W, H = ui.H, w = 400, x = W * 0.5f - w * 0.5f;
    ui.geo.rect(0, 0, W, H, Color{6, 8, 12, 120});
    int rows = menuPage == 0 ? 11 : 4;
    float h = 118 + rows * 48.0f, y = H * 0.5f - h * 0.5f;
    ui.panel(x, y, w, h, Color{14, 16, 22, 240}, 18);
    ui.geo.rrect(x, y, w, 6, 3, col::accent);
    ui.labelC(W * 0.5f, y + 54, 38, col::white, "worldridr");
    ui.labelC(W * 0.5f, y + 80, 14, col::gray, menuPage == 0 ? "Fahrschule auf der ganzen Welt" : netplay::status());
    float by = y + 104;
    auto item = [&](const std::string& label, bool active = false) { bool hit = ui.button(x + 24, by, w - 48, 40, label, active, 18); by += 48; return hit; };
    if (menuPage == 0) {
        if (item(spawned ? "Weiterfahren" : "Fahren")) menuOpen = false;
        if (item("Fahrstunden")) { menuOpen = false; if (editor::active()) editor::close(); script.event("key", "f1"); }
        if (item("Weltkarte: \xC3\xBC" "berall hin teleportieren")) { menuOpen = false; if (editor::active()) editor::close(); worldmap::open(); }
        if (item("Ort per Namen suchen")) { menuOpen = false; beginTyping(1); }
        if (item(traffic::wanted > 0 ? "Verkehr: an" : "Verkehr: aus")) traffic::wanted = traffic::wanted > 0 ? 0 : std::max(1, cfg.traffic);
        if (item(editor::active() ? "Studio verlassen" : "Studio: bauen und Stra\xC3\x9F" "en legen")) {
            menuOpen = false;
            if (editor::active()) editor::close();
            else { Vec3 f = car.forward(); editor::open(car.pos, std::atan2(-f.x, -f.z)); }
        }
        if (item("Mehrspieler")) menuPage = 1;
        if (item(cfg.automatic ? "Getriebe: Automatik" : "Getriebe: Schaltung")) cfg.automatic = !cfg.automatic;
        if (item(camMode == 0 ? "Kamera: Cockpit" : "Kamera: au\xC3\x9F" "en")) camMode = (camMode + 1) % 2;
        if (item(input.shifterReady() ? "Schalthebel neu einrichten" : "Schalthebel (H-Schaltung) einrichten")) { menuOpen = false; startShifterSetup(); }
        if (item("Beenden")) running = false;
    } else {
        if (item("Spiel hosten", netplay::hosting())) netplay::host(cfg.port);
        if (item("Beitreten (Adresse eingeben)")) { menuOpen = false; beginTyping(2); }
        if (item("Trennen")) netplay::stop();
        if (item("Zur\xC3\xBC" "ck")) menuPage = 0;
    }
}

// ---------- Ampeln / Stoppschilder ueberwachen ----------
struct CtlTrack { float prevD = -100; bool stopped = false; double seen = -10; };
static std::unordered_map<uint64_t, CtlTrack> tracks;

static void checkControls() {
    if (tracks.size() > 400) tracks.clear();
    Vec3 f = car.forward();
    world.forControls(car.pos, 45, [&](const Control& c) {
        float rx = car.pos.x - c.pos.x, rz = car.pos.z - c.pos.z;
        float dd = rx * c.dx + rz * c.dz + 1.9f; // Fahrzeugfront relativ zur Haltelinie
        float lat = rx * (-c.dz) + rz * c.dx;
        float align = f.x * c.dx + f.z * c.dz;
        uint64_t key = ((uint64_t)(uint32_t)(int)(c.pos.x * 4) << 34) ^ ((uint64_t)(uint32_t)(int)(c.pos.z * 4) << 4) ^ (uint64_t)(c.type * 4 + (c.dx > 0 ? 2 : 0) + (c.dz > 0 ? 1 : 0));
        CtlTrack& t = tracks[key];
        if (t.seen < simTime - 1.0) { t.prevD = dd; t.stopped = false; }
        t.seen = simTime;
        if (dd > -16 && dd < 1.5f && std::fabs(car.speed()) < 0.4f) t.stopped = true;
        if (t.prevD < 0 && dd >= 0 && lat > -1.0f && lat < c.hw + 1.0f && align > 0.5f && car.speed() > 0.5f) {
            if (c.type == 0) { int s = World::signalState(c, simTime); if (s == 0 || s == 1) script.event("red_light"); }
            else if (c.type == 1 && !t.stopped) script.event("stop_sign");
        }
        t.prevD = dd;
    });
}

static void saveScreenshot(const std::string& path, int w, int h) {
    std::vector<uint8_t> px((size_t)w * h * 3);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, px.data());
    stbi_flip_vertically_on_write(1);
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    stbi_write_png(path.c_str(), w, h, 3, px.data(), w * 3);
    logMsg("Screenshot: %s", path.c_str());
}

int main(int argc, char** argv) {
    if (!std::filesystem::exists("scripts/main.lua")) { // Projektordner finden (scripts/ und config.lua)
        std::filesystem::path p = SDL_GetBasePath() ? SDL_GetBasePath() : ".";
        for (int i = 0; i < 4; i++) {
            if (std::filesystem::exists(p / "scripts/main.lua")) { std::filesystem::current_path(p); break; }
            p = p.parent_path();
        }
    }
    loadConfig();
    double shotAt = -1;
    std::string shotPath, startLesson, joinAddr;
    bool startEditor = false, startHost = false, showMenu = true, startMap = false, testTp = false;
    double testTpLat = 0, testTpLon = 0;
    bool revTest = false;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--vr") cfg.vr = true;
        else if (a == "--autodrive") { autodrive = true; showMenu = false; }
        else if (a == "--chase") camMode = 1;
        else if (a == "--editor") { startEditor = true; showMenu = false; }
        else if (a == "--host") startHost = true;
        else if (a == "--nomenu") showMenu = false;
        else if (a == "--map") { startMap = true; showMenu = false; }
        else if (a == "--revtest") { revTest = true; showMenu = false; cfg.traffic = 0; }
        else if (a == "--tp" && i + 2 < argc) { testTpLat = atof(argv[i + 1]); testTpLon = atof(argv[i + 2]); testTp = true; i += 2; } // Selbsttest: nach 4 s teleportieren
        else if (a == "--traffic" && i + 1 < argc) cfg.traffic = atoi(argv[++i]);
        else if (a == "--join" && i + 1 < argc) joinAddr = argv[++i];
        else if (a == "--name" && i + 1 < argc) cfg.playerName = argv[++i];
        else if (a == "--place" && i + 1 < argc) cfg.startPlace = argv[++i];
        else if (a == "--lesson" && i + 1 < argc) { startLesson = argv[++i]; showMenu = false; }
        else if (a == "--shot" && i + 2 < argc) { shotAt = atof(argv[i + 1]); shotPath = argv[i + 2]; i += 2; }
    }

    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_JOYSTICK | SDL_INIT_HAPTIC)) { logMsg("SDL_Init: %s", SDL_GetError()); return 1; }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    if (!cfg.vr) { SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 1); SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 4); }
    win = SDL_CreateWindow("worldridr", cfg.width, cfg.height, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | (cfg.fullscreen ? SDL_WINDOW_FULLSCREEN : 0));
    if (!win) { logMsg("Fenster: %s", SDL_GetError()); return 1; }
    SDL_GLContext ctx = SDL_GL_CreateContext(win);
    if (!ctx) { logMsg("OpenGL 3.3 nicht verfuegbar: %s", SDL_GetError()); return 1; }
    if (!gfxInit() || !font.load()) return 1;
    logMsg("OpenGL: %s", (const char*)glGetString(GL_RENDERER));

    Vertex sky[3] = {{{-1, -1, 0}, {0, 0, 1}, {0, 0}, Color{}}, {{3, -1, 0}, {0, 0, 1}, {0, 0}, Color{}}, {{-1, 3, 0}, {0, 0, 1}, {0, 0}, Color{}}};
    skyMesh.create(sky, 3);
    mirrorRT[0].create(416, 264); mirrorRT[1].create(640, 180); mirrorRT[2].create(416, 264);
    for (int i = 0; i < 3; i++) {
        Vec3 c = MIRROR_POS[i];
        float hw = MIRROR_HW[i], hh = MIRROR_HH[i], z = c.z + 0.022f;
        mirrorQuad[i].quad({c.x - hw, c.y - hh, z}, {c.x + hw, c.y - hh, z}, {c.x + hw, c.y + hh, z}, {c.x - hw, c.y + hh, z}, {0, 0, 1}, Color{});
    }
    carModelInit();

    audioInit(cfg.volume);
    input.init(cfg.wheel);
    loadShifter();
    world.init(cfg);
    if (!cfg.startPlace.empty()) {
        double lat, lon;
        if (geocode(cfg.startPlace, lat, lon)) { cfg.lat = lat; cfg.lon = lon; }
        else logMsg("Startort \"%s\" nicht gefunden - verwende Standardkoordinaten", cfg.startPlace.c_str());
    }
    world.setOrigin(cfg.lat, cfg.lon);
    editor::init();
    script.init(&hud);
    if (cfg.vr) vr.init();
    if (startHost) netplay::host(cfg.port);
    if (!joinAddr.empty()) netplay::join(joinAddr, cfg.port);
    menuOpen = showMenu;
    traffic::wanted = cfg.traffic;
    int swapInterval = -1;

    Uint64 last = SDL_GetPerformanceCounter();
    bool starterPrev = false, starterLock = false, wantShot = false;
    double spawnTime = -1, lastTelemetry = 0, lastAutoShift = 0;
    int shotCounter = 0;

    while (running) {
        input.beginFrame();
        bool uiClick = false;
        bool edit = editor::active();
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) { running = false; continue; }
            if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT) uiClick = true;
            if (e.type == SDL_EVENT_TEXT_INPUT && typing) { typed += e.text.text; continue; }
            if (e.type == SDL_EVENT_KEY_DOWN && typing) {
                SDL_Keycode k = e.key.key;
                if (k == SDLK_ESCAPE) { typing = 0; SDL_StopTextInput(win); }
                else if (k == SDLK_BACKSPACE) { while (!typed.empty()) { unsigned char ch = typed.back(); typed.pop_back(); if ((ch & 0xC0) != 0x80) break; } }
                else if (k == SDLK_RETURN || k == SDLK_KP_ENTER) {
                    int mode = typing;
                    typing = 0; SDL_StopTextInput(win);
                    if (!typed.empty() && mode == 1) startTeleport(typed);
                    if (!typed.empty() && mode == 2) { netplay::join(typed, cfg.port); statusLine = netplay::status(); }
                    if (!typed.empty() && mode == 3) startTeleport(typed, true);
                }
                continue;
            }
            if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) {
                SDL_Keycode k = e.key.key;
                if (k == SDLK_ESCAPE && calStep >= 0) { calStep = -1; statusLine.clear(); continue; }
                if (k == SDLK_ESCAPE && worldmap::active()) { worldmap::close(); statusLine.clear(); continue; }
                if (k == SDLK_ESCAPE) { menuOpen = !menuOpen; menuPage = 0; statusLine.clear(); continue; }
                if (k == SDLK_M && !edit && !menuOpen) { if (worldmap::active()) worldmap::close(); else worldmap::open(); continue; }
                if (k == SDLK_P) { wantShot = true; continue; }
                if (k == SDLK_F2 && spawned) { if (edit) editor::close(); else { Vec3 f = car.forward(); editor::open(car.pos, std::atan2(-f.x, -f.z)); } continue; }
            }
            if (menuOpen) continue;
            if (worldmap::active()) { worldmap::event(e); continue; }
            if (edit) { editor::event(e); continue; }
            input.handleEvent(e, false);
            if (e.type == SDL_EVENT_MOUSE_MOTION && (e.motion.state & SDL_BUTTON_RMASK)) {
                lookYaw = clampv(lookYaw - e.motion.xrel * 0.004f, -2.4f, 2.4f);
                lookPitch = clampv(lookPitch - e.motion.yrel * 0.004f, -1.0f, 1.0f);
            } else if (e.type == SDL_EVENT_MOUSE_BUTTON_UP && e.button.button == SDL_BUTTON_RIGHT) { lookYaw = lookPitch = 0; }
            else if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) {
                SDL_Keycode k = e.key.key;
                if (k == SDLK_T) beginTyping(1);
                else if (k == SDLK_F3) debugOverlay = !debugOverlay;
                else if (k == SDLK_F5) vr.recenter();
                else if (k == SDLK_F1) script.event("key", "f1");
                else if (k >= SDLK_0 && k <= SDLK_9) { char s[2] = {(char)('0' + (k - SDLK_0)), 0}; script.event("key", s); }
            }
        }
        edit = editor::active();
        Uint64 nowC = SDL_GetPerformanceCounter();
        float dt = clampv((float)((double)(nowC - last) / (double)SDL_GetPerformanceFrequency()), 0.0005f, 0.05f);
        last = nowC;
        simTime += dt;

        if (tpState == 2) { // Ortswechsel abgeschlossen
            std::lock_guard<std::mutex> lk(tpMutex);
            world.setOrigin(tpLat, tpLon);
            spawned = false; spawnTime = -1; statusLine.clear(); tpState = 0;
            car.place({0, 0, 0}, 0);
            if (edit) editor::open({0, 0, 0}, 0);
        } else if (tpState == 3) { statusLine = "Ort nicht gefunden"; tpState = 0; }
        else if (tpState == 4) { // Suchergebnis nur auf der Karte zeigen
            std::lock_guard<std::mutex> lk(tpMutex);
            worldmap::setCenter(tpLat, tpLon, 14);
            statusLine.clear(); tpState = 0;
        }

        bool driving = spawned && !edit;
        if (testTp && spawned && simTime - spawnTime > 4) { testTp = false; gameTeleport(testTpLat, testTpLon); }
        bool mapOpen = worldmap::active();
        bool controls = driving && !menuOpen && !typing && !mapOpen;
        input.update(dt, car.speed(), !controls);
        updateShifterSetup(dt);
        Vec3 focus = edit ? editor::cameraPos() : (spawned ? car.pos : Vec3{});
        world.update(focus, simTime);
        editor::refresh(simTime);
        // Strassenbezug jedes Bild neu bestimmen: Zeiger in nachgeladene/entfernte Kacheln duerfen nicht ueberleben
        roadHit = spawned ? world.nearestRoad(car.pos.x, car.pos.z, 400) : RoadHit{};
        if (roadHit.found && roadHit.dist > 60) roadHit.found = false;
        bool ready =world.ready(spawned ? car.pos : Vec3{}, simTime);
        if (!spawned && ready) {
            spawnCar(false);
            spawned = true; spawnTime = simTime;
            script.event("spawn");
            if (!startLesson.empty()) { script.event("key", "f1"); script.event("key", startLesson.c_str()); startLesson.clear(); }
            if (startMap) { worldmap::open(); startMap = false; }
            if (startEditor) { // Selbsttest: Uebersicht von oben auf den Startpunkt
                editor::open(car.pos, 0);
                editor::setCamera({10, car.pos.y + 45, 55}, 0, -0.62f);
                startEditor = false;
            }
        }
        Vec3 tdPos;
        float tdYaw;
        if (editor::wantsTestDrive(tdPos, tdYaw)) { // aus dem Studio direkt losfahren
            editor::close();
            car.place({tdPos.x, tdPos.y + 0.62f, tdPos.z}, tdYaw);
            car.handbrake = false;
            edit = false;
        }

        // ---------- Oberflaeche ----------
        int ww, wh;
        SDL_GetWindowSizeInPixels(win, &ww, &wh);
        if (ww < 1 || wh < 1) { SDL_Delay(50); continue; }
        float aspect = (float)ww / wh, uiH = 720, uiW = uiH * aspect;
        float mxp, myp;
        Uint32 mb = SDL_GetMouseState(&mxp, &myp);
        bool cockpit = camMode == 0 && !edit;
        ui.begin(uiW, uiH, mxp * uiH / wh, myp * uiH / wh, (mb & SDL_BUTTON_LMASK) != 0, uiClick);
        if (mapOpen && !menuOpen) worldmap::buildUI();
        else if (!edit) buildHUD(cockpit, !spawned);
        if (edit && !menuOpen) editor::buildUI();
        buildPrompt();
        if (menuOpen) buildMenu();

        // ---------- Fahren ----------
        if (driving) {
            car.automatic = cfg.automatic;
            car.autoClutch = cfg.autoClutch == -1 ? !input.wheelPresent : cfg.autoClutch == 1;
            car.steer = input.steer; car.throttle = input.throttle; car.brake = input.brake; car.clutch = input.clutch;
            // Zuendschluessel: bei laufendem Motor = abstellen, sonst Anlasser solange gehalten
            if (input.starter && !starterPrev && car.engineOn) { car.engineOn = false; starterLock = true; }
            if (!input.starter) starterLock = false;
            car.starter = input.starter && !starterLock && !car.engineOn;
            starterPrev = input.starter;
            if (input.shiftUp) car.shift(1);
            if (input.shiftDown) car.shift(-1);
            if (input.hGear != -99 && controls && calStep < 0) { // H-Schaltung: Hebelstellung = Gang
                static int lastGrind = -99;
                if (input.hGear == car.gear) lastGrind = -99;
                else {
                    car.requestGear(input.hGear);
                    if (car.evGrind) { if (lastGrind == input.hGear) car.evGrind = false; else lastGrind = input.hGear; } // nur einmal pro Versuch melden
                }
            }
            if (!input.shifterReady() && (input.buttons >> 11) != 0 && calStep < 0) { // Hebel bewegt, aber noch nicht eingerichtet
                static double hinted = -100;
                if (simTime - hinted > 20) { hinted = simTime; hud.msgs.push_back({"Schalthebel erkannt: Esc > \"Schalthebel einrichten\"", 6.0f}); }
            }
            if (input.handbrake) car.handbrake = !car.handbrake;
            if (input.belt) belt = !belt;
            if (input.lights) lights = !lights;
            if (input.camera) camMode = (camMode + 1) % 2;
            if (input.blinkL) { blinker = blinker == -1 ? 0 : -1; blinkArmed = false; }
            if (input.blinkR) { blinker = blinker == 1 ? 0 : 1; blinkArmed = false; }
            if (input.hazard) blinker = blinker == 2 ? 0 : 2;
            if (input.reset || script.wantReset) { spawnCar(true); script.wantReset = false; }

            roadHit = world.nearestRoad(car.pos.x, car.pos.z, 400);
            onRoad = !roadHit.found || roadHit.dist < roadHit.seg->hw + 0.8f;
            if (roadHit.found && roadHit.dist > 60) roadHit.found = false;
            Vec3 f = car.forward();
            wrongWay = false; laneOffset = 0;
            if (roadHit.found && onRoad) {
                float along = f.x * roadHit.dx + f.z * roadHit.dz;
                laneOffset = roadHit.side * (along >= 0 ? 1.0f : -1.0f);
                wrongWay = roadHit.seg->oneway != 0 && along * roadHit.seg->oneway < -0.3f && car.speed() > 1.5f;
            }
            car.onRoad = onRoad;

            if (autodrive) { // Selbsttest: folgt der Strasse
                belt = true; car.handbrake = false; car.autoClutch = true;
                car.starter = !car.engineOn;
                if (car.engineOn && !car.automatic) {
                    if (car.gear < 1) car.requestGear(1);
                    else if (car.rpm() > 3000 && car.gear < 4 && simTime - lastAutoShift > 1.5) { car.shift(1); lastAutoShift = simTime; }
                }
                if (car.automatic) car.selector = 3;
                car.throttle = car.speed() < 11 ? 0.45f : 0.08f;
                car.brake = 0;
                if (roadHit.found) {
                    float sgn = (f.x * roadHit.dx + f.z * roadHit.dz) >= 0 ? 1.0f : -1.0f;
                    float errR = sgn * (roadHit.dx * (-f.z) + roadHit.dz * f.x);
                    float target = roadHit.seg->oneway ? 0.0f : roadHit.seg->hw * 0.5f;
                    car.steer = clampv(errR * 2.0f + (target - laneOffset) * 0.12f, -1.0f, 1.0f);
                }
            }

            if (revTest) { // Selbsttest Motor: im Leerlauf feste Pedalstellungen halten und die Drehzahl protokollieren
                double t = simTime - spawnTime;
                car.autoClutch = true; car.handbrake = true; car.starter = !car.engineOn;
                if (car.gear != 0) car.requestGear(0);
                car.throttle = t < 3 ? 0.0f : t < 7 ? 0.10f : t < 11 ? 0.15f : t < 15 ? 0.20f : t < 18 ? 0.0f : t < 19.2 ? 1.0f : 0.0f;
                static double lastLog = 0;
                if (simTime - lastLog > (t > 17.9 && t < 20.5 ? 0.1 : 1.0)) { lastLog = simTime; logMsg("revtest t=%.1f gas=%.2f rpm=%.0f", t, car.throttle, car.rpm()); }
            }
            Vec3 prev = car.pos;
            car.update(dt, world);
            crashCooldown -= dt;
            if (world.insideBuilding(car.pos.x, car.pos.z) || editor::blocked(car.pos.x, car.pos.z, car.pos.y)) {
                car.pos.x = prev.x; car.pos.z = prev.z;
                bool hard = length(car.vel) > 1.5f;
                car.vel = car.vel * -0.15f;
                if (hard && crashCooldown <= 0) { script.event("crash"); crashCooldown = 2.0f; }
            }
            if (car.evStall) script.event("stall");
            if (car.evGrind) { script.event("grind"); car.evGrind = false; }
            checkControls();
            traffic::update(dt);

            // Blinker: Takt + automatisches Zuruecksetzen nach dem Abbiegen
            bool lit = blinker != 0 && std::fmod(simTime, 0.74) < 0.37;
            if (lit != blinkLit && blinker != 0) audioClick();
            blinkLit = lit;
            if (blinker == 1 || blinker == -1) {
                if (car.steer * blinker > 0.28f) blinkArmed = true;
                if (blinkArmed && car.steer * blinker < 0.06f) { blinker = 0; blinkArmed = false; }
            }

            double lat, lon;
            world.toLatLon(car.pos.x, car.pos.z, lat, lon);
            script.carBegin();
            script.carNum("speed_kmh", std::fabs(car.speed()) * 3.6);
            script.carNum("speed_signed", car.speed() * 3.6);
            script.carNum("rpm", car.rpm());
            script.carNum("gear", car.gear);
            script.carNum("selector", car.selector);
            script.carNum("clutch", car.clutch);
            script.carNum("throttle", car.throttle);
            script.carNum("brake", car.brake);
            script.carNum("steer", car.steer);
            script.carNum("blinker", blinker);
            script.carNum("heading", std::fmod(std::atan2(f.x, -f.z) * 180.0 / PI + 360.0, 360.0));
            script.carNum("odo", car.odo);
            script.carNum("lane_offset", laneOffset);
            script.carNum("speed_limit", roadHit.found && onRoad ? roadHit.seg->maxspeed : 0);
            script.carNum("road_class", roadHit.found ? roadHit.seg->klass : -1);
            script.carNum("lat", lat);
            script.carNum("lon", lon);
            script.carBool("automatic", car.automatic);
            script.carBool("auto_clutch", car.autoClutch);
            script.carBool("handbrake", car.handbrake);
            script.carBool("engine_on", car.engineOn);
            script.carBool("belt", belt);
            script.carBool("lights", lights);
            script.carBool("on_road", onRoad);
            script.carBool("wrong_way", wrongWay);
            script.carBool("two_way_marked", roadHit.found && roadHit.seg->oneway == 0 && roadHit.seg->marked);
            script.carBool("wheel", input.wheelPresent);
            script.carStr("road_name", roadHit.found ? roadHit.name : "");
            script.carEnd();
            script.update(dt);

            float load = car.engineOn ? clampv(car.throttle * 0.9f + 0.1f, 0.0f, 1.0f) : (car.starter ? 0.5f : 0.0f);
            audioSet(car.rpm(), load, std::fabs(car.speed()), input.horn);
            input.setFFB(car.ffb, dt);

            if (autodrive && simTime - lastTelemetry > 1.0) {
                lastTelemetry = simTime;
                float gh = 0;
                world.surface(car.pos.x, car.pos.z, car.pos.y, gh);
                logMsg("t=%.0f v=%.1f km/h rpm=%.0f gang=%d hoehe=%.2f strasse=%d spur=%+.1f mitspieler=%d", simTime, car.speed() * 3.6f, car.rpm(), car.gear,
                       car.pos.y - gh, (int)onRoad, laneOffset, (int)netplay::cars().size());
                for (const NetCar& c : netplay::cars())
                    logMsg("   Mitspieler %s: Abstand %.1f m (dx %.1f dy %.1f dz %.1f)", c.name.c_str(), length(c.pos - car.pos), c.pos.x - car.pos.x, c.pos.y - car.pos.y, c.pos.z - car.pos.z);
            }
        } else {
            audioSet(edit ? 0.0f : car.rpm(), 0.1f, 0, false);
            input.setFFB(0, dt);
        }
        for (auto it = hud.msgs.begin(); it != hud.msgs.end();) { it->ttl -= dt; it = it->ttl <= 0 ? hud.msgs.erase(it) : it + 1; }

        // ---------- Mehrspieler ----------
        if (netplay::active() && spawned) {
            NetCar me;
            me.name = cfg.playerName;
            world.toLatLon(car.pos.x, car.pos.z, me.lat, me.lon);
            me.y = car.pos.y; me.q = car.q; me.steer = car.w[0].steer; me.speed = car.speed(); me.wheelRot = car.w[0].rot;
            me.flags = (uint8_t)((((blinker == -1 || blinker == 2) && blinkLit) ? 1 : 0) | (((blinker == 1 || blinker == 2) && blinkLit) ? 2 : 0) | (car.brake > 0.05f ? 4 : 0) | (lights ? 8 : 0));
            netplay::update(dt, me);
        }

        // ---------- Rendern ----------
        Mat4 M = car.transform();
        buildProps(focus);
        bool vrFrame = !edit && vr.beginFrame();
        int wantInterval = vr.running ? 0 : 1;
        if (wantInterval != swapInterval) { SDL_GL_SetSwapInterval(wantInterval); swapInterval = wantInterval; }
        if (cfg.mirrors && (cockpit || vrFrame) && spawned) renderMirrors(M);

        bool drewWindow = false;
        if (vrFrame) {
            if (vr.shouldRender()) {
                for (int i = 0; i < 2; i++) {
                    vr.bindEye(i);
                    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
                    Mat4 camM = mul(M, mul(translate(EYE), vr.eye[i].pose));
                    Mat4 view = rigidInverse(camM);
                    drawScene(view, vr.eye[i].proj, xformPoint(camM, {0, 0, 0}));
                    Mat4 vp = mul(vr.eye[i].proj, view);
                    drawOwnCar(vp, true);
                    // Hinweise als schwebende Tafel (1,2 m breit, 1,6 m vor dem Fahrer); Instrumente sind im Armaturenbrett
                    const float s = 1.2f / uiW;
                    Mat4 panelM = mul(M, mul(translate({EYE.x, EYE.y + 0.05f, EYE.z - 1.6f}), mul(scaleM(s, -s, s), translate({-uiW * 0.5f, -uiH * 0.5f, 0}))));
                    ui.flush(mul(vp, panelM));
                    if (i == 0) {
                        glBindFramebuffer(GL_READ_FRAMEBUFFER, vr.eye[0].fbo);
                        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
                        glBlitFramebuffer(0, 0, vr.eye[0].w, vr.eye[0].h, 0, 0, ww, wh, GL_COLOR_BUFFER_BIT, GL_LINEAR);
                        drewWindow = true;
                    }
                    vr.releaseEye(i);
                }
            }
            vr.endFrame();
        }
        if (!drewWindow) {
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glViewport(0, 0, ww, wh);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            Mat4 camM;
            if (edit) camM = editor::camera();
            else if (cockpit) camM = mul(M, mul(translate(EYE), mul(rotY(lookYaw + input.lookYaw), rotX(lookPitch - 0.13f))));
            else {
                Vec3 f = car.forward();
                float yaw = std::atan2(-f.x, -f.z) + lookYaw;
                Vec3 back{std::sin(yaw), 0, std::cos(yaw)};
                camM = mul(translate(car.pos + back * 7.2f + Vec3{0, 2.2f, 0}), mul(rotY(yaw), rotX(-0.20f + lookPitch)));
            }
            Mat4 view = rigidInverse(camM), proj = perspective(cfg.fov * PI / 180.0f, aspect, 0.08f, 40000.0f);
            mainVP = mul(proj, view);
            if (edit && !menuOpen) editor::update(dt, mainVP, (float)ww, (float)wh);
            drawScene(view, proj, xformPoint(camM, {0, 0, 0}));
            if (spawned) drawOwnCar(mainVP, cockpit);
            if (edit) editor::drawGizmos(mainVP);
            if (worldmap::active()) worldmap::draw(ortho(0, uiW, uiH, 0), ww, wh);
            ui.flush(ortho(0, uiW, uiH, 0));
        }
        bool autoShot = shotAt >= 0 && ((spawned && simTime - spawnTime >= shotAt) || simTime > shotAt + 90);
        if (wantShot || autoShot) {
            char name[64];
            snprintf(name, sizeof name, "screenshots/shot_%03d.png", shotCounter++);
            saveScreenshot(autoShot ? shotPath : name, ww, wh);
            wantShot = false;
            if (autoShot) running = false;
        }
        SDL_GL_SwapWindow(win);
    }
    input.setFFB(0, 0);
    netplay::stop();
    SDL_Quit();
    _exit(0); // Netzwerk-Threads nicht abwarten
}
