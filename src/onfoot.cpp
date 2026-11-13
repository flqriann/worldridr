// Zu Fuss unterwegs: Aussteigen/Einsteigen, Auto zu- und aufsperren, vom Auto erfasst werden (Ragdoll, schwarzer Bildschirm, aufstehen).
#include "game.h"
#include "audio.h"
#include "keys.h"
#include "ragdoll.h"

namespace foot {
bool on = false, locked = false;
Vec3 pos;
float yaw = 0, camYaw = 0;
}

namespace {
enum State { WALK, RAG, FADEOUT, BLACK, FADEIN } st = WALK;
float stateT = 0, speedS = 0, phase = 0, lockFlash = 0;
Ragdoll rag;
const Color SHIRT{70, 100, 170, 255}, PANTS{40, 46, 70, 255}, SKIN{236, 190, 160, 255}, HAIR{60, 40, 28, 255};

float distToCar() { return std::hypot(foot::pos.x - car.pos.x, foot::pos.z - car.pos.z); }
} // namespace

namespace foot {

void exitCar() {
    if (on) return;
    if (std::fabs(car.speed()) > 2.0f) { hud.msgs.push_back({"Zum Aussteigen anhalten", 3.0f}); return; }
    car.engineOn = false; car.handbrake = true; car.starter = false;
    // links neben dem Auto, sonst rechts
    Vec3 side[2] = {qrot(car.q, {-1.9f, 0, 0.2f}), qrot(car.q, {1.9f, 0, 0.2f})};
    Vec3 p = car.pos + side[0];
    if (world.insideBuilding(p.x, p.z)) p = car.pos + side[1];
    float h = car.pos.y - 0.5f;
    world.surface(p.x, p.z, car.pos.y + 1.0f, h);
    pos = {p.x, h + 0.05f, p.z};
    Vec3 f = car.forward();
    yaw = camYaw = std::atan2(-f.x, -f.z);
    on = true; st = WALK; stateT = 0; speedS = 0;
    hud.msgs.push_back({"Ausgestiegen - O: einsteigen, Y: zusperren", 4.0f});
}

void enterCar() {
    if (!on || st != WALK) return;
    if (distToCar() > 4.2f) { hud.msgs.push_back({"Zu weit vom Auto entfernt", 3.0f}); return; }
    if (locked) { hud.msgs.push_back({"Das Auto ist abgesperrt - Y zum Aufsperren", 3.5f}); audioClick(); return; }
    on = false;
    Vec3 f = car.forward();
    hud.msgs.push_back({"Eingestiegen", 2.0f});
    (void)f;
}

void toggleLock() {
    if (on && distToCar() > 30.0f) { hud.msgs.push_back({"Zu weit vom Auto entfernt (Funkschl\xC3\xBC" "ssel)", 3.0f}); return; }
    locked = !locked;
    lockFlash = 1.0f;
    audioClick();
    hud.msgs.push_back({locked ? "Auto abgesperrt" : "Auto aufgesperrt", 2.5f});
}

bool lockBlink() { return lockFlash > 0 && std::fmod(lockFlash, 0.5f) > 0.22f; }

Vec3 forwardDir() { return {-std::sin(yaw), 0, -std::cos(yaw)}; }

// Hinweistext fuer die Anzeige
std::string prompt() {
    if (!on || st != WALK) return "";
    float d = distToCar();
    if (d > 4.2f) return locked && d < 12 ? "Auto abgesperrt (Y: aufsperren)" : "";
    return locked ? "Auto abgesperrt  \xC2\xB7  Y: aufsperren" : "O  Einsteigen  \xC2\xB7  Y: zusperren";
}

float fade() {
    if (!on) return 0;
    if (st == FADEOUT) return clampv(stateT / 0.9f, 0.0f, 1.0f);
    if (st == BLACK) return 1.0f;
    if (st == FADEIN) return 1.0f - clampv(stateT / 1.3f, 0.0f, 1.0f);
    return 0;
}

bool incapacitated() { return on && st != WALK; }

Mat4 camera() {
    Vec3 target = (st == RAG || st == FADEOUT) ? rag.center() : pos;
    float dist = (st == RAG || st == FADEOUT) ? 5.2f : 3.4f;
    float h = (st == RAG || st == FADEOUT) ? 2.4f : 1.9f;
    Vec3 back{std::sin(camYaw), 0, std::cos(camYaw)};
    Mat4 m = mul(translate(target + back * dist + Vec3{0, h, 0}), mul(fromQuatPos(qaxis({0, 1, 0}, camYaw), {}), fromQuatPos(qaxis({1, 0, 0}, -0.2f), {})));
    return m;
}

void update(float dt) {
    if (lockFlash > 0) lockFlash = std::max(0.0f, lockFlash - dt);
    if (!on) return;
    stateT += dt;
    Vec3 hv;
    switch (st) {
    case WALK: {
        float fwd = (keys::held(A_THROTTLE) ? 1.0f : 0.0f) - (keys::held(A_BRAKE) ? 1.0f : 0.0f);
        float str = (keys::held(A_RIGHT) ? 1.0f : 0.0f) - (keys::held(A_LEFT) ? 1.0f : 0.0f);
        float want = (fwd != 0 || str != 0) ? (keys::held(A_SPRINT) ? 4.4f : 1.55f) : 0.0f;
        speedS += (want - speedS) * std::min(1.0f, dt * 6.0f);
        if (fwd != 0 || str != 0) {
            Vec3 f{-std::sin(camYaw), 0, -std::cos(camYaw)}, r{std::cos(camYaw), 0, -std::sin(camYaw)};
            Vec3 dir = normalize(f * fwd + r * str);
            float tw = std::atan2(-dir.x, -dir.z), err = std::remainder(tw - yaw, 2 * PI);
            yaw += err * std::min(1.0f, dt * 9.0f);
            Vec3 np = pos + dir * (speedS * dt);
            if (!world.insideBuilding(np.x, pos.z) && !editor::blocked(np.x, pos.z, pos.y)) pos.x = np.x;
            if (!world.insideBuilding(pos.x, np.z) && !editor::blocked(pos.x, np.z, pos.y)) pos.z = np.z;
        }
        // eigenes Auto ist ein Hindernis
        Vec3 rel = pos - car.pos;
        Vec3 cf = car.forward(), cr{-cf.z, 0, cf.x};
        float along = dot(rel, cf), lat = dot(rel, cr);
        if (std::fabs(along) < 2.4f && std::fabs(lat) < 1.15f) {
            if (std::fabs(lat) / 1.15f > std::fabs(along) / 2.4f) lat = lat >= 0 ? 1.15f : -1.15f; else along = along >= 0 ? 2.4f : -2.4f;
            pos.x = car.pos.x + cf.x * along + cr.x * lat;
            pos.z = car.pos.z + cf.z * along + cr.z * lat;
        }
        float h;
        if (world.surface(pos.x, pos.z, pos.y + 1.2f, h)) pos.y += (h + 0.05f - pos.y) * std::min(1.0f, dt * 14.0f);
        phase += dt * speedS * 4.6f;
        if (traffic::hitTest(pos, 0.45f, hv)) { // von einem Fahrzeug erfasst
            rag.shirt = SHIRT; rag.pants = PANTS; rag.skin = SKIN; rag.hair = HAIR;
            rag.start(pos, yaw, hv * 0.9f);
            st = RAG; stateT = 0;
            hud.msgs.push_back({"Du wurdest angefahren!", 3.0f});
            traffic::offence(2);
        }
        break;
    }
    case RAG:
        rag.step(dt);
        if (stateT > 1.2f && (rag.speed() < 0.5f || stateT > 3.2f)) { st = FADEOUT; stateT = 0; }
        break;
    case FADEOUT:
        rag.step(dt);
        if (stateT >= 0.9f) {
            Vec3 c = rag.center();
            float h = c.y;
            world.surface(c.x, c.z, c.y + 1.0f, h);
            pos = {c.x, h + 0.05f, c.z};
            st = BLACK; stateT = 0; speedS = 0;
        }
        break;
    case BLACK:
        if (stateT >= 1.8f) { st = FADEIN; stateT = 0; hud.msgs.push_back({"Du stehst wieder auf", 3.0f}); }
        break;
    case FADEIN:
        if (stateT >= 1.3f) { st = WALK; stateT = 0; }
        break;
    }
}

// Spielerfigur / Ragdoll in den gemeinsamen Fussgaenger-Batch zeichnen
void appendToBatch(Batch& b) {
    if (!on) return;
    if (st == RAG || st == FADEOUT) { rag.draw(b); return; }
    ped::appendFigure(b, pos, yaw, phase, clampv(speedS / 1.55f, 0.0f, 2.6f), SHIRT, PANTS, SKIN, HAIR);
}

} // namespace foot

Vec3 playerPos() { return foot::on ? foot::pos : car.pos; }
Vec3 playerForward() { return foot::on ? Vec3{-std::sin(foot::camYaw), 0, -std::cos(foot::camYaw)} : car.forward(); }

namespace foot {
// Selbsttest: Spieler wird mit 9 m/s von hinten getroffen
void debugHit() {
    if (!on || st != WALK) return;
    rag.shirt = SHIRT; rag.pants = PANTS; rag.skin = SKIN; rag.hair = HAIR;
    rag.start(pos, yaw, Vec3{-std::sin(yaw), 0, -std::cos(yaw)} * 9.0f);
    st = RAG; stateT = 0;
}
}
