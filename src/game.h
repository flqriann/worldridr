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
    int model = -2;             // MODEL_SELECTED (-2), MODEL_BUILTIN (-1) oder Index in die Liste der .obj-Autos
    bool tinted = false;        // true: paint ueberschreibt die Originalfarbe des .obj-Modells
    bool siren = false;         // Blaulicht (Polizei im Einsatz)
    int kind = 0;               // 0 Auto, 1 Moped, 2 Motorrad
    float lean = 0;             // Schraeglage (rad), nur Zweiraeder
};
constexpr int MODEL_SELECTED = -2, MODEL_BUILTIN = -1;
// .obj-Autos aus assets/cars (Wavefront OBJ + MTL + Farbtextur, Raeder als eigene Gruppen "wheel-front-left" ...)
int carModelCount();
const std::string& carModelName(int i);
extern int carModelSel;           // gewaehltes Auto des Spielers (-1 = Eigenbau)
void carModelCycle();
int carModelFind(const std::string& name);   // Index oder -1
bool carModelInfo(int i, std::string& out);   // Abmessungen und Dreiecke (laedt das Modell)
extern int vehKind;                              // Fahrzeug des Spielers: 0 Auto, 1 Moped, 2 Motorrad
extern bool carColorSet;                       // Spielerfarbe gesetzt (sonst Originalfarbe)
extern uint8_t carColor[3];
void carOwnPaint(CarVisual& v);               // Spielerfarbe auf ein CarVisual anwenden
void carPreviewDraw(int x, int y, int w, int h, float yaw);   // 3D-Vorschau des gewaehlten Autos (Pixelrechteck)
struct DashState {
    float kmh = 0, rpm = 0, steerInput = 0, odoKm = 0;
    int gear = 0, selector = 0, limit = 0;
    bool automatic = false, blinkL = false, blinkR = false, handbrake = false, belt = false, engineOn = false, lights = false;
    float fuel = 1;                 // Tankinhalt 0..1
    std::string road;
};
extern Vec3 EYE;                  // Fahrerkopf relativ zum Schwerpunkt (je nach Fahrzeug)
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
std::string policeStatus();   // Hinweis fuer die Anzeige ("Polizei: Bitte anhalten"), sonst leer
bool policeActive();
void offence(int kind);       // Verstoss melden: 0 rote Ampel, 1 Stoppschild, 2 Unfall
float sirenLevel();           // Lautstaerke der naechsten Sirene 0..1
void policeMarks(std::vector<std::pair<Vec3, bool>>& out);  // Polizeiautos (Position, im Einsatz) fuer die Minimap
}

// ---------- Tank, Geld, Tankstellen, Supermaerkte ----------
namespace econ {
extern float fuel, fuelCap, money;                       // Liter, Liter, Euro
void setTank(float capacityLiters, float consumption);   // je nach Fahrzeug
float fuelFrac();
bool deliveryActive();
void update(float dt);
void keyJ();                                             // Tanken / Lieferauftrag annehmen
void navToNearest(int kind);                             // 0 Tankstelle, 1 Supermarkt
void onCrash();
void onArrived();                                        // Navigationsziel erreicht (Lieferung abliefern)
void buildHUD();
}

// ---------- Fussgaenger ----------
namespace ped {
extern int wanted;                                           // gewuenschte Anzahl (0 = aus)
void update(float dt);
void draw(const Mat4& vp, Vec3 cam, float maxDist);
void crossing(std::vector<std::pair<Vec3, Vec3>>& out);    // Fussgaenger auf der Fahrbahn (Position, Richtung)
int count();
void debugSpawnAhead();
}

// ---------- Navigation und Minimap ----------
namespace nav {
void setDestination(double lat, double lon);   // Route vom Auto zum Ziel berechnen (OSRM) und anzeigen
void clear();
bool active();                                 // Ziel gesetzt
bool destination(double& lat, double& lon);
const std::vector<std::pair<double, double>>& routeLatLon();  // Routenverlauf (Breite, Laenge)
void update(float dt);
void buildHUD();                               // Minimap und Abbiegehinweis (in die Oberflaeche)
void drawWorld();                              // Routenband und Zielmarkierung in der 3D-Welt
void zoomCycle();                              // Minimap: Zoomstufen, zuletzt aus
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
struct LanServer { std::string name, address; int players = 0; double seen = 0; };
namespace netplay {
struct ChatLine { std::string name, text; double t; };
bool server(int port, const std::string& name, int maxPlayers);   // eigenstaendiger Server ohne Fenster
void serverUpdate(float dt);
void discover();                                                    // LAN-Server per Broadcast suchen
void pollDiscovery();
std::vector<LanServer> lanServers();
void chat(const std::string& text);
std::vector<ChatLine>& chatLog();
int playerCount();
bool host(int port);
bool join(const std::string& address, int port);
void stop();
bool active();
bool hosting();
std::string status();
void update(float dt, const NetCar& mine);
std::vector<NetCar>& cars();
}
