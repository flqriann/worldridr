#pragma once
// Gemeinsame Zustaende und Schnittstellen der Spielmodule (Automodell, Oberflaeche, Editor, Netzwerk)
#include <SDL3/SDL.h>
#include <functional>
#include "gfx.h"
#include "input.h"
#include "script.h"
#include "vehicle.h"
#include "vr.h"
#include "world.h"

extern Config cfg;
extern World world;
extern Vehicle car;
extern Input input;
extern Script script;
extern HudState hud;
extern VR vr;
extern Font font;
extern double simTime;

// ---------- Automodell ----------
struct CarVisual {
    Mat4 M;
    float steer = 0;            // Radeinschlag (rad)
    float wheelRot[4] = {};
    float comp[4] = {0.127f, 0.127f, 0.127f, 0.127f};
    Color paint{176, 28, 34, 255};
    bool brake = false, blinkL = false, blinkR = false, lights = false, reverse = false;
};
struct DashState {
    float kmh = 0, rpm = 0, steerInput = 0, odoKm = 0;
    int gear = 0, selector = 0, limit = 0;
    bool automatic = false, blinkL = false, blinkR = false, handbrake = false, belt = false, engineOn = false, lights = false;
    std::string road;
};
extern const Vec3 EYE;            // Fahrerkopf relativ zum Schwerpunkt
extern const Vec3 MIRROR_POS[3];
extern const float MIRROR_HW[3], MIRROR_HH[3];
void carModelInit();
void carDraw(const CarVisual& v, const Mat4& vp, bool cockpit, const DashState* dash);

// ---------- Oberflaeche (kleines Immediate-Mode-UI) ----------
struct Ui {
    Batch geo, text;
    float W = 1280, H = 720, mx = 0, my = 0;
    bool down = false, clicked = false, captured = false;
    void begin(float w, float h, float mouseX, float mouseY, bool isDown, bool wasClicked);
    bool over(float x, float y, float w, float h) const { return mx >= x && mx < x + w && my >= y && my < y + h; }
    void panel(float x, float y, float w, float h, Color c = Color{16, 18, 24, 215}, float r = 10);
    bool button(float x, float y, float w, float h, const std::string& label, bool active = false, float px = 17);
    void label(float x, float y, float px, Color c, const std::string& s);
    void labelC(float cx, float y, float px, Color c, const std::string& s);
    void labelR(float rx, float y, float px, Color c, const std::string& s);
    void flush(const Mat4& mvp);
};
extern Ui ui;
namespace col {
const Color white{238, 240, 244, 255}, gray{150, 156, 166, 255}, accent{255, 184, 48, 255}, red{238, 70, 60, 255}, green{80, 225, 120, 255},
    blue{110, 180, 255, 255}, dark{16, 18, 24, 215}, btn{38, 42, 52, 235}, btnHot{58, 66, 84, 245};
}
std::vector<std::string> wrapText(const std::string& s, float px, float maxW);

// ---------- Editor ----------
namespace editor {
void init();
void open(Vec3 at, float yaw);
void close();
void setCamera(Vec3 p, float yaw, float pitch);
bool active();
void event(const SDL_Event& e);
void update(float dt, const Mat4& vp, float ww, float wh);
void buildUI();
void drawWorld();             // gebaute Objekte + Strassenunterbau (in jedem Durchgang)
void drawGizmos(const Mat4& vp);
Mat4 camera();
Vec3 cameraPos();
void refresh(double now);     // lokale Koordinaten nach Ortswechsel / nachgeladenem Gelaende neu aufbauen
bool surface(float x, float z, float yRef, float& h);
bool blocked(float x, float z, float y);
void save();
bool wantsTestDrive(Vec3& pos, float& yaw);
}

// ---------- KI-Verkehr ----------
namespace traffic {
extern int wanted;            // gewuenschte Anzahl Fahrzeuge (0 = aus)
void update(float dt);
void draw(const Mat4& vp, Vec3 cam, float maxDist);
int count();
}

// ---------- Weltkarte ----------
namespace worldmap {
void open();
void close();
bool active();
void event(const SDL_Event& e);
void buildUI();
void draw(const Mat4& mvp, int ww, int wh);
void setCenter(double lat, double lon, float zoom);
}
void gameTeleport(double lat, double lon);      // Auto an einen beliebigen Ort der Welt setzen
void gamePosition(double& lat, double& lon);
void gameSearchOnMap();                         // Ortssuche starten, Ergebnis zentriert die Karte

// ---------- Mehrspieler ----------
struct NetCar {
    uint32_t id = 0;
    std::string name;
    double lat = 0, lon = 0;
    float y = 0, steer = 0, speed = 0, wheelRot = 0;
    Quat q;
    uint8_t flags = 0; // 1 Blinker links, 2 rechts, 4 Bremse, 8 Licht
    // nur lokal:
    Vec3 pos;
    Quat qs;
    bool placed = false;
    double lastSeen = 0;
};
namespace netplay {
bool host(int port);
bool join(const std::string& address, int port);
void stop();
bool active();
bool hosting();
std::string status();
void update(float dt, const NetCar& mine);
std::vector<NetCar>& cars();
}
