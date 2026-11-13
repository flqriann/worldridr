// Tank, Geld, Tankstellen und Supermaerkte:
//  - Der Tank leert sich mit der Fahrt (Motor aus bei leerem Tank). Tankstellen (J) fuellen nach und kosten Geld.
//  - Supermaerkte vergeben Lieferauftraege (J): Waren zu einem Ziel bringen, die Navigation startet automatisch.
//    Bezahlt wird nach Strecke, Puenktlichkeit und Fahrstil; damit verdient man das Geld fuers Tanken.
#include "game.h"
#include <random>

namespace econ {
float fuel = -1, fuelCap = 45, money = 60;
float consumeScale = 1.0f;
}

namespace {
const float FUEL_ARCADE = 4.0f;   // schnellerer Verbrauch, damit Tankstellen im Spiel eine Rolle spielen
const float PUMP_RATE = 2.2f;     // Liter pro Sekunde beim Tanken
bool refueling = false, lowWarned = false, emptyWarned = false;
float refuelLiters = 0, refuelCost = 0;
int nearKind = -1;
Poi nearPoi;
struct Delivery { bool active = false; float reward = 0, limit = 0, km = 0; double start = 0; int crashes = 0; std::string place; } del;
std::mt19937 rng{4242};

std::string eur(float v) {
    char b[32];
    snprintf(b, sizeof b, "%.2f", v);
    for (char* p = b; *p; p++) if (*p == '.') *p = ',';
    return std::string(b) + " EUR";
}
std::string mmss(float s) {
    int t = std::max(0, (int)s);
    char b[16];
    snprintf(b, sizeof b, "%d:%02d", t / 60, t % 60);
    return b;
}
} // namespace

namespace econ {

float fuelFrac() { return fuelCap > 0 ? clampv(fuel / fuelCap, 0.0f, 1.0f) : 0.0f; }
bool deliveryActive() { return del.active; }

void setTank(float capacity, float consumption) {
    fuelCap = capacity;
    consumeScale = consumption;
    if (fuel < 0 || fuel > fuelCap) fuel = fuelCap;
}

static void findNear() {
    nearKind = -1;
    float bd = 24;
    world.forPoisNear(car.pos.x, car.pos.z, 24, [&](const Poi& p) {
        float d = std::hypot(p.x - car.pos.x, p.z - car.pos.z);
        if (d < bd) { bd = d; nearPoi = p; nearKind = p.kind; }
    });
}

void update(float dt) {
    if (fuel < 0) fuel = fuelCap;
    // Verbrauch
    if (car.engineOn) {
        float lps = (0.00025f + 8.3e-6f * car.throttle * car.rpm()) * FUEL_ARCADE * consumeScale;
        fuel -= lps * dt;
    }
    if (fuel <= 0) {
        fuel = 0;
        if (car.engineOn) car.engineOn = false;
        car.starter = false;
        if (!emptyWarned) { emptyWarned = true; hud.msgs.push_back({"Tank leer! Taste K: n\xC3\xA4" "chste Tankstelle navigieren", 6.0f}); }
    } else emptyWarned = false;
    if (fuelFrac() < 0.15f && !lowWarned && fuel > 0) { lowWarned = true; hud.msgs.push_back({"Tank fast leer - Taste K navigiert zur n\xC3\xA4" "chsten Tankstelle", 6.0f}); }
    if (fuelFrac() > 0.3f) lowWarned = false;

    findNear();
    if (refueling) { // laufendes Tanken
        bool ok = nearKind == 0 && std::fabs(car.speed()) < 0.6f && !car.engineOn;
        float dl = std::min(PUMP_RATE * dt, fuelCap - fuel);
        float price = nearPoi.price;
        if (ok && dl > 0 && money >= dl * price) {
            fuel += dl; money -= dl * price; refuelLiters += dl; refuelCost += dl * price;
        } else {
            refueling = false;
            if (refuelLiters > 0.05f) hud.msgs.push_back({"Getankt: " + std::to_string((int)std::round(refuelLiters)) + " l f\xC3\xBCr " + eur(refuelCost) + (fuel >= fuelCap - 0.05f ? " (Tank voll)" : ""), 5.0f});
            else if (money < price) hud.msgs.push_back({"Nicht genug Geld - Lieferauftr\xC3\xA4ge bei Superm\xC3\xA4rkten bringen Geld", 5.0f});
        }
    }
    // Lieferung: abgebrochen, wenn die Navigation beendet wurde
    if (del.active && !nav::active()) { del.active = false; hud.msgs.push_back({"Lieferauftrag abgebrochen", 4.0f}); }
}

void keyJ() {
    if (nearKind == 0) {
        if (refueling) { refueling = false; return; }
        if (car.engineOn) { hud.msgs.push_back({"Zum Tanken den Motor abstellen (Taste I)", 4.0f}); return; }
        if (std::fabs(car.speed()) > 0.6f) { hud.msgs.push_back({"Zum Tanken anhalten", 3.0f}); return; }
        if (fuel >= fuelCap - 0.05f) { hud.msgs.push_back({"Der Tank ist schon voll", 3.0f}); return; }
        refueling = true; refuelLiters = refuelCost = 0;
    } else if (nearKind == 1) {
        if (del.active) { hud.msgs.push_back({"Du hast schon einen Lieferauftrag", 3.0f}); return; }
        if (std::fabs(car.speed()) > 1.0f) { hud.msgs.push_back({"Zum Beladen anhalten", 3.0f}); return; }
        // Ziel: eine Strasse 1,2 bis 2,8 km entfernt
        std::vector<RoadSeg> segs;
        world.collectSegs(segs);
        std::vector<const RoadSeg*> cand;
        for (const RoadSeg& s : segs) {
            if (s.klass < 2 || s.klass > 5) continue;
            float d = std::hypot((s.ax + s.bx) * 0.5f - car.pos.x, (s.az + s.bz) * 0.5f - car.pos.z);
            if (d > 1200 && d < 2800) cand.push_back(&s);
        }
        if (cand.empty()) { hud.msgs.push_back({"Kein Lieferziel in Reichweite - die Karte l\xC3\xA4" "dt noch", 4.0f}); return; }
        const RoadSeg* s = cand[rng() % cand.size()];
        float tx = (s->ax + s->bx) * 0.5f, tz = (s->az + s->bz) * 0.5f;
        double lat, lon;
        world.toLatLon(tx, tz, lat, lon);
        float km = std::hypot(tx - car.pos.x, tz - car.pos.z) / 1000.0f * 1.3f;
        del.active = true; del.km = km; del.reward = 14.0f + km * 9.0f; del.limit = km * 1000.0f / 8.0f + 150.0f; del.start = simTime; del.crashes = 0;
        nav::setDestination(lat, lon);
        hud.msgs.push_back({"Lieferauftrag angenommen: " + eur(del.reward), 5.0f});
    }
}

void onCrash() { if (del.active) del.crashes++; }

void onArrived() {
    if (!del.active) return;
    float elapsed = (float)(simTime - del.start), pay = del.reward;
    bool late = elapsed > del.limit;
    if (late) pay *= 0.5f;
    pay = std::max(4.0f, pay - 6.0f * del.crashes);
    money += pay;
    del.active = false;
    hud.msgs.push_back({"Geliefert: +" + eur(pay) + (late ? " (versp\xC3\xA4tet)" : "") + (del.crashes ? "  (Unf\xC3\xA4lle abgezogen)" : ""), 6.0f});
}

// Naechste Tankstelle (kind 0) oder Supermarkt (kind 1) in den geladenen Kacheln als Navigationsziel setzen
void navToNearest(int kind) {
    float bd = 1e9f;
    Poi best;
    bool found = false;
    world.forPoisNear(car.pos.x, car.pos.z, 4000, [&](const Poi& p) {
        if (p.kind != kind) return;
        float d = std::hypot(p.x - car.pos.x, p.z - car.pos.z);
        if (d < bd) { bd = d; best = p; found = true; }
    });
    if (!found) { hud.msgs.push_back({kind == 0 ? "Keine Tankstelle in der N\xC3\xA4he gefunden" : "Kein Supermarkt in der N\xC3\xA4he gefunden", 4.0f}); return; }
    double lat, lon;
    world.toLatLon(best.x, best.z, lat, lon);
    nav::setDestination(lat, lon);
    hud.msgs.push_back({(kind == 0 ? "Navigation zur Tankstelle" : "Navigation zum Supermarkt") + std::string(" (") + std::to_string((int)(bd / 10) * 10) + " m)", 4.0f});
}

void buildHUD() {
    float W = ui.W, H = ui.H, cx = W * 0.5f;
    bool calm = cfg.hudMode == 0;
    char b[64];
    // Hinweis an Tankstelle / Supermarkt
    std::string prompt;
    if (refueling) {
        snprintf(b, sizeof b, "Tanke ...  %.1f l  \xC2\xB7  %s", refuelLiters, eur(refuelCost).c_str());
        prompt = b;
    } else if (nearKind == 0) {
        std::string pr = eur(nearPoi.price);
        prompt = "J  Tanken  ·  " + pr.substr(0, pr.size() - 4) + " EUR/l";
    } else if (nearKind == 1) prompt = del.active ? std::string() : "J  Lieferauftrag annehmen  (" + (nearPoi.name.empty() ? std::string("Supermarkt") : nearPoi.name) + ")";
    if (!prompt.empty()) {
        float w = font.width(prompt, 17) + 36;
        ui.geo.rrect(cx - w / 2, H - 150, w, 36, 12, Color{14, 17, 23, 215});
        ui.labelC(cx, H - 126, 17, Color{255, 214, 120, 255}, prompt);
    }
    // Lieferstatus (oben rechts unter dem Tempolimit)
    if (del.active) {
        float left = del.limit - (float)(simTime - del.start);
        std::string t = "Lieferung  " + eur(del.reward) + "  \xC2\xB7  " + (left > 0 ? mmss(left) : std::string("verspätet"));
        float w = font.width(t, 14) + 28;
        ui.geo.rrect(W - w - 14, 82, w, 28, 10, Color{14, 17, 23, 170});
        ui.label(W - w, 101, 14, left > 0 ? col::white : Color{240, 130, 110, 255}, t);
    }
    // Geld: nur wenn es gerade wichtig ist (an POIs, bei knappem Tank) oder im ausfuehrlichen Modus
    if (!calm || nearKind >= 0 || fuelFrac() < 0.2f || del.active) {
        std::string t = eur(money);
        ui.label(16, H - 28, 14, Color{190, 196, 206, 255}, t);
    }
}

} // namespace econ
