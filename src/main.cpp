// worldridr - Fahrschul-Simulator auf echten Karten
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <lua.hpp>
#include <nlohmann/json.hpp>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>
#include "audio.h"
#include "game.h"
#include "keys.h"
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
static Target mirrorRT[3], sceneRT;
static Mesh skyMesh;

static bool lightsOn();
static bool spawned = false, belt = false, lights = false, debugOverlay = false, autodrive = false;
static int blinker = 0, camMode = 0; // blinker: -1 links, 1 rechts, 2 Warnblinker
static bool blinkArmed = false, blinkLit = false;
static int typing = 0;               // 0 nichts, 1 Ort, 2 Server-Adresse
static std::string typed, statusLine;
static float lookYaw = 0, lookPitch = 0, crashCooldown = 0;
static RoadHit roadHit;
static bool onRoad = true, wrongWay = false, running = true;
static float laneOffset = 0;
static bool menuOpen = false, started = false;
static float previewYawOff = 0;
static int keyListen = -1;   // Tastenbelegung: Aktion, die gerade eine neue Taste bekommt
struct ThumbRect { int idx; float x, y, w, h; };
static std::vector<ThumbRect> thumbRects;      // Vorschaubilder im Autokatalog (je Eintrag eines)
static std::map<int, Target> thumbs;
static int thumbColorKey = -2;
static double spawnedAt = -100;
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
    } else if (lua_type(L, -1) == LUA_TSTRING) { cfg.startPlace = lua_tostring(L, -1); cfg.startAuto = cfg.startPlace == "auto"; if (cfg.startAuto) cfg.startPlace.clear(); }
    if (lua_istable(L, -1)) cfg.startAuto = false;
    lua_pop(L, 1);
    cfg.googleKey = str("google_maps_api_key");
    cfg.automatic = str("transmission") == "automatic";
    if (!str("player_name").empty()) cfg.playerName = str("player_name");
    if (!str("car_model").empty()) cfg.carModel = str("car_model");
    lua_getglobal(L, "auto_clutch");
    if (lua_isboolean(L, -1)) cfg.autoClutch = lua_toboolean(L, -1) ? 1 : 0;
    lua_pop(L, 1);
    cfg.vr = boolean("vr", cfg.vr);
    cfg.fullscreen = boolean("fullscreen", cfg.fullscreen);

    cfg.width = (int)num("width", cfg.width);
    cfg.height = (int)num("height", cfg.height);
    cfg.nearZoom = (int)num("near_zoom", cfg.nearZoom);
    cfg.port = (int)num("port", cfg.port);
    cfg.quality = clampv((int)num("quality", cfg.quality), 0, 2);
    cfg.police = boolean("police", cfg.police);
    cfg.faultPoints = boolean("fault_points", cfg.faultPoints);
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

// ---------- Einstellungen (settings.cfg) und Qualitaetsstufen ----------
static void presetValues(int q) {
    q = clampv(q, 0, 2);
    static const float scale[3] = {0.70f, 0.85f, 1.0f}, bld[3] = {450, 800, 1500}, road[3] = {600, 1000, 1800};
    static const int traffic[3] = {6, 12, 22}, peds[3] = {4, 10, 20};
    cfg.quality = q;
    cfg.renderScale = scale[q]; cfg.bldDist = bld[q]; cfg.roadDist = road[q];
    cfg.traffic = traffic[q]; cfg.peds = peds[q]; cfg.mirrors = q >= 1; cfg.msaa = q >= 2;
}
static void applyQualityGL() {
    world.setQuality(cfg.quality);
    gfxQuality(cfg.quality > 0 ? 1 : 0);
    ped::wanted = cfg.peds;
    traffic::wanted = cfg.traffic;
}
// Fahrzeug des Spielers (Auto / Moped / Motorrad): Physik, Tank und Kameraposition setzen
static void applyVehicle() {
    car.setKind(vehKind);
    EYE = vehKind == 0 ? Vec3{-0.36f, 0.60f, 0.22f} : Vec3{0.0f, 0.98f, 0.14f};
    static const float cap[3] = {45.0f, 6.5f, 12.0f}, use[3] = {1.0f, 0.12f, 0.25f};
    car.setEngine(cfg.diesel ? 1 : 0);
    float old = econ::fuel;
    econ::fuel = -1;
    econ::setTank(cap[vehKind], use[vehKind]);
    if (old >= 0) econ::fuel = std::min(old, cap[vehKind]);
}
static void saveSettings() {
    FILE* f = fopen("settings.cfg", "w");
    if (!f) return;
    fprintf(f, "quality=%d\nscale=%.2f\nmirrors=%d\ntraffic=%d\npolice=%d\nfaults=%d\nhud=%d\nminimap=%d\nautomatic=%d\ncamera=%d\ncar=%s\nmsaa=%d\nbld=%.0f\nroad=%.0f\npeds=%d\nmapq=%d\nhour=%.2f\nweather=%d\ntimespeed=%d\n",
            cfg.quality, cfg.renderScale, (int)cfg.mirrors, cfg.traffic, (int)cfg.police, (int)cfg.faultPoints, cfg.hudMode, (int)cfg.minimap, (int)cfg.automatic, camMode,
            carModelSel >= 0 ? carModelName(carModelSel).c_str() : "builtin", (int)cfg.msaa, cfg.bldDist, cfg.roadDist, cfg.peds, (int)cfg.mapQuestions, envir::hour, envir::weather, envir::speedIdx);
    if (carColorSet) fprintf(f, "carcolor=%d,%d,%d\n", carColor[0], carColor[1], carColor[2]);
    // Der Tankstand wird bewusst nicht gespeichert: jede Fahrt beginnt mit vollem Tank (sonst kommt man mit leerem Tank nicht mehr los).
    fprintf(f, "veh=%d\nmoney=%.2f\ndiesel=%d\n", vehKind, econ::money, (int)cfg.diesel);
    keys::save(f);
    fclose(f);
}
// vor dem Fenster aufrufen (MSAA gilt nur beim Start)
static bool loadSettings() {
    presetValues(cfg.quality);
    FILE* f = fopen("settings.cfg", "r");
    if (!f) return false;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char* eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        std::string k = line, v = eq + 1;
        while (!v.empty() && (v.back() == '\n' || v.back() == '\r')) v.pop_back();
        double n = atof(v.c_str());
        if (k == "quality") presetValues((int)n);
        else if (k == "scale") cfg.renderScale = clampv((float)n, 0.4f, 1.0f);
        else if (k == "mirrors") cfg.mirrors = n != 0;
        else if (k == "traffic") cfg.traffic = clampv((int)n, 0, 40);
        else if (k == "peds") cfg.peds = clampv((int)n, 0, 40);
        else if (k == "police") cfg.police = n != 0;
        else if (k == "faults") cfg.faultPoints = n != 0;
        else if (k == "hud") cfg.hudMode = clampv((int)n, 0, 1);
        else if (k == "minimap") cfg.minimap = n != 0;
        else if (k == "mapq") cfg.mapQuestions = n != 0;
        else if (k == "veh") vehKind = clampv((int)n, 0, 2);
        else if (k == "fuel") continue; // alte Einstellungsdateien: gespeicherter Tankstand wird ignoriert, der Tank startet voll
        else if (k == "money") econ::money = (float)n;
        else if (k == "carcolor") { int r, g, b; carColorSet = sscanf(v.c_str(), "%d,%d,%d", &r, &g, &b) == 3; if (carColorSet) { carColor[0] = (uint8_t)r; carColor[1] = (uint8_t)g; carColor[2] = (uint8_t)b; } }
        else if (k == "automatic") cfg.automatic = n != 0;
        else if (k == "camera") camMode = clampv((int)n, 0, 1);
        else if (k == "car") cfg.carModel = v;
        else if (k == "msaa") cfg.msaa = n != 0;
        else if (k == "bld") cfg.bldDist = (float)n;
        else if (k == "road") cfg.roadDist = (float)n;
        else if (k == "diesel") cfg.diesel = n != 0;
        else if (k == "hour") envir::hour = clampv((float)n, 0.0f, 24.0f);
        else if (k == "weather") envir::weather = clampv((int)n, 0, 5);
        else if (k == "timespeed") envir::speedIdx = clampv((int)n, 0, 3);
        else keys::load(k, v);
    }
    fclose(f);
    return true;
}

// Standort per IP-Adresse (Stadtgenauigkeit). Das Ergebnis wird 7 Tage zwischengespeichert, damit nicht bei jedem Start gefragt wird.
static bool ipLocate(double& lat, double& lon, std::string& city) {
    namespace fs = std::filesystem;
    const char* cachePath = "cache/ip_location.json";
    std::error_code ec;
    if (fs::exists(cachePath, ec)) {
        auto age = fs::file_time_type::clock::now() - fs::last_write_time(cachePath, ec);
        std::vector<uint8_t> data;
        if (!ec && age < std::chrono::hours(24 * 7) && readFile(cachePath, data)) {
            auto j = nlohmann::json::parse(data.begin(), data.end(), nullptr, false);
            if (j.is_object() && j.contains("lat") && j.contains("lon")) { lat = j["lat"]; lon = j["lon"]; city = j.value("city", ""); return true; }
        }
    }
    std::vector<uint8_t> out;
    int st = 0;
    if (!httpRequest("https://ipwho.is/?fields=success,latitude,longitude,city", out, st) || st != 200) return false;
    auto j = nlohmann::json::parse(out.begin(), out.end(), nullptr, false);
    if (!j.is_object() || !j.value("success", false) || !j.contains("latitude")) return false;
    lat = j["latitude"]; lon = j["longitude"]; city = j.value("city", "");
    nlohmann::json c = {{"lat", lat}, {"lon", lon}, {"city", city}};
    std::string s = c.dump();
    writeFile(cachePath, std::vector<uint8_t>(s.begin(), s.end()));
    return true;
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
    // Startpunkt: naechste befahrbare Strasse, deren Umgebung frei von Gebaeuden ist (Durchgaenge/Arkaden sind in den
    // Daten oft als Gebaeude eingetragen und wuerden das Auto festhalten)
    struct Cand { RoadSeg s; float d; };
    std::vector<Cand> cands;
    world.forSegmentsNear(p.x, p.z, 450, [&](const RoadSeg& s) {
        float mx = (s.ax + s.bx) * 0.5f, mz = (s.az + s.bz) * 0.5f;
        if (s.klass > 5 || std::hypot(s.bx - s.ax, s.bz - s.az) < 6.0f) return;
        cands.push_back({s, std::hypot(mx - p.x, mz - p.z)});
    });
    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.d < b.d; });
    float x = p.x, z = p.z, yaw = 0;
    bool found = false;
    for (const Cand& c : cands) {
        float len = std::hypot(c.s.bx - c.s.ax, c.s.bz - c.s.az), dx = (c.s.bx - c.s.ax) / len, dz = (c.s.bz - c.s.az) / len;
        float mx = (c.s.ax + c.s.bx) * 0.5f, mz = (c.s.az + c.s.bz) * 0.5f;
        auto clearDist = [&](float fx, float fz) { // wie weit geradeaus frei von Gebaeuden (bis 30 m)
            for (float t = 4; t <= 30; t += 4) if (world.insideBuilding(mx + fx * t, mz + fz * t)) return t;
            return 30.0f;
        };
        float fwdFree = clearDist(dx, dz), backFree = clearDist(-dx, -dz);
        if (c.s.oneway == -1) { dx = -dx; dz = -dz; std::swap(fwdFree, backFree); }
        if (world.insideBuilding(mx, mz) || (c.s.oneway != 0 ? fwdFree : std::max(fwdFree, backFree)) < 22.0f) continue;
        if (c.s.oneway == 0 && backFree > fwdFree) { dx = -dx; dz = -dz; }
        float off = c.s.oneway ? 0.0f : c.s.hw * 0.5f;
        x = mx - dz * off; z = mz + dx * off;
        yaw = std::atan2(-dx, -dz);
        found = true;
        break;
    }
    if (!found) { // Rueckfall: naechste Strasse wie bisher
        RoadHit h = world.nearestRoad(p.x, p.z, 600);
        if (h.found) {
            float dx = h.dx, dz = h.dz;
            if (h.seg->oneway == -1) { dx = -dx; dz = -dz; }
            float off = h.seg->oneway ? 0.0f : h.seg->hw * 0.5f;
            x = h.px - dz * off; z = h.pz + dx * off;
            yaw = std::atan2(-dx, -dz);
        }
    }
    float y = 0;
    logMsg("Start: %d Kandidaten, gefunden=%d, Gebaeude=%d, x=%.1f z=%.1f", (int)cands.size(), (int)found, (int)world.insideBuilding(x, z), x, z);
    world.surface(x, z, 1e9f, y);
    car.place({x, y + 0.62f, z}, yaw);
}

static Mat4 rotY(float a) { return fromQuatPos(qaxis({0, 1, 0}, a), {}); }
static Mat4 rotX(float a) { return fromQuatPos(qaxis({1, 0, 0}, a), {}); }

// Fahrer-Insasse: federt mit der Fahrdynamik und dreht den Kopf mit der Kamera bzw. dem VR-Headset
static Sway ownSwayLat, ownSwayLong;
static float ownHeadYaw = 0, ownHeadPitch = 0;
static void updateOwnOccupant(float dt) {
    static Vec3 prevV;
    Vec3 a = (car.vel - prevV) * (1.0f / std::max(dt, 0.001f));
    prevV = car.vel;
    Vec3 f = car.forward(), r{-f.z, 0, f.x};
    ownSwayLong.step(clampv(dot(a, f), -12.0f, 12.0f), dt);
    ownSwayLat.step(clampv(dot(a, r), -12.0f, 12.0f), dt);
    float ty = lookYaw + input.lookYaw, tp = lookPitch;
    if (vr.running) { // Kopfhaltung des VR-Headsets
        Vec3 hf = xformDir(vr.eye[0].pose, {0, 0, -1});
        ty = std::atan2(-hf.x, -hf.z);
        tp = std::asin(clampv(hf.y, -1.0f, 1.0f));
    }
    float k = std::min(1.0f, dt * 12.0f);
    ownHeadYaw += (clampv(ty, -1.6f, 1.6f) - ownHeadYaw) * k;
    ownHeadPitch += (clampv(tp, -0.7f, 0.7f) - ownHeadPitch) * k;
}

static CarVisual ownVisual() {
    CarVisual v;
    v.M = car.transform();
    v.steer = car.w[0].steer;
    for (int i = 0; i < 4; i++) { v.wheelRot[i] = car.w[i].rot; v.comp[i] = car.w[i].comp; }
    v.brake = car.brake > 0.05f;
    v.blinkL = ((blinker == -1 || blinker == 2) && blinkLit) || foot::lockBlink();
    v.blinkR = ((blinker == 1 || blinker == 2) && blinkLit) || foot::lockBlink();
    v.lights = lightsOn();
    v.reverse = car.gear == -1;
    carOwnPaint(v);
    v.driver = car.kind == 0; v.occX = ownSwayLat.x; v.occZ = -ownSwayLong.x; v.headYaw = ownHeadYaw; v.headPitch = ownHeadPitch;
    v.kind = car.kind;
    if (car.kind > 0) { // Zweirad neigt sich in Kurven
        static float leanS = 0;
        float target = std::atan(car.speed() * car.angVel.y / 9.81f) * 0.95f;
        leanS += (clampv(target, -0.6f, 0.6f) - leanS) * 0.12f;
        v.lean = leanS;
    }
    return v;
}
// Geltendes Tempolimit: Strassenlimit, bei aktiver Wechselanzeige davor das strengere (einmal je Bild berechnet)
static int effectiveLimit() {
    static double t = -1;
    static int v = 0;
    if (simTime != t) {
        t = simTime;
        int base = roadHit.found && onRoad ? (int)roadHit.seg->maxspeed : 0;
        int dyn = base > 0 ? signs::dynamicLimit(car.pos, car.forward()) : 0;
        v = dyn > 0 ? std::min(base, dyn) : base;
    }
    return v;
}
static DashState ownDash() {
    DashState d;
    d.kmh = car.speed() * 3.6f; d.rpm = car.rpm(); d.steerInput = car.steer; d.odoKm = car.odo / 1000.0f;
    d.gear = car.gear; d.selector = car.selector; d.automatic = car.automatic;
    d.limit = effectiveLimit();
    d.blinkL = (blinker == -1 || blinker == 2) && blinkLit; d.blinkR = (blinker == 1 || blinker == 2) && blinkLit;
    d.handbrake = car.handbrake; d.belt = belt; d.engineOn = car.engineOn; d.lights = lightsOn(); d.fuel = econ::fuelFrac();
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
        }   // Stopp- und Vorrangschilder zeichnet signs.cpp (echte Formen)
        if (c.type <= 1) {
            Vec3 a = c.pos + right * (c.oneway ? -c.hw + 0.3f : 0.15f), b = c.pos + right * (c.hw - 0.3f);
            Vec3 up{0, 0.06f, 0}, back = dir * -0.45f;
            stopLines.quad(a + up, b + up, b + back + up, a + back + up, {0, 1, 0}, Color{225, 225, 225, 255});
        }
    });
}

// ---------- Szene ----------
static bool lightsOn() { return lights || envir::night() > 0.55f; }   // Lichtautomatik bei Dunkelheit
static void drawScene(const Mat4& view, const Mat4& proj, Vec3 cam, bool mirrorPass = false, int mirrorIdx = -1) {
    Mat4 vp = mul(proj, view);
    envir::apply();
    { Vec3 hf = car.forward(); hf.y = 0; hf = normalize(hf);
      gfxHead(car.pos + hf * 2.0f + Vec3{0, 0.35f, 0}, normalize(hf + Vec3{0, -0.07f, 0}), lightsOn() ? 1.0f : 0.0f); }
    { // Himmel: Blickrichtung je Pixel direkt aus Projektion und Drehung (keine Matrix-Inverse, kein NaN, keine Kameraposition)
        const float* V = view.m; const float* P = proj.m;
        float ax = 1.0f / P[0], ay = 1.0f / P[5], ox = P[8] / P[0], oy = P[9] / P[5];
        Mat4 s{};
        for (int i = 0; i < 3; i++) {
            float M0 = V[i * 4 + 0], M1 = V[i * 4 + 1], M2 = V[i * 4 + 2];
            s.m[0 + i] = M0 * ax;
            s.m[4 + i] = M1 * ay;
            s.m[8 + i] = 0;
            s.m[12 + i] = M0 * ox + M1 * oy - M2;
        }
        s.m[3] = s.m[7] = s.m[11] = 0; s.m[15] = 1;
        gfxCamera(cam, s);
    }
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
        world.drawTerrain(level, vp, cam);
        if (level < 2) glClear(GL_DEPTH_BUFFER_BIT);
    }
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    gfxBias(2e-5f);
    gfxMode(0, FLAG_LIT | FLAG_FOG | FLAG_GLOSS);
    world.drawWater(vp, cam, mirrorPass ? 1200.0f : 3500.0f);
    glDepthMask(GL_FALSE);
    gfxBias(3e-5f);
    gfxMode(2);
    world.drawRoads(vp, cam, mirrorPass ? 250.0f : cfg.roadDist);
    gfxMode(0, FLAG_FOG);
    stopLines.draw();
    glDepthMask(GL_TRUE);
    gfxBias(1e-5f);
    gfxMode(2);
    world.drawCustomRoads(); // selbst gebaute Strassen liegen ueber dem Gelaende und schreiben Tiefe
    nav::drawWorld();
    glDisable(GL_BLEND);
    gfxBias(0);
    gfxMode(3);
    world.drawBuildings(vp, cam, mirrorPass ? 250.0f : cfg.bldDist);
    { // Fahrbahnen auf Bruecken: nach der Brueckenplatte, mit Tiefenwerten
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        gfxBias(1e-5f);
        gfxMode(2);
        world.drawDecks(vp, cam, mirrorPass ? 250.0f : cfg.roadDist);
        glDisable(GL_BLEND);
        gfxBias(0);
        gfxMode(3);
    }
    editor::drawWorld();
    gfxMode(0, FLAG_LIT | FLAG_FOG);
    forest::draw(mirrorPass);
    propsLit.draw();
    gfxMode(0, FLAG_FOG);
    propsUnlit.draw();
    signs::draw(vp, cam, simTime, mirrorPass);
    for (const NetCar& c : netplay::cars()) {
        if (!c.placed) continue;
        CarVisual v;
        v.M = fromQuatPos(c.qs, c.pos);
        v.steer = c.steer;
        for (float& r : v.wheelRot) r = c.wheelRot;
        v.paint = Color{c.paint[0], c.paint[1], c.paint[2], 255}; v.tinted = c.tinted; v.model = c.model; v.kind = c.kind;
        v.blinkL = (c.flags & 1) != 0; v.blinkR = (c.flags & 2) != 0; v.brake = (c.flags & 4) != 0; v.lights = (c.flags & 8) != 0;
        // Fahrer des Mitspielers mit seiner echten Kopfhaltung (bei VR: Headset), Hemdfarbe aus der Spieler-ID
        v.driver = c.kind == 0; v.headYaw = c.hyS; v.headPitch = c.hpS;
        { uint32_t h = c.id * 2654435761u; v.occShirt = Color{(uint8_t)(60 + (h >> 4 & 0x7F)), (uint8_t)(70 + (h >> 11 & 0x7F)), (uint8_t)(90 + (h >> 18 & 0x7F)), 255}; }
        carDraw(v, vp, false, nullptr);
    }
    traffic::draw(vp, cam, mirrorPass ? 160.0f : std::min(700.0f, cfg.bldDist));
    ped::draw(vp, cam, mirrorPass ? 90.0f : std::min(260.0f, cfg.bldDist * 0.5f));
    if (!mirrorPass) envir::drawPrecip(cam, car.vel, simTime);
    if (mirrorPass && mirrorIdx != 1 && spawned) { // Seitenspiegel zeigen die eigene Karosserie (Flanke und Heck)
        CarVisual self = ownVisual();
        carDraw(self, vp, false, nullptr);
    }
    gfxMVP(vp);
}

static void drawOwnCar(const Mat4& vp, bool cockpit) {
    CarVisual v = ownVisual();
    DashState d = ownDash();
    carDraw(v, vp, cockpit, &d);
    if (cockpit && cfg.mirrors && car.kind == 0) {
        gfxModel(v.M);
        gfxMode(4);
        gfxTint(0.9f, 0.92f, 0.95f);
        for (int i = 0; i < 3; i++) { gfxBindTexture(mirrorRT[i].tex); mirrorQuad[i].draw(); }
        gfxModel(identity());
    }
}

static void renderMirrors(const Mat4& M) {
    static const float yaw[3] = {PI - 0.17f, PI, PI + 0.17f}, fov[3] = {0.42f, 0.25f, 0.42f};
    static int turn = 0;
    int count = cfg.quality >= 2 ? 3 : 1; // niedrig/mittel: pro Bild nur ein Spiegel neu rendern
    for (int n = 0; n < count; n++) {
        int i = (turn++) % 3;
        Mat4 camM = mul(M, mul(translate(MIRROR_POS[i]), rotY(yaw[i])));
        Mat4 view = rigidInverse(camM);
        Mat4 proj = mul(scaleM(-1, 1, 1), perspective(fov[i], MIRROR_HW[i] / MIRROR_HH[i], 0.5f, 16000.0f));
        glBindFramebuffer(GL_FRAMEBUFFER, mirrorRT[i].fbo);
        glViewport(0, 0, mirrorRT[i].w, mirrorRT[i].h);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        drawScene(view, proj, xformPoint(camM, {0, 0, 0}), true, i);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

// ---------- HUD ----------
static const Color PANEL{14, 17, 23, 150};
static const Color SOFT{190, 196, 206, 255};

// ---------- HUD: bewusst ruhig. Im Cockpit zeigt das Armaturenbrett alles; ausserhalb nur das Noetigste ----------
static void buildHUD(bool cockpit, bool loading) {
    float W = ui.W, H = ui.H, cx = W * 0.5f;
    char buf[256];
    bool calm = cfg.hudMode == 0;
    { std::string fp = foot::prompt(); // zu Fuss: Hinweis zum Einsteigen/Sperren
        if (!fp.empty()) { float pw2 = font.width(fp, 17) + 36; ui.geo.rrect(cx - pw2 / 2, H - 120, pw2, 36, 12, Color{14, 17, 23, 215}); ui.labelC(cx, H - 96, 17, Color{255, 214, 120, 255}, fp); } }
    if ((!cockpit || car.kind > 0) && spawned && !loading && !foot::on) { // kompakte Tempoanzeige
        float by = H - 20;
        ui.geo.rrect(cx - 78, by - 48, 156, 48, 14, PANEL);
        snprintf(buf, sizeof buf, "%d", (int)std::lround(std::fabs(car.speed() * 3.6f)));
        ui.labelC(cx - 20, by - 13, 32, col::white, buf);
        ui.label(cx + 6, by - 14, 13, col::gray, "km/h");
        std::string g = car.automatic ? std::string(1, "PRND"[car.selector]) : (car.gear == -1 ? "R" : car.gear == 0 ? "N" : std::to_string(car.gear));
        ui.labelC(cx + 56, by - 13, 22, SOFT, g);
        if (!calm) {
            float rf = clampv(car.rpm() / 7000.0f, 0.0f, 1.0f);
            ui.geo.rrect(cx - 70, by - 6, 140, 3, 1.5f, Color{44, 48, 58, 200});
            ui.geo.rrect(cx - 70, by - 6, std::max(4.0f, 140 * rf), 3, 1.5f, rf > 0.86f ? col::red : col::blue);
        }
        std::string warn = !car.engineOn ? "Motor aus" : car.handbrake ? "Handbremse" : "";
        { // Tankstand: schmaler Balken, wird bei wenig Sprit rot
            float ff = econ::fuelFrac();
            ui.geo.rrect(cx - 52, by + 6, 104, 4, 2, Color{44, 48, 58, 200});
            ui.geo.rrect(cx - 52, by + 6, std::max(3.0f, 104 * ff), 4, 2, ff < 0.15f ? Color{238, 80, 66, 255} : Color{120, 190, 255, 230});
        }
        if (!warn.empty()) ui.labelC(cx, by - 58, 14, Color{240, 170, 120, 255}, warn);
    }
    if (roadHit.found && onRoad && spawned && !loading) { // Tempolimit: klein, unaufdringlich
        int lim = effectiveLimit();
        bool over = std::fabs(car.speed() * 3.6f) > lim + 3;
        float r = calm ? 23.0f : 32.0f, px = W - r - 20, py = r + 18;
        ui.geo.disc(px, py, r, over ? Color{238, 70, 60, 255} : Color{200, 52, 52, 235});
        ui.geo.disc(px, py, r * 0.76f, Color{240, 240, 240, 245});
        snprintf(buf, sizeof buf, "%d", lim);
        ui.labelC(px, py + r * 0.3f, r * 0.8f, Color{15, 15, 15, 255}, buf);
        if (!calm && !roadHit.name.empty()) ui.labelR(W - 20, py + r + 22, 15, SOFT, roadHit.name);
    }
    float ty = 14;
    bool lessonText = !hud.instruction.empty() && (hud.lessonActive || !hud.title.empty());
    if (lessonText) { // Fahrstunde: ein ruhiges Feld oben
        auto lines = wrapText(hud.instruction, 17, 520);
        float h = 22 + (hud.title.empty() ? 0 : 20) + lines.size() * 23.0f;
        ui.geo.rrect(cx - 290, ty, 580, h, 12, PANEL);
        float y = ty + 22;
        if (!hud.title.empty()) { ui.labelC(cx, y, 13, col::gray, hud.title); y += 22; }
        for (size_t i = 0; i < lines.size(); i++) ui.labelC(cx, y + i * 23.0f, 17, col::white, lines[i]);
        ty += h + 8;
    }
    std::string pol = traffic::policeStatus();
    if (!pol.empty() && spawned) {
        bool flash = std::fmod(simTime, 0.8) < 0.4;
        float w = font.width(pol, 17) + 40;
        ui.geo.rrect(cx - w / 2, ty, w, 34, 12, Color{14, 17, 23, 190});
        ui.geo.disc(cx - w / 2 + 16, ty + 17, 5, flash ? Color{70, 120, 255, 255} : Color{255, 70, 60, 255});
        ui.labelC(cx + 6, ty + 23, 17, col::white, pol);
        ty += 42;
    }
    float my = H * 0.30f; // kurze Meldungen, hoechstens zwei
    int shown = 0;
    for (auto& m : hud.msgs) {
        if (shown++ >= 2) break;
        float w = font.width(m.text, 19) + 32;
        uint8_t a = (uint8_t)(255 * clampv(m.ttl, 0.0f, 1.0f));
        ui.geo.rrect(cx - w / 2, my - 22, w, 34, 11, Color{14, 17, 23, (uint8_t)(a * 0.62f)});
        ui.labelC(cx, my, 19, Color{236, 232, 220, a}, m.text);
        my += 40;
    }
    float ly = 14;
    if (hud.faultsOn && (hud.points > 0 || hud.lessonActive)) { // Fehlerpunkte nur im Pruefungsmodus oder in Fahrstunden
        snprintf(buf, sizeof buf, "Fehlerpunkte  %d", hud.points);
        float w = font.width(buf, 15) + 28, h = 32 + (calm ? 0 : (float)hud.faults.size() * 19.0f);
        ui.geo.rrect(14, ly, std::max(w, calm ? w : 300.0f), h, 11, PANEL);
        ui.label(28, ly + 22, 15, hud.points > 0 ? Color{240, 130, 110, 255} : col::white, buf);
        if (!calm) for (size_t i = 0; i < hud.faults.size(); i++) ui.label(28, ly + 44 + i * 19.0f, 13, col::gray, hud.faults[i]);
        ly += h + 8;
    }
    if (netplay::active()) {
        std::string s = netplay::status();
        ui.geo.rrect(14, ly, font.width(s, 13) + 24, 26, 9, PANEL);
        ui.label(26, ly + 18, 13, SOFT, s);
        ly += 34;
        for (const NetCar& c : netplay::cars()) { // Namensschilder nur in der Naehe
            const float* m = mainVP.m;
            Vec3 p = c.pos + Vec3{0, 1.7f, 0};
            float w = m[3] * p.x + m[7] * p.y + m[11] * p.z + m[15];
            if (!c.placed || w < 1 || w > 300) continue;
            float sx = ((m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12]) / w * 0.5f + 0.5f) * W, sy = (1 - ((m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13]) / w * 0.5f + 0.5f)) * H;
            float tw = font.width(c.name, 13) + 14;
            ui.geo.rrect(sx - tw / 2, sy - 18, tw, 22, 8, Color{14, 17, 23, 170});
            ui.labelC(sx, sy - 3, 13, col::white, c.name);
        }
        // Chat: letzte Nachrichten unten links, blenden nach 12 s aus
        auto& log = netplay::chatLog();
        float cy = H - 40;
        for (int i = (int)log.size() - 1, n = 0; i >= 0 && n < 5; i--, n++) {
            double age = simTime - log[i].t;
            if (age > 12 && !(typing == 4)) break;
            uint8_t a = (uint8_t)(255 * clampv((float)(14 - age), 0.0f, 1.0f));
            std::string t = log[i].name + ": " + log[i].text;
            ui.geo.rrect(14, cy - 16, font.width(t, 14) + 20, 22, 8, Color{14, 17, 23, (uint8_t)(a * 0.6f)});
            ui.label(24, cy, 14, Color{232, 232, 236, a}, t);
            cy -= 26;
        }
    }
    // Quellenangabe: dezent (Pflicht der Datenanbieter), ausgeblendet nach einigen Sekunden im Spiel
    if (spawned && !loading) econ::buildHUD();
    if (!spawned || simTime - spawnedAt < 12 || menuOpen) ui.label(14, H - 8, 10, Color{210, 210, 214, 120}, world.attribution());
    if (loading) {
        float w = 300;
        ui.geo.rrect(cx - w / 2, H * 0.5f - 26, w, 52, 14, Color{14, 17, 23, 200});
        ui.labelC(cx, H * 0.5f - 2, 19, col::white, "Welt wird geladen ...");
        snprintf(buf, sizeof buf, "%d Kacheln", world.pendingJobs());
        ui.labelC(cx, H * 0.5f + 17, 12, col::gray, buf);
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

// ---------- Kartenfragen: wenn die Daten widerspruechlich sind (z. B. Spurzahl wechselt), einmal nachfragen und dauerhaft merken ----------
static std::vector<MapConflict> mapQ;
static void buildMapQuestion() {
    if (!cfg.mapQuestions) { world.takeConflicts(); mapQ.clear(); return; } // Kartenfragen ausgeschaltet
    for (MapConflict& c : world.takeConflicts()) { mapQ.push_back(c); logMsg("Kartenfrage: %s, Spuren %d <-> %d", c.name.c_str(), c.lanesA, c.lanesB); }
    if (mapQ.empty() || !spawned || menuOpen || typing || worldmap::active() || editor::active()) return;
    static double nextAsk = 0;
    if (std::fabs(car.speed()) > 0.8f || simTime < nextAsk || simTime - spawnedAt < 6) return; // nur im Stand, selten (hoechstens alle 25 s)
    int best = -1;
    float bd = 300;
    for (size_t i = 0; i < mapQ.size(); i++) {
        float x, z;
        world.toLocal(mapQ[i].lat, mapQ[i].lon, x, z);
        float d = std::hypot(x - car.pos.x, z - car.pos.z);
        if (d < bd) { bd = d; best = (int)i; }
    }
    if (best < 0) return;
    MapConflict c = mapQ[best];
    float W = ui.W, H = ui.H, w = 380, x = W - w - 24, y = H * 0.30f;
    ui.panel(x, y, w, 196, Color{14, 17, 23, 225}, 14);
    ui.label(x + 18, y + 28, 15, col::accent, "Kartenfrage");
    std::string name = c.name.empty() ? "Diese Stra\xC3\x9F" "e" : c.name;
    ui.label(x + 18, y + 54, 16, col::white, name);
    ui.label(x + 18, y + 76, 13, col::gray, "Die Spurzahl wechselt hier von " + std::to_string(c.lanesA) + " auf " + std::to_string(c.lanesB) + ".");
    ui.label(x + 18, y + 94, 13, col::gray, "Wie viele Spuren stimmen? Die Antwort wird gespeichert.");
    if (ui.button(x + 18, y + 108, 110, 36, std::to_string(c.lanesA) + " Spuren", false, 16)) { nextAsk = simTime + 25; world.resolveConflict(c, c.lanesA); mapQ.erase(mapQ.begin() + best); return; }
    if (ui.button(x + 136, y + 108, 110, 36, std::to_string(c.lanesB) + " Spuren", false, 16)) { nextAsk = simTime + 25; world.resolveConflict(c, c.lanesB); mapQ.erase(mapQ.begin() + best); return; }
    if (ui.button(x + 18, y + 152, 228, 32, "So lassen, nicht mehr fragen", false, 14)) { nextAsk = simTime + 25; world.resolveConflict(c, 0); mapQ.erase(mapQ.begin() + best); return; }
}

// Vorschaubilder des Katalogs: je Eintrag ein eigenes 3D-Bild (hoechstens zwei neue pro Bild, wegen der Rechenzeit)
static void renderThumbs() {
    int key = carColorSet ? ((carColor[0] << 16) | (carColor[1] << 8) | carColor[2]) : -1;
    if (key != thumbColorKey) { for (auto& kv : thumbs) kv.second.destroy(); thumbs.clear(); thumbColorKey = key; }
    int budget = 2, count = carModelCount();
    for (const ThumbRect& r : thumbRects) {
        if (budget <= 0) break;
        auto it = thumbs.find(r.idx);
        if (it != thumbs.end() && it->second.fbo) continue;
        int model = -1, kind = 0;
        if (r.idx >= 1 && r.idx <= count) model = r.idx - 1;
        else if (r.idx > count) kind = r.idx - count;
        carThumbRender(thumbs[r.idx], model, kind);
        budget--;
    }
}
static void drawThumbs(const Mat4& mvp) {
    if (thumbRects.empty()) return;
    glDisable(GL_DEPTH_TEST);
    gfxMVP(mvp);
    gfxModel(identity());
    gfxBias(0);
    gfxMode(8);
    for (const ThumbRect& r : thumbRects) {
        auto it = thumbs.find(r.idx);
        if (it == thumbs.end() || !it->second.fbo) continue;
        Batch b;
        Color w{255, 255, 255, 255};
        Vec3 n{0, 0, 1};
        b.vert({r.x, r.y, 0}, n, 0, 1, w); b.vert({r.x + r.w, r.y, 0}, n, 1, 1, w); b.vert({r.x + r.w, r.y + r.h, 0}, n, 1, 0, w);
        b.vert({r.x, r.y, 0}, n, 0, 1, w); b.vert({r.x + r.w, r.y + r.h, 0}, n, 1, 0, w); b.vert({r.x, r.y + r.h, 0}, n, 0, 0, w);
        gfxBindTexture(it->second.tex);
        b.draw();
    }
    glEnable(GL_DEPTH_TEST);
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
                  : typing == 3 ? "Ort auf der Karte suchen: " + typed + "_"
                  : typing == 4 ? "Chat (Enter = senden, Esc = abbrechen): " + typed + "_" : statusLine;
    float w = font.width(s, 22) + 44;
    ui.geo.rrect(cx - w / 2, H * 0.66f - 30, w, 46, 12, Color{16, 18, 24, 240});
    ui.labelC(cx, H * 0.66f, 22, col::white, s);
}

static void beginTyping(int mode) { typing = mode; typed.clear(); statusLine.clear(); SDL_StartTextInput(win); }

void gameSearchOnMap() { beginTyping(3); }

// ---------- Hauptmenue / Pause / Einstellungen / Mehrspieler ----------
static std::vector<std::string> savedServers() {
    std::vector<std::string> out;
    FILE* f = fopen("servers.txt", "r");
    if (!f) return out;
    char line[200];
    while (fgets(line, sizeof line, f)) {
        std::string s = line;
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
        if (!s.empty() && s[0] != '#') out.push_back(s);
    }
    fclose(f);
    return out;
}
static void rememberServer(const std::string& addr) {
    for (const std::string& s : savedServers()) if (s == addr) return;
    FILE* f = fopen("servers.txt", "a");
    if (f) { fprintf(f, "%s\n", addr.c_str()); fclose(f); }
}

static void closeMenu() { menuOpen = false; started = true; menuPage = 0; }

static void buildMenu() {
    float W = ui.W, H = ui.H;
    if (menuPage != 3) ui.geo.rect(0, 0, W, H, Color{4, 6, 10, started ? (uint8_t)150 : (uint8_t)70});
    float pw = (menuPage == 2 || menuPage == 4) ? 820.0f : (menuPage == 3 ? 480.0f : 440.0f);
    ui.geo.rect(0, 0, pw + 64, H, Color{8, 10, 15, 215});
    ui.geo.rect(pw + 64, 0, 2, H, Color{255, 184, 48, 90});
    const float x = 64;
    ui.label(x, 118, 54, col::white, "worldridr");
    ui.label(x, 148, 16, col::gray, menuPage == 1 ? "Mehrspieler" : menuPage == 2 ? "Einstellungen" : menuPage == 3 ? "Autokatalog" : "Fahrschule auf echten Karten");
    float by = 190, bw = 360;
    auto item = [&](const std::string& label, bool active = false, float bx = -1, float w = -1) {
        return ui.button(bx < 0 ? x : bx, by, w < 0 ? bw : w, 42, label, active, 17);
    };
    auto next = [&]() { by += 52; };
    if (menuPage == 0) {
        if (item(started ? "Weiterfahren" : "Spielen", !started)) closeMenu();
        next();
        if (item("Fahrstunden")) { closeMenu(); if (editor::active()) editor::close(); script.event("lesson", "0"); }
        next();
        if (item("Weltkarte und Navigation")) { closeMenu(); if (editor::active()) editor::close(); worldmap::open(); }
        next();
        if (item("Mehrspieler")) menuPage = 1;
        next();
        if (item("Autokatalog")) menuPage = 3;
        next();
        if (item("Einstellungen")) menuPage = 2;
        next();
        if (spawned) {
            if (item(editor::active() ? "Studio verlassen" : "Studio: bauen und Stra\xC3\x9F" "en legen")) {
                closeMenu();
                if (editor::active()) editor::close();
                else { Vec3 f = car.forward(); editor::open(car.pos, std::atan2(-f.x, -f.z)); }
            }
            next();
        }
        if (nav::active()) {
            if (item("Navigation beenden")) { nav::cancel(); closeMenu(); }
            next();
        }
        if (item("Beenden")) running = false;
        if (!spawned) ui.label(x, H - 66, 14, Color{255, 200, 120, 255}, "Welt wird geladen ...");
        ui.label(x, H - 40, 12, col::gray, "Esc  Men\xC3\xBC     M  Weltkarte     N  Minimap     F2  Studio     Eingabetaste  Chat");
        ui.label(x, H - 20, 10, Color{180, 184, 192, 120}, world.attribution());
    } else if (menuPage == 1) {
        ui.label(x, by - 8, 14, col::gray, netplay::status());
        by += 14;
        if (item("Spiel hosten", netplay::hosting())) netplay::host(cfg.port);
        next();
        if (item("Adresse eingeben und beitreten")) { closeMenu(); beginTyping(2); }
        next();
        if (item("LAN-Server suchen")) netplay::discover();
        next();
        std::vector<LanServer> lan = netplay::lanServers();
        for (const LanServer& s : lan) {
            if (by > H - 170) break;
            if (item(s.name + "  (" + std::to_string(s.players) + ")  " + s.address)) { netplay::join(s.address, cfg.port); rememberServer(s.address); }
            next();
        }
        int shown = 0;
        for (const std::string& s : savedServers()) {
            if (shown++ >= 3 || by > H - 170) break;
            if (item("Gespeichert: " + s)) netplay::join(s, cfg.port);
            next();
        }
        if (netplay::active()) {
            if (item("Trennen")) netplay::stop();
            next();
        }
        if (item("Zur\xC3\xBC" "ck")) menuPage = 0;
        if (netplay::active()) {
            float px = pw + 100;
            ui.panel(px, 190, 300, 40 + netplay::cars().size() * 26.0f, PANEL, 12);
            ui.label(px + 16, 216, 15, col::white, "Mitspieler");
            float py = 244;
            for (const NetCar& c : netplay::cars()) { ui.label(px + 16, py, 14, SOFT, c.name); py += 26; }
        }
        ui.label(x, H - 40, 12, col::gray, "Server ohne Fenster:  worldridr.exe --server  (siehe README)");
    } else if (menuPage == 3) {
        // ---------- Autokatalog: Liste, Farbauswahl; die 3D-Vorschau wird rechts daneben gezeichnet ----------
        static float lastX = 0;
        thumbRects.clear();
        if (ui.down && ui.mx > pw + 70 && !ui.captured) previewYawOff += (ui.mx - lastX) * 0.012f;
        lastX = ui.mx;
        int count = carModelCount();
        auto pretty = [](std::string s) {
            for (char& c : s) if (c == '-' || c == '_') c = ' ';
            if (!s.empty()) s[0] = (char)toupper((unsigned char)s[0]);
            return s;
        };
        int entries = count + 3; // Eigenbau + Autos + Moped + Motorrad
        bool two = entries > 9;
        float bwid = two ? 196.0f : 400.0f;
        float ly = 172;
        for (int idx = 0; idx < entries; idx++) {
            int col2 = two ? idx % 2 : 0, row = two ? idx / 2 : idx;
            float bx = x + col2 * (bwid + 8), byy = ly + row * 40;
            float old = by;
            by = byy;
            std::string label;
            bool active;
            if (idx <= count) { int ci = idx - 1; label = ci < 0 ? "Eigenbau" : pretty(carModelName(ci)); active = vehKind == 0 && carModelSel == ci; }
            else { label = idx == count + 1 ? "Derbi Senda (Moped)" : "Motorrad 125"; active = vehKind == idx - count; }
            if (item("", active, bx, bwid)) {
                int oldKind = vehKind;
                if (idx <= count) { vehKind = 0; carModelSel = idx - 1; } else vehKind = idx - count;
                if (vehKind != oldKind) { applyVehicle(); if (spawned) spawnCar(true); }
                saveSettings();
            }
            thumbRects.push_back({idx, bx + 6, byy + 3, 64.0f, 36.0f});
            { std::string shown = label; if (shown == "Derbi Senda (Moped)") shown = "Derbi Senda"; ui.label(bx + 78, byy + 26, 14, active ? Color{20, 16, 6, 255} : col::white, shown); }
            by = old;
        }
        float cy0 = ly + ((entries + (two ? 1 : 0)) / (two ? 2 : 1)) * 40 + 14;
        ui.label(x, cy0 + 4, 14, col::gray, "Farbe");
        static const uint8_t pal[15][3] = {{200, 30, 40}, {230, 110, 30}, {235, 190, 30}, {40, 140, 80}, {30, 150, 150}, {40, 90, 190}, {25, 45, 100}, {120, 50, 150},
                                           {220, 100, 160}, {235, 235, 238}, {170, 174, 182}, {100, 104, 112}, {28, 30, 34}, {120, 80, 50}, {210, 190, 150}};
        for (int i = 0; i < 16; i++) {
            float sx = x + (i % 8) * 46, sy = cy0 + 14 + (i / 8) * 46;
            bool orig = i == 0;
            Color c = orig ? Color{70, 76, 90, 255} : Color{pal[i - 1][0], pal[i - 1][1], pal[i - 1][2], 255};
            bool sel = orig ? !carColorSet : (carColorSet && carColor[0] == pal[i - 1][0] && carColor[1] == pal[i - 1][1] && carColor[2] == pal[i - 1][2]);
            bool hot = ui.over(sx, sy, 38, 38);
            if (hot) ui.captured = true;
            if (sel) ui.geo.rrect(sx - 3, sy - 3, 44, 44, 10, Color{255, 255, 255, 255});
            ui.geo.rrect(sx, sy, 38, 38, 8, hot ? Color{(uint8_t)std::min(255, c.r + 30), (uint8_t)std::min(255, c.g + 30), (uint8_t)std::min(255, c.b + 30), 255} : c);
            if (orig) ui.labelC(sx + 19, sy + 25, 13, col::white, "Orig.");
            if (hot && ui.clicked) {
                ui.clicked = false;
                carColorSet = !orig;
                if (!orig) { carColor[0] = pal[i - 1][0]; carColor[1] = pal[i - 1][1]; carColor[2] = pal[i - 1][2]; }
                saveSettings();
            }
        }
        by = cy0 + 14 + 2 * 46 + 6;
        if (item("Fertig")) { menuPage = 0; saveSettings(); }
        // Beschriftung der Vorschau
        float px = pw + 100;
        ui.label(px, 70, 30, col::white, vehKind == 1 ? "Derbi Senda (Moped)" : vehKind == 2 ? "Motorrad 125" : (carModelSel >= 0 ? pretty(carModelName(carModelSel)) : "Eigenbau"));
        std::string info;
        if (vehKind == 1) ui.label(px, 96, 14, col::gray, "50 ccm · 6 Gänge · ca. 50 km/h · Tank 6,5 l");
        else if (vehKind == 2) ui.label(px, 96, 14, col::gray, "125 ccm · 6 Gänge · ca. 100 km/h · Tank 12 l");
        else if (carModelSel >= 0 && carModelInfo(carModelSel, info)) ui.label(px, 96, 14, col::gray, info);
        else if (carModelSel < 0) ui.label(px, 96, 14, col::gray, "Im Spiel erzeugtes Modell");
        ui.label(px, H - 28, 13, Color{150, 156, 166, 200}, "Mit der Maus ziehen = drehen");
    } else if (menuPage == 4) {
        // ---------- Tastenbelegung ----------
        ui.label(x, 166, 14, col::gray, keyListen >= 0 ? "Neue Taste dr\xC3\xBC" "cken ...  (Esc = abbrechen)" : "Auf eine Taste klicken, dann die neue Taste dr\xC3\xBC" "cken. Pfeiltasten und Bild auf/ab bleiben als Zweittasten.");
        const int perCol = 9;
        for (int i = 0; i < A_COUNT; i++) {
            int c2 = i / perCol, row = i % perCol;
            float bx = x + c2 * 272, byy = 190 + row * 44;
            ui.label(bx, byy + 26, 13, SOFT, keys::label((Act)i));
            bool lis = keyListen == i;
            float old = by;
            by = byy;
            if (item(lis ? std::string("...") : keys::name((Act)i), lis, bx + 142, 118)) keyListen = i;
            by = old;
        }
        by = 190 + perCol * 44 + 12;
        if (item("Alle auf Standard", false, x, 260)) { keys::resetAll(); saveSettings(); }
        { float old = by; if (item("Fertig", false, x + 272, 260)) { menuPage = 2; keyListen = -1; } by = old; }
    } else {
        // Einstellungen in zwei Spalten
        float x2 = x + 380;
        auto left = [&](const std::string& label, bool active = false) { return item(label, active, x, 360); };
        auto right = [&](const std::string& label, float ry, bool active = false) {
            float old = by;
            by = ry;
            bool h = item(label, active, x2, 360);
            by = old;
            return h;
        };
        static const char* qn[3] = {"Niedrig", "Mittel", "Hoch"};
        bool changed = false;
        float ry = by;
        if (left(std::string("Grafik: ") + qn[cfg.quality])) { presetValues((cfg.quality + 1) % 3); applyQualityGL(); changed = true; }
        next();
        {
            static const float steps[5] = {0.5f, 0.6f, 0.7f, 0.85f, 1.0f};
            int cur = 0;
            for (int i = 0; i < 5; i++) if (std::fabs(steps[i] - cfg.renderScale) < 0.03f) cur = i;
            char b[48];
            snprintf(b, sizeof b, "Aufl\xC3\xB6sung: %d %%", (int)std::lround(steps[cur] * 100));
            if (left(b)) { cfg.renderScale = steps[(cur + 1) % 5]; changed = true; }
        }
        next();
        if (left(cfg.mirrors ? "Spiegel: an" : "Spiegel: aus")) { cfg.mirrors = !cfg.mirrors; changed = true; }
        next();
        {
            const char* n = cfg.bldDist < 600 ? "Kurz" : cfg.bldDist < 1100 ? "Mittel" : "Weit";
            if (left(std::string("Sichtweite: ") + n)) {
                if (cfg.bldDist < 600) { cfg.bldDist = 800; cfg.roadDist = 1000; }
                else if (cfg.bldDist < 1100) { cfg.bldDist = 1500; cfg.roadDist = 1800; }
                else { cfg.bldDist = 450; cfg.roadDist = 600; }
                changed = true;
            }
        }
        next();
        {
            static const int steps[6] = {0, 4, 8, 12, 18, 24};
            int cur = 0;
            for (int i = 0; i < 6; i++) if (steps[i] <= cfg.traffic) cur = i;
            if (left(cfg.traffic == 0 ? std::string("Verkehr: aus") : "Verkehr: " + std::to_string(cfg.traffic) + " Fahrzeuge")) {
                cfg.traffic = steps[(cur + 1) % 6];
                traffic::wanted = cfg.traffic;
                changed = true;
            }
        }
        next();
        if (left(cfg.msaa ? "Kantengl\xC3\xA4ttung: an (nach Neustart)" : "Kantengl\xC3\xA4ttung: aus")) { cfg.msaa = !cfg.msaa; changed = true; }
        next();
        if (left(cfg.mapQuestions ? "Kartenfragen: an" : "Kartenfragen: aus")) { cfg.mapQuestions = !cfg.mapQuestions; changed = true; }
        next();
        if (left(cfg.diesel ? "Motor: Diesel (Turbo)" : "Motor: Benzin (Turbo)")) { cfg.diesel = !cfg.diesel; car.setEngine(cfg.diesel ? 1 : 0); changed = true; }
        next();
        {
            static const float presets[6] = {-1.0f, 6.5f, 12.0f, 17.5f, 19.8f, 23.0f};
            static const char* pn[6] = {"Jetzt", "Morgen", "Mittag", "Abend", "DÃ¤mmerung", "Nacht"};
            int cur = -1;
            for (int i = 1; i < 6; i++) if (std::fabs(presets[i] - envir::hour) < 0.26f) cur = i;
            char b[64];
            int hh = (int)envir::hour, mm = (int)((envir::hour - hh) * 60);
            snprintf(b, sizeof b, "Tageszeit: %02d:%02d%s%s", hh, mm, cur > 0 ? "  " : "", cur > 0 ? pn[cur] : "");
            if (left(b)) {
                int nx = cur < 0 ? 1 : cur + 1;
                envir::hour = nx >= 6 ? envir::systemHour() : presets[nx];
                changed = true;
            }
        }
        next();
        if (left(std::string("Wetter: ") + envir::weatherName[envir::weather])) { envir::weather = (envir::weather + 1) % 6; changed = true; }
        next();
        float leftEnd = by;
        if (right(cfg.police ? "Polizei: an" : "Polizei: aus", ry)) { cfg.police = !cfg.police; changed = true; }
        ry += 52;
        {
            static const char* sn[4] = {"Uhr steht", "Echtzeit", "30-fach", "120-fach"};
            if (right(std::string("Zeitablauf: ") + sn[envir::speedIdx], ry)) { envir::speedIdx = (envir::speedIdx + 1) % 4; changed = true; }
            ry += 52;
        }
        {
            static const int steps[5] = {0, 5, 10, 20, 30};
            int cur = 0;
            for (int i = 0; i < 5; i++) if (steps[i] <= cfg.peds) cur = i;
            if (right(cfg.peds == 0 ? std::string("Fu\xC3\x9Fg\xC3\xA4nger: aus") : "Fu\xC3\x9Fg\xC3\xA4nger: " + std::to_string(cfg.peds), ry)) { cfg.peds = steps[(cur + 1) % 5]; ped::wanted = cfg.peds; changed = true; }
            ry += 52;
        }
        if (right(cfg.faultPoints ? "Fehlerpunkte: an" : "Fehlerpunkte: aus", ry)) { cfg.faultPoints = !cfg.faultPoints; changed = true; }
        ry += 52;
        if (right(cfg.hudMode == 0 ? "Anzeige: ruhig" : "Anzeige: ausf\xC3\xBC" "hrlich", ry)) { cfg.hudMode = 1 - cfg.hudMode; changed = true; }
        ry += 52;
        if (right(cfg.minimap ? "Minimap: an" : "Minimap: aus", ry)) { cfg.minimap = !cfg.minimap; changed = true; }
        ry += 52;
        if (right(cfg.automatic ? "Getriebe: Automatik" : "Getriebe: Schaltung", ry)) { cfg.automatic = !cfg.automatic; changed = true; }
        ry += 52;
        if (right(camMode == 0 ? "Kamera: Cockpit" : "Kamera: au\xC3\x9F" "en", ry)) { camMode = (camMode + 1) % 2; changed = true; }
        ry += 52;
        if (right("Auto: " + carModelName(carModelSel) + "  (Katalog)", ry)) menuPage = 3;
        ry += 52;
        if (right(input.shifterReady() ? "Schalthebel neu einrichten" : "Schalthebel einrichten", ry)) { closeMenu(); startShifterSetup(); }
        by = std::max(leftEnd, ry + 52) + 8;
        if (item("Zur\xC3\xBC" "ck")) menuPage = 0;
        { float old = by; if (item("Tasten belegen", false, x2, 360)) menuPage = 4; by = old; }
        if (changed) saveSettings();
        ui.label(x, H - 24, 12, col::gray, "Niedrig = schnellste Darstellung f\xC3\xBCr schwache Rechner. \xC3\x84nderungen werden automatisch gespeichert.");
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
            if (c.type == 0) { int s = World::signalState(c, simTime); if (s == 0 || s == 1) { script.event("red_light"); traffic::offence(0); } }
            else if (c.type == 1 && !t.stopped) { script.event("stop_sign"); traffic::offence(1); }
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
    bool haveSettings = loadSettings();
    double shotAt = -1;
    std::string shotPath, startLesson, joinAddr;
    bool startEditor = false, startHost = false, showMenu = true, startMap = false, testTp = false;
    double testTpLat = 0, testTpLon = 0, destLat = 0, destLon = 0;
    bool edCamPending = false;
    bool driveTest = false;
    float edCam[5] = {};
    if (const char* ec = std::getenv("WR_EDCAM")) edCamPending = sscanf(ec, "%f,%f,%f,%f,%f", &edCam[0], &edCam[1], &edCam[2], &edCam[3], &edCam[4]) == 5;
    bool haveDest = false;
    bool revTest = false;
    int serverMax = 16;
    bool policeTest = false, pedShow = false;
    int footTest = 0;
    int poiTest = 0;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--vr") cfg.vr = true;
        else if (a == "--server") cfg.server = true;
        else if (a == "--page" && i + 1 < argc) menuPage = atoi(argv[++i]);
        else if (a == "--policetest") policeTest = true;
        else if (a == "--pedshow") pedShow = true;
        else if (a == "--foottest") footTest = 1;
        else if (a == "--poitest") poiTest = 1;
        else if (a == "--port" && i + 1 < argc) cfg.port = atoi(argv[++i]);
        else if (a == "--max" && i + 1 < argc) serverMax = atoi(argv[++i]);
        else if (a == "--autodrive") { autodrive = true; showMenu = false; }
        else if (a == "--chase") camMode = 1;
        else if (a == "--cockpit") camMode = 0;
        else if (a == "--editor") { startEditor = true; showMenu = false; }
        else if (a == "--host") startHost = true;
        else if (a == "--nomenu") showMenu = false;
        else if (a == "--map") { startMap = true; showMenu = false; }
        else if (a == "--drivetest") { driveTest = true; autodrive = true; showMenu = false; cfg.traffic = 0; }
        else if (a == "--revtest") { revTest = true; showMenu = false; cfg.traffic = 0; }
        else if (a == "--dest" && i + 2 < argc) { destLat = atof(argv[i + 1]); destLon = atof(argv[i + 2]); haveDest = true; i += 2; }
        else if (a == "--tp" && i + 2 < argc) { testTpLat = atof(argv[i + 1]); testTpLon = atof(argv[i + 2]); testTp = true; i += 2; } // Selbsttest: nach 4 s teleportieren
        else if (a == "--traffic" && i + 1 < argc) cfg.traffic = atoi(argv[++i]);
        else if (a == "--join" && i + 1 < argc) joinAddr = argv[++i];
        else if (a == "--name" && i + 1 < argc) cfg.playerName = argv[++i];
        else if (a == "--place" && i + 1 < argc) { cfg.startPlace = argv[++i]; cfg.startAuto = false; }
        else if (a == "--lesson" && i + 1 < argc) { startLesson = argv[++i]; showMenu = false; }
        else if (a == "--shot" && i + 2 < argc) { shotAt = atof(argv[i + 1]); shotPath = argv[i + 2]; i += 2; }
    }

    if (cfg.server) { // eigenstaendiger Server: kein Fenster, keine Grafik - verteilt nur Positionen und Chat
        std::string sname = cfg.playerName == "Fahrer" ? "worldridr Server" : cfg.playerName;
        if (!netplay::server(cfg.port, sname, serverMax)) return 1;
        logMsg("Server laeuft. Beenden mit Strg+C. Spieler treten mit der Adresse oben bei (UDP-Port %d freigeben).", cfg.port);
        Uint64 lastT = SDL_GetPerformanceCounter();
        double lastReport = 0;
        for (;;) {
            Uint64 nowT = SDL_GetPerformanceCounter();
            float dt = (float)((double)(nowT - lastT) / (double)SDL_GetPerformanceFrequency());
            lastT = nowT;
            simTime += dt;
            netplay::serverUpdate(dt);
            if (simTime - lastReport > 60) {
                lastReport = simTime;
                logMsg("Server: %d Spieler online", (int)netplay::cars().size());
            }
            SDL_Delay(8);
        }
    }
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_JOYSTICK | SDL_INIT_HAPTIC)) { logMsg("SDL_Init: %s", SDL_GetError()); return 1; }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    if (!cfg.vr && cfg.msaa) { SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 1); SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 4); }
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
    if (cfg.startAuto && cfg.startPlace.empty()) { // Standard: Startort aus der IP-Adresse
        double la, lo;
        std::string city;
        if (ipLocate(la, lo, city)) { cfg.lat = la; cfg.lon = lo; logMsg("Startort per IP-Adresse: %s (%.3f, %.3f)", city.c_str(), la, lo); }
        else logMsg("IP-Standort nicht verfügbar - verwende Standardort");
    }
    if (!cfg.startPlace.empty()) {
        double lat, lon;
        if (geocode(cfg.startPlace, lat, lon)) { cfg.lat = lat; cfg.lon = lon; }
        else logMsg("Startort \"%s\" nicht gefunden - verwende Standardkoordinaten", cfg.startPlace.c_str());
    }
    world.setOrigin(cfg.lat, cfg.lon);
    applyQualityGL();
    applyVehicle();
    (void)haveSettings;
    editor::init();
    forest::init();
    script.init(&hud);
    if (cfg.vr) vr.init();
    if (startHost) netplay::host(cfg.port);
    if (!joinAddr.empty()) netplay::join(joinAddr, cfg.port);
    menuOpen = showMenu;
    started = !showMenu;
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
            if (keyListen >= 0 && e.type == SDL_EVENT_KEY_DOWN) { if (e.key.key != SDLK_ESCAPE) { keys::set((Act)keyListen, e.key.key); saveSettings(); } keyListen = -1; continue; }
            if (e.type == SDL_EVENT_TEXT_INPUT && typing) { typed += e.text.text; continue; }
            if (e.type == SDL_EVENT_KEY_DOWN && typing) {
                SDL_Keycode k = e.key.key;
                if (k == SDLK_ESCAPE) { typing = 0; SDL_StopTextInput(win); }
                else if (k == SDLK_BACKSPACE) { while (!typed.empty()) { unsigned char ch = typed.back(); typed.pop_back(); if ((ch & 0xC0) != 0x80) break; } }
                else if (k == SDLK_RETURN || k == SDLK_KP_ENTER) {
                    int mode = typing;
                    typing = 0; SDL_StopTextInput(win);
                    if (!typed.empty() && mode == 1) startTeleport(typed);
                    if (!typed.empty() && mode == 2) { if (netplay::join(typed, cfg.port)) rememberServer(typed); else statusLine = netplay::status(); }
                    if (!typed.empty() && mode == 3) startTeleport(typed, true);
                    if (!typed.empty() && mode == 4) netplay::chat(typed);
                }
                continue;
            }
            if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) {
                SDL_Keycode k = e.key.key;
                if (k == SDLK_ESCAPE && calStep >= 0) { calStep = -1; statusLine.clear(); continue; }
                if (k == SDLK_ESCAPE && worldmap::active()) { worldmap::close(); statusLine.clear(); continue; }
                if (k == SDLK_ESCAPE && edit && !menuOpen && editor::escape()) continue;
                if (k == SDLK_ESCAPE) { menuOpen = !menuOpen; menuPage = 0; statusLine.clear(); if (!menuOpen) started = true; continue; }
                if (keys::is(A_CHAT, k) && !menuOpen && !edit && !worldmap::active() && netplay::active()) { beginTyping(4); continue; }
                if (keys::is(A_MAP, k) && !edit && !menuOpen) { if (worldmap::active()) worldmap::close(); else worldmap::open(); continue; }
                if (k == SDLK_P) { wantShot = true; continue; }
                if (k == SDLK_F2 && spawned) { if (edit) editor::close(); else { Vec3 f = car.forward(); editor::open(car.pos, std::atan2(-f.x, -f.z)); } continue; }
            }
            if (menuOpen) continue;
            if (worldmap::active()) { worldmap::event(e); continue; }
            if (edit) { editor::event(e); continue; }
            input.handleEvent(e, false);
            if (e.type == SDL_EVENT_MOUSE_MOTION && (e.motion.state & SDL_BUTTON_RMASK)) {
                if (foot::on) foot::camYaw -= e.motion.xrel * 0.004f;
                lookYaw = clampv(lookYaw - e.motion.xrel * 0.004f, -2.4f, 2.4f);
                lookPitch = clampv(lookPitch - e.motion.yrel * 0.004f, -1.0f, 1.0f);
            } else if (e.type == SDL_EVENT_MOUSE_BUTTON_UP && e.button.button == SDL_BUTTON_RIGHT) { lookYaw = lookPitch = 0; }
            else if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) {
                SDL_Keycode k = e.key.key;
                if (keys::is(A_PLACE, k)) beginTyping(1);
                else if (k == SDLK_F3) debugOverlay = !debugOverlay;
                else if (keys::is(A_MINIMAP, k)) nav::zoomCycle();
                else if (keys::is(A_EXIT, k)) { if (foot::on) foot::enterCar(); else foot::exitCar(); }
                else if (keys::is(A_LOCK, k)) foot::toggleLock();
                else if (keys::is(A_ACTION, k)) econ::keyJ();
                else if (keys::is(A_FUELNAV, k)) econ::navToNearest(0);                else if (k == SDLK_F5) vr.recenter();
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
            if (std::getenv("WR_EDCAM")) edCamPending = true;
        } else if (tpState == 3) { statusLine = "Ort nicht gefunden"; tpState = 0; }
        else if (tpState == 4) { // Suchergebnis nur auf der Karte zeigen
            std::lock_guard<std::mutex> lk(tpMutex);
            worldmap::setCenter(tpLat, tpLon, 14);
            statusLine.clear(); tpState = 0;
        }

        bool driving = spawned && !edit;
        if (testTp && spawned && simTime - spawnTime > 4) { testTp = false; gameTeleport(testTpLat, testTpLon); }
        bool mapOpen = worldmap::active();
        bool controls = driving && !menuOpen && !typing && !mapOpen && !foot::on;
        input.update(dt, car.speed(), !controls);
        if (spawned) foot::update(dt);
        updateShifterSetup(dt);
        netplay::pollDiscovery();
        Vec3 focus = edit ? editor::cameraPos() : (spawned ? playerPos() : Vec3{});
        world.update(focus, simTime);
        editor::refresh(simTime);
        if (edCamPending && editor::active() && spawned) { // Test: Studio-Kamera relativ zum Gelaende platzieren (WR_EDCAM="x,hoehe,z,gieren,nicken")
            float gh;
            if (world.height(edCam[0], edCam[2], gh)) { editor::setCamera({edCam[0], gh + edCam[1], edCam[2]}, edCam[3], edCam[4]); edCamPending = false; }
        }
        // Strassenbezug jedes Bild neu bestimmen: Zeiger in nachgeladene/entfernte Kacheln duerfen nicht ueberleben
        roadHit = spawned ? world.nearestRoad(car.pos.x, car.pos.z, 400, 7, car.pos.y - 0.55f) : RoadHit{};
        if (roadHit.found && roadHit.dist > 60) roadHit.found = false;
        bool ready =world.ready(spawned ? car.pos : Vec3{}, simTime);
        if (!spawned && ready) {
            spawnCar(false);
            spawned = true; spawnTime = simTime; spawnedAt = simTime;
            script.event("spawn");
            if (!startLesson.empty()) { script.event("key", "f1"); script.event("key", startLesson.c_str()); startLesson.clear(); }
            if (startMap) { worldmap::open(); startMap = false; }
            if (haveDest) { nav::setDestination(destLat, destLon); haveDest = false; }
            if (startEditor) { // Selbsttest: Uebersicht von oben auf den Startpunkt
                editor::open(car.pos, 0);
                editor::setCamera({10, car.pos.y + 45, 55}, 0, -0.62f);
                if (const char* hy = getenv("WR_CAMY")) editor::setCamera({10, car.pos.y + (float)atof(hy), 55}, 0, -0.3f);
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
        bool cockpit = camMode == 0 && !edit && !foot::on;
        ui.begin(uiW, uiH, mxp * uiH / wh, myp * uiH / wh, (mb & SDL_BUTTON_LMASK) != 0, uiClick);
        if (mapOpen && !menuOpen) worldmap::buildUI();
        else if (!edit && !menuOpen && foot::fade() < 0.5f) { buildHUD(cockpit, !spawned); if (spawned) nav::buildHUD(); buildMapQuestion(); }
        if (edit && !menuOpen) editor::buildUI();
        buildPrompt();
        if (foot::fade() > 0.01f) ui.geo.rect(0, 0, uiW, uiH, Color{0, 0, 0, (uint8_t)(255.0f * foot::fade())});
        if (menuOpen) buildMenu();

        // ---------- Fahren ----------
        if (driving) {
            car.automatic = cfg.automatic;
            car.autoClutch = cfg.autoClutch == -1 ? !input.wheelPresent : cfg.autoClutch == 1;
            car.steer = input.steer; car.throttle = input.throttle; car.brake = input.brake; car.clutch = input.clutch;
            if (foot::on) { car.throttle = 0; car.brake = 0.35f; car.steer = 0; car.clutch = 0; car.handbrake = true; car.engineOn = false; car.starter = false; }
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
            if (input.reset || script.wantReset) {
                if (car.seized || car.health < 1.0f) { car.repairEngine(); hud.msgs.push_back({"Motor repariert", 4.0f}); }
                spawnCar(true);
                script.wantReset = false;
            }

            roadHit = world.nearestRoad(car.pos.x, car.pos.z, 400, 7, car.pos.y - 0.55f);
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
                bool dtHold = driveTest && simTime - spawnTime >= 5.0;
                if (car.engineOn && !car.automatic && !dtHold) {
                    if (car.gear < 1) car.requestGear(1);
                    else if (car.rpm() > (driveTest ? 2400 : 3000) && car.gear < (driveTest ? 5 : 4) && simTime - lastAutoShift > (driveTest ? 1.2 : 1.5)) { car.shift(1); lastAutoShift = simTime; }
                }
                if (car.automatic) car.selector = 3;
                car.throttle = car.speed() < (driveTest ? 22 : 11) ? 0.45f : 0.08f;
                car.brake = 0;
                if (driveTest) { // Selbsttest Motorbremse + Money-Shift: Fahrzeug gezielt auf Tempo setzen und im Schub rollen lassen
                    double t = simTime - spawnTime;
                    static double lastDt = 0;
                    static int phase = -1;
                    static const float RAT[6] = {3.78f, 2.12f, 1.36f, 1.03f, 0.84f, 0.70f};
                    auto launch = [&](float v, int g) {
                        Vec3 fw = car.forward(); fw.y = 0; fw = normalize(fw);
                        car.vel = fw * v;
                        for (Wheel& wh : car.w) wh.omega = v / Vehicle::wheelRadius();
                        car.gear = g; car.engW = v / Vehicle::wheelRadius() * RAT[g - 1] * 3.65f; car.engineOn = true; car.clutch = 0;
                    };
                    car.autoClutch = false; car.throttle = 0; car.brake = 0;
                    if (t >= 5.0 && phase < 0) { phase = 0; launch(27.0f, 5); logMsg("drivetest --- Phase 1: 5. Gang, Schub aus 97 km/h"); }
                    if (t >= 9.0 && phase == 0) { phase = 1; launch(20.0f, 3); logMsg("drivetest --- Phase 2: 3. Gang, Schub aus 72 km/h"); }
                    if (t >= 13.0 && phase == 1) { phase = 2; launch(25.0f, 5); logMsg("drivetest --- Phase 3: 5. Gang, jetzt Kupplung treten und in den 2. Gang"); }
                    if (phase == 2) {
                        if (t >= 14.0 && t < 14.5) { car.clutch = 1.0f; if (car.gear != 2) car.requestGear(2); }
                        else if (t >= 14.5) car.clutch = 0.0f;
                    }
                    if (t >= 19.0 && phase == 2) { phase = 3; launch(36.0f, 6); logMsg("drivetest --- Phase 4: 6. Gang bei 130 km/h, Kupplung treten, 2. Gang, einkuppeln (echter Money-Shift)"); }
                    if (phase == 3) {
                        if (t >= 20.0 && t < 20.5) { car.clutch = 1.0f; if (car.gear != 2) car.requestGear(2); }
                        else if (t >= 20.5) car.clutch = 0.0f;
                    }
                    if (simTime - lastDt > (phase >= 2 ? 0.2 : 0.5) && phase >= 0 && (t < 15.0 || t > 19.0)) {
                        lastDt = simTime;
                        logMsg("drivetest t=%.1f v=%.1f km/h gang=%d rpm=%.0f kupplung=%.1f motor=%s gesundheit=%.2f", t, car.speed() * 3.6f, car.gear, car.rpm(), car.clutch,
                               car.seized ? "KAPUTT" : (car.engineOn ? "an" : "aus"), car.health);
                    }
                }
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
                if (autodrive) logMsg("Kollision mit Gebaeude/Objekt bei x=%.1f z=%.1f y=%.1f (Tempo %.1f)", car.pos.x, car.pos.z, car.pos.y, length(car.vel) * 3.6f);
                car.pos.x = prev.x; car.pos.z = prev.z;
                bool hard = length(car.vel) > 1.5f;
                car.vel = car.vel * -0.15f;
                if (hard && crashCooldown <= 0) { script.event("crash"); traffic::offence(2); econ::onCrash(); crashCooldown = 2.0f; }
            }
            if (car.evStall) script.event("stall");
            if (car.evGrind) { script.event("grind"); car.evGrind = false; }
            if (car.evOverrev) hud.msgs.push_back({"Drehzahl viel zu hoch - Motor wird besch\xC3\xA4" "digt!", 3.5f});
            if (car.evBlown) { hud.msgs.push_back({"MOTORSCHADEN (\xC3\x9C" "berdrehen)  -  Taste R: reparieren und zur\xC3\xBC" "ck auf die Stra\xC3\x9F" "e", 9.0f}); script.event("engine_blown"); }
            if (car.evNonFinite) { logMsg("Fahrzeugphysik: ungueltiger Zustand abgefangen"); }
            { // Dauerhinweise: Motorschaden bzw. Fahrzeug liegt auf der Seite/dem Dach
                static double lastHint = 0, flippedSince = -1;
                if (car.flipped && std::fabs(car.speed()) < 2.0f) { if (flippedSince < 0) flippedSince = simTime; } else flippedSince = -1;
                if (simTime - lastHint > 20.0 && (car.seized || (flippedSince >= 0 && simTime - flippedSince > 3.0))) {
                    lastHint = simTime;
                    hud.msgs.push_back({car.seized ? "Motorschaden - Taste R: reparieren und zur\xC3\xBC" "ck auf die Stra\xC3\x9F" "e" : "Fahrzeug liegt - Taste R: zur\xC3\xBC" "ck auf die Stra\xC3\x9F" "e", 6.0f});
                }
            }
            checkControls();
            envir::update(dt);
            traffic::update(dt);
            nav::update(dt);
            econ::update(dt);
            ped::update(dt);

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
            script.carNum("speed_limit", effectiveLimit());
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
            hud.faultsOn = cfg.faultPoints || hud.lessonActive;
            if (pedShow && simTime - spawnTime > 7) { ped::debugSpawnAhead(); pedShow = false; }
            if (footTest == 1 && simTime - spawnTime > 6) { foot::exitCar(); footTest = 2; }
            if (footTest == 2 && simTime - spawnTime > 8) { foot::debugHit(); footTest = 3; }
            if (poiTest == 1 && simTime - spawnTime > 7) { // Selbsttest: zur naechsten Tankstelle setzen, Tank fast leer, tanken
                float bd = 1e9f; Poi bp; bool fnd = false;
                world.forPoisNear(car.pos.x, car.pos.z, 4000, [&](const Poi& p) { float d = std::hypot(p.x - car.pos.x, p.z - car.pos.z); if (p.kind == 0 && d < bd) { bd = d; bp = p; fnd = true; } });
                if (fnd) { float yy = 0; world.surface(bp.x - bp.ux * 12, bp.z - bp.uz * 12, 1e9f, yy); car.place({bp.x - bp.ux * 12.0f, yy + 0.62f, bp.z - bp.uz * 12.0f}, std::atan2(-bp.ux, -bp.uz)); econ::fuel = 8; logMsg("Tankstelle: %.0f m entfernt, Preis %.2f, Marke %s", bd, bp.price, bp.name.c_str()); }
                poiTest = 2;
            }
            if (poiTest == 2 && simTime - spawnTime > 9) { econ::keyJ(); poiTest = 3; }
            if (policeTest && simTime - spawnTime > 8) { traffic::offence(2); traffic::offence(2); traffic::offence(0); policeTest = false; }
            script.update(dt);

            float load = car.engineOn ? clampv(car.throttle * 0.9f + 0.1f, 0.0f, 1.0f) : (car.starter ? 0.5f : 0.0f);
            audioRain(std::min(1.0f, envir::rainLevel()));
            audioEngine(4, car.engineType != 0);
            audioSet(car.rpm(), load, std::fabs(car.speed()), input.horn);
            audioSiren(traffic::sirenLevel());
            input.setFFB(car.ffb, dt);

            if (autodrive && simTime - lastTelemetry > 1.0) {
                lastTelemetry = simTime;
                float gh = 0;
                world.surface(car.pos.x, car.pos.z, car.pos.y, gh);
                logMsg("t=%.0f v=%.1f km/h rpm=%.0f gang=%d hoehe=%.2f strasse=%d spur=%+.1f mitspieler=%d fussgaenger=%d dist=%.1f bld=%d x=%.1f z=%.1f", simTime, car.speed() * 3.6f, car.rpm(), car.gear,
                       car.pos.y - gh, (int)onRoad, laneOffset, (int)netplay::cars().size(), ped::count(), roadHit.found ? roadHit.dist : -1.0f, (int)world.insideBuilding(car.pos.x, car.pos.z), car.pos.x, car.pos.z);
                { float th = 0; world.height(car.pos.x, car.pos.z, th);
                  logMsg("   Gelaende %.1f  Fahrzeug %.1f  Strasse: klass=%d ya=%.1f yb=%.1f", th, car.pos.y, roadHit.found ? (int)roadHit.seg->klass : -1, roadHit.found ? roadHit.seg->ya : 0.0f, roadHit.found ? roadHit.seg->yb : 0.0f); }
                for (const NetCar& c : netplay::cars())
                    logMsg("   Mitspieler %s: Abstand %.1f m (dx %.1f dy %.1f dz %.1f)", c.name.c_str(), length(c.pos - car.pos), c.pos.x - car.pos.x, c.pos.y - car.pos.y, c.pos.z - car.pos.z);
            }
        } else {
            audioSet(edit ? 0.0f : car.rpm(), 0.1f, 0, false);
            audioSiren(0);
            input.setFFB(0, dt);
        }
        for (auto it = hud.msgs.begin(); it != hud.msgs.end();) { it->ttl -= dt; it = it->ttl <= 0 ? hud.msgs.erase(it) : it + 1; }

        // ---------- Mehrspieler ----------
        if (netplay::active() && spawned) {
            NetCar me;
            me.name = cfg.playerName;
            world.toLatLon(car.pos.x, car.pos.z, me.lat, me.lon);
            me.y = car.pos.y; me.q = car.q; me.steer = car.w[0].steer; me.speed = car.speed(); me.wheelRot = car.w[0].rot;
            { CarVisual ov = ownVisual(); me.model = carModelSel; me.kind = car.kind; me.tinted = ov.tinted; me.paint[0] = ov.paint.r; me.paint[1] = ov.paint.g; me.paint[2] = ov.paint.b; }
            me.headYaw = ownHeadYaw; me.headPitch = ownHeadPitch;   // bei VR: echter Kopf, sonst Kamerablick
            me.flags = (uint8_t)((((blinker == -1 || blinker == 2) && blinkLit) ? 1 : 0) | (((blinker == 1 || blinker == 2) && blinkLit) ? 2 : 0) | (car.brake > 0.05f ? 4 : 0) | (lightsOn() ? 8 : 0));
            netplay::update(dt, me);
        }

        // ---------- Rendern ----------
        updateOwnOccupant(dt);
        Mat4 M = car.transform();
        buildProps(focus);
        forest::update(edit ? editor::cameraPos() : (spawned ? playerPos() : Vec3{}));
        bool vrFrame = !edit && vr.beginFrame();
        int wantInterval = vr.running ? 0 : 1;
        if (wantInterval != swapInterval) { SDL_GL_SetSwapInterval(wantInterval); swapInterval = wantInterval; }
        if (cfg.mirrors && (cockpit || vrFrame) && spawned && car.kind == 0) renderMirrors(M);

        bool drewWindow = false;
        // Menue, Texteingabe und Weltkarte lassen sich am PC-Fenster bedienen; im Headset steht nur ein Hinweis
        bool pcUi = vrFrame && (menuOpen || typing || worldmap::active());
        std::vector<Vertex> savedGeo, savedText;
        if (pcUi) {
            savedGeo.swap(ui.geo.v); savedText.swap(ui.text.v);
            ui.panel(0, 0, uiW, uiH, Color{0, 0, 0, 0}, 0);
            float cx2 = uiW * 0.5f, cy2 = uiH * 0.5f;
            ui.geo.rrect(cx2 - 330, cy2 - 90, 660, 180, 20, Color{16, 18, 24, 235});
            ui.labelC(cx2, cy2 - 20, 40, col::white, "Bitte auf den PC-Monitor schauen");
            ui.labelC(cx2, cy2 + 28, 22, col::gray, "Das MenÃ¼ wird am Computer mit der Maus bedient");
            ui.labelC(cx2, cy2 + 62, 18, col::gray, "Esc schlieÃt das MenÃ¼");
        }
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
                    if (i == 0 && !pcUi) {
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
        if (pcUi) { ui.geo.v.swap(savedGeo); ui.text.v.swap(savedText); } // eigentliches Menue fuer das PC-Fenster zurueck
        if (!drewWindow) {
            bool scaled = cfg.renderScale < 0.97f && !edit;
            int rw = scaled ? std::max(320, (int)(ww * cfg.renderScale)) : ww, rh = scaled ? std::max(180, (int)(wh * cfg.renderScale)) : wh;
            if (scaled) {
                if (sceneRT.w != rw || sceneRT.h != rh) sceneRT.create(rw, rh);
                glBindFramebuffer(GL_FRAMEBUFFER, sceneRT.fbo);
            } else glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glViewport(0, 0, rw, rh);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            Mat4 camM;
            if (edit) camM = editor::camera();
            else if (menuOpen && !started) { // Hauptmenue: langsame Kamerafahrt um den Startpunkt
                Vec3 c = spawned ? car.pos : Vec3{};
                float gh = c.y;
                if (!spawned) world.height(0, 0, gh);
                float a = (float)simTime * 0.06f;
                camM = mul(translate(c + Vec3{std::sin(a) * 16.0f, 0, std::cos(a) * 16.0f} + Vec3{0, spawned ? 3.5f : (gh - c.y + 30.0f), 0}), mul(rotY(a), rotX(-0.14f)));
            }
            else if (foot::on) camM = foot::camera();
            else if (cockpit) camM = mul(M, mul(translate(EYE), mul(rotY(lookYaw + input.lookYaw), rotX(lookPitch - 0.13f))));
            else {
                Vec3 f = car.forward();
                float yaw = std::atan2(-f.x, -f.z) + lookYaw;
                Vec3 back{std::sin(yaw), 0, std::cos(yaw)};
                camM = mul(translate(car.pos + back * 7.2f + Vec3{0, 2.2f, 0}), mul(rotY(yaw), rotX(-0.20f + lookPitch)));
            }
            Mat4 view = rigidInverse(camM), proj = perspective(cfg.fov * PI / 180.0f, aspect, 0.2f, 16000.0f);
            mainVP = mul(proj, view);
            if (edit && !menuOpen) editor::update(dt, mainVP, (float)ww, (float)wh);
            drawScene(view, proj, xformPoint(camM, {0, 0, 0}));
            if (spawned) drawOwnCar(mainVP, cockpit && !(menuOpen && !started));
            if (edit) editor::drawGizmos(mainVP);
            if (scaled) { // niedrigere Renderaufloesung auf Fenstergroesse hochskalieren; Oberflaeche bleibt scharf
                glBindFramebuffer(GL_READ_FRAMEBUFFER, sceneRT.fbo);
                glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
                glViewport(0, 0, ww, wh);
                glBlitFramebuffer(0, 0, rw, rh, 0, 0, ww, wh, GL_COLOR_BUFFER_BIT, GL_LINEAR);
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
            }
            if (menuOpen && menuPage == 3) { // Autokatalog: Vorschau rechts neben der Liste
                int x0 = (int)((480.0f + 66) * wh / uiH);
                carPreviewDraw(x0, 0, ww - x0, wh, (float)simTime * 0.45f + previewYawOff);
                glViewport(0, 0, ww, wh);
                renderThumbs();
                glViewport(0, 0, ww, wh);
            }
            if (worldmap::active()) worldmap::draw(ortho(0, uiW, uiH, 0), ww, wh);
            ui.flush(ortho(0, uiW, uiH, 0));
            if (menuOpen && menuPage == 3) drawThumbs(ortho(0, uiW, uiH, 0));
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
    saveSettings();
    netplay::stop();
    SDL_Quit();
    _exit(0); // Netzwerk-Threads nicht abwarten
}
