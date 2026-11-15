// KI-Verkehr: Fahrzeuge folgen dem Strassennetz (OpenStreetMap + selbst gebaute Strassen), halten Abstand,
// beachten Ampeln, Stoppschilder, Vorrang der hoeherrangigen Strasse und Rechts-vor-links.
#include "game.h"
#include <random>
#include <unordered_map>

namespace traffic {
int wanted = 22;
}

namespace {
struct Edge { RoadSeg s; int n0, n1; float len, dx, dz; };
std::vector<Edge> E;
std::vector<std::vector<int>> adj;
std::vector<Vec3> nodeP;
std::unordered_map<uint64_t, int> nodeIdx;
int builtVersion = -1, builtGen = -1;
std::mt19937 rng{20240611};

struct AI {
    int e = 0, ne = -1;
    bool rev = false, nrev = false;
    float s = 0, yaw = 0, v = 0, wheelRot = 0, steerVis = 0, waited = 0, stopTimer = 0, stuck = 0, crashWait = 0;
    Vec3 pos;
    Color paint;
    int model = 0;
    bool braking = false, ctl = false, stopDone = false, yielding = false;
    int police = 0;       // 0 normal, 1 Streife, 2 im Einsatz (Blaulicht), 3 folgt unauffaellig
    float latBias = 0;    // seitlicher Versatz in der Spur (m), z. B. beim Ueberholen
    bool pullover = false; // Polizei fuehrt den Spieler an den Strassenrand (Warnblinker)
    Sway sl, sg;          // Insassen federn mit der Fahrdynamik (seitlich / laengs)
    float prevV = 0, headYaw = 0;
    bool passenger = false;
    Color shirt{70, 100, 170, 255};
    int kind = 0;         // 0 Auto, 1 Moped, 2 Motorrad
    int uid = 0;
    int turn = 0;
    bool freeBody = false;   // nach einem Zusammenstoss: frei beweglicher Starrkoerper statt Spurfolge
    Vec3 vel;                // Geschwindigkeit im freien Zustand (x, z)
    float yawRate = 0, freeT = 0, slowT = 0;
    bool puppet = false;     // Mitspieler-Ansicht: Zustand kommt vom Host
    Vec3 tgt; float tyaw = 0; double seen = 0;
};
std::vector<AI> cars;

// ---------- Polizei ----------
// Ablauf: Verstoesse erhoehen "heat". Ab einem Schwellwert kommt eine Streife (oder wird neu losgeschickt), blinkt und ordnet
// das Anhalten an (ORDER). Haelt der Spieler an, gibt es eine Verkehrskontrolle (STOPPED). Ignoriert er die Anweisung,
// beginnt die Verfolgung (CHASE), bis er die Polizei abhaengt oder stehen bleibt.
enum { P_NONE, P_FOLLOW, P_ORDER, P_CHASE, P_STOPPED };
struct PoliceCase {
    int state = P_NONE, uid = -1, witness = -1;
    bool passed = false;
    double t0 = 0, cooldown = 0, farSince = -1;
    float heat = 0, speeding = 0, stopHold = 0, still = 0;
} pc;
int nextUid = 1;
int policeModel = -2;

bool isPoliceCar(const AI& c) { return c.police > 0; }
AI* policeCar() {
    for (AI& c : cars) if (c.uid == pc.uid && c.police > 0) return &c;
    return nullptr;
}

// Alle Verkehrsteilnehmer in einheitlicher Form (KI, Spieler, Mitspieler)
struct Obs { Vec3 pos, fwd; float v; int toNode; float dNode; int klass; bool ctl, player; int ai, edge; bool yielding; };
double priorityCooldown = 0, crashCooldown = 0;

uint64_t nodeKey(float x, float z) { return ((uint64_t)(uint32_t)(int)std::lround(x * 4) << 32) | (uint32_t)(int)std::lround(z * 4); }
int nodeOf(float x, float z) {
    uint64_t k = nodeKey(x, z);
    auto it = nodeIdx.find(k);
    if (it != nodeIdx.end()) return it->second;
    nodeIdx[k] = (int)nodeP.size();
    nodeP.push_back({x, 0, z});
    adj.emplace_back();
    return (int)nodeP.size() - 1;
}
bool allowed(int e, bool rev) { int o = E[e].s.oneway; return o == 0 || (o == 1 && !rev) || (o == -1 && rev); }
int fromNode(int e, bool rev) { return rev ? E[e].n1 : E[e].n0; }
int toNode(int e, bool rev) { return rev ? E[e].n0 : E[e].n1; }
Vec3 dirOf(int e, bool rev) { return rev ? Vec3{-E[e].dx, 0, -E[e].dz} : Vec3{E[e].dx, 0, E[e].dz}; }
float laneOffset(const RoadSeg& s) {
    if (s.oneway) return s.lanes >= 2 ? s.hw - s.hw / s.lanes : 0.0f;
    int perDir = std::max(1, s.lanes / 2);
    return s.lanes < 2 ? s.hw * 0.45f : s.hw - s.hw / perDir * 0.5f;
}

void chooseNext(AI& c) {
    int n = toNode(c.e, c.rev);
    Vec3 d = dirOf(c.e, c.rev);
    if (c.police >= 2) { // im Einsatz: den Abzweig waehlen, der am besten zum Spieler fuehrt (auch Wenden)
        Vec3 toP = car.pos - nodeP[n];
        toP.y = 0;
        toP = normalize(toP);
        int bestE = -1;
        bool bestRev = false;
        float bestS = -2;
        for (int e2 : adj[n]) {
            bool rev2 = E[e2].n1 == n;
            if (e2 == c.e) rev2 = !c.rev;
            if (!allowed(e2, rev2)) continue;
            float s = dot(dirOf(e2, rev2), toP) - (e2 == c.e ? 0.35f : 0.0f);
            if (s > bestS) { bestS = s; bestE = e2; bestRev = rev2; }
        }
        if (bestE >= 0) { c.ne = bestE; c.nrev = bestRev; return; }
    }
    std::vector<std::pair<int, bool>> cand;
    std::vector<float> w;
    for (int e2 : adj[n]) {
        if (e2 == c.e) continue;
        bool rev2 = E[e2].n1 == n;
        if (!allowed(e2, rev2)) continue;
        cand.push_back({e2, rev2});
        w.push_back(0.4f + 2.5f * std::max(0.0f, dot(d, dirOf(e2, rev2))));
    }
    if (cand.empty()) {
        if (allowed(c.e, !c.rev)) { c.ne = c.e; c.nrev = !c.rev; } else c.ne = -1; // Sackgasse: wenden
        return;
    }
    std::discrete_distribution<int> dist(w.begin(), w.end());
    int k = dist(rng);
    c.ne = cand[k].first; c.nrev = cand[k].second;
}

Vec3 pathPoint(const AI& c, float ahead) {
    float d = c.s + ahead;
    int e = c.e;
    bool rev = c.rev;
    if (d > E[e].len && c.ne >= 0) { d -= E[e].len; e = c.ne; rev = c.nrev; }
    Vec3 dir = dirOf(e, rev), a = nodeP[fromNode(e, rev)];
    Vec3 right{-dir.z, 0, dir.x};
    return a + dir * d + right * (laneOffset(E[e].s) + c.latBias);
}

void rebuild() {
    std::vector<RoadSeg> segs;
    world.collectSegs(segs);
    E.clear(); adj.clear(); nodeP.clear(); nodeIdx.clear();
    for (const RoadSeg& s : segs) {
        if (s.klass > 5) continue; // keine Zufahrten und Feldwege
        float dx = s.bx - s.ax, dz = s.bz - s.az, len = std::hypot(dx, dz);
        if (len < 0.3f) continue;
        Edge ed{s, nodeOf(s.ax, s.az), nodeOf(s.bx, s.bz), len, dx / len, dz / len};
        if (ed.n0 == ed.n1) continue;
        adj[ed.n0].push_back((int)E.size());
        adj[ed.n1].push_back((int)E.size());
        E.push_back(ed);
    }
    // vorhandene Fahrzeuge auf das neue Netz umhaengen
    for (size_t i = 0; i < cars.size();) {
        AI& c = cars[i];
        Vec3 f{-std::sin(c.yaw), 0, -std::cos(c.yaw)};
        int best = -1;
        float bd = 4.0f;
        for (size_t e = 0; e < E.size(); e++) {
            const Edge& ed = E[e];
            float t = clampv((c.pos.x - ed.s.ax) * ed.dx + (c.pos.z - ed.s.az) * ed.dz, 0.0f, ed.len);
            float dd = std::hypot(c.pos.x - (ed.s.ax + ed.dx * t), c.pos.z - (ed.s.az + ed.dz * t)) - ed.s.hw;
            if (dd < bd && std::fabs(f.x * ed.dx + f.z * ed.dz) > 0.5f) { bd = dd; best = (int)e; }
        }
        if (best < 0) { cars.erase(cars.begin() + i); continue; }
        c.e = best;
        c.rev = f.x * E[best].dx + f.z * E[best].dz < 0;
        if (!allowed(c.e, c.rev)) { cars.erase(cars.begin() + i); continue; }
        Vec3 a = nodeP[fromNode(c.e, c.rev)], d = dirOf(c.e, c.rev);
        c.s = dot(c.pos - a, d);
        chooseNext(c);
        i++;
    }
}

void spawn(bool pursuer = false) {
    if (E.empty()) return;
    std::uniform_int_distribution<int> pick(0, (int)E.size() - 1);
    static const Color pal[10] = {{40, 90, 190, 255}, {228, 228, 232, 255}, {28, 30, 34, 255}, {150, 154, 162, 255}, {230, 170, 30, 255},
                                  {40, 120, 80, 255}, {120, 30, 36, 255}, {90, 96, 104, 255}, {200, 205, 210, 255}, {30, 60, 110, 255}};
    for (int tries = 0; tries < 40; tries++) {
        int e = pick(rng);
        const Edge& ed = E[e];
        if (ed.len < 8) continue;
        Vec3 mid{(ed.s.ax + ed.s.bx) * 0.5f, 0, (ed.s.az + ed.s.bz) * 0.5f};
        float dp = std::hypot(mid.x - playerPos().x, mid.z - playerPos().z);
        if (pursuer ? (dp < 70 || dp > 200) : (dp < 80 || dp > 330)) continue;
        if (pursuer) { Vec3 f = car.forward(); f.y = 0; f = normalize(f); if (dot(mid - car.pos, f) > -25) continue; }
        AI c;
        c.e = e;
        c.rev = (rng() & 1) != 0;
        if (pursuer) { Vec3 toP = car.pos - Vec3{(ed.s.ax + ed.s.bx) * 0.5f, 0, (ed.s.az + ed.s.bz) * 0.5f}; c.rev = (ed.dx * toP.x + ed.dz * toP.z) < 0; }
        if (!allowed(e, c.rev)) c.rev = !c.rev;
        c.s = ed.len * 0.5f;
        chooseNext(c);
        c.pos = pathPoint(c, 0);
        bool tooClose = false;
        for (const AI& o : cars) if (std::hypot(o.pos.x - c.pos.x, o.pos.z - c.pos.z) < 14) tooClose = true;
        float h;
        if (tooClose ||!world.surface(c.pos.x, c.pos.z, 1e9f, h)) continue;
        c.pos.y = h;
        Vec3 d = dirOf(e, c.rev);
        c.yaw = std::atan2(-d.x, -d.z);
        c.v = std::min(ed.s.maxspeed, 50.0f) / 3.6f * 0.6f;
        c.paint = pal[rng() % 10];
        c.model = carModelCount() > 0 ? (int)(rng() % carModelCount()) : -1;
        c.uid = nextUid++;
        c.passenger = rng() % 100 < 40;
        { static const Color sh[6] = {{196, 70, 62, 255}, {60, 100, 170, 255}, {220, 220, 214, 255}, {70, 140, 90, 255}, {230, 180, 60, 255}, {90, 90, 100, 255}}; c.shirt = sh[rng() % 6]; }
        { int r100 = (int)(rng() % 100); c.kind = r100 < 9 ? 1 : (r100 < 15 ? 2 : 0); }
        if (policeModel == -2) policeModel = carModelFind("police");
        int patrols = 0;
        for (const AI& o : cars) if (o.police == 1) patrols++;
        if (pursuer || (cfg.police && policeModel >= 0 && patrols < std::max(1, traffic::wanted / 8) && rng() % 4 == 0)) {
            c.police = pursuer ? 2 : 1;
            c.model = policeModel; c.kind = 0;
            if (pursuer) { c.v = std::min(ed.s.maxspeed, 90.0f) / 3.6f * 0.8f; chooseNext(c); }
        } else if (carModelCount() > 1 && c.model == policeModel) c.model = (c.model + 1) % carModelCount();
        cars.push_back(c);
        return;
    }
}


// ---------- Zusammenstoesse: Rechtecke (2D) mit Impuls, Drehimpuls und Reibung ----------
struct Body { Vec3 pos, fwd, vel; float hl = 2.15f, hw = 0.92f, m = 1300, w = 0; float I() const { return m * (4 * hl * hl + 4 * hw * hw) / 12.0f; } };
bool insideBox(const Body& X, Vec3 p) {
    Vec3 d = p - X.pos, r{-X.fwd.z, 0, X.fwd.x};
    return std::fabs(dot(d, X.fwd)) <= X.hl + 0.03f && std::fabs(dot(d, r)) <= X.hw + 0.03f;
}
// n zeigt von A nach B; liefert Eindringtiefe und Kontaktpunkt
bool obbContact(const Body& A, const Body& B, Vec3& n, float& pen, Vec3& pt) {
    Vec3 ar{-A.fwd.z, 0, A.fwd.x}, br{-B.fwd.z, 0, B.fwd.x};
    Vec3 axes[4] = {A.fwd, ar, B.fwd, br};
    Vec3 d = B.pos - A.pos;
    d.y = 0;
    float best = 1e9f;
    for (int k = 0; k < 4; k++) {
        const Vec3& ax = axes[k];
        float ra = A.hl * std::fabs(dot(ax, A.fwd)) + A.hw * std::fabs(dot(ax, ar));
        float rb = B.hl * std::fabs(dot(ax, B.fwd)) + B.hw * std::fabs(dot(ax, br));
        float dist = dot(d, ax), ov = ra + rb - std::fabs(dist);
        if (ov <= 0) return false;
        if (ov < best) { best = ov; n = dist >= 0 ? ax : ax * -1.0f; }
    }
    pen = best;
    auto corners = [&](const Body& X, Vec3* c) {
        Vec3 r{-X.fwd.z, 0, X.fwd.x};
        c[0] = X.pos + X.fwd * X.hl + r * X.hw; c[1] = X.pos + X.fwd * X.hl - r * X.hw;
        c[2] = X.pos - X.fwd * X.hl + r * X.hw; c[3] = X.pos - X.fwd * X.hl - r * X.hw;
    };
    Vec3 ca[4], cb[4];
    corners(A, ca); corners(B, cb);
    int ia = 0, ib = 0;
    for (int k = 1; k < 4; k++) { if (dot(ca[k], n) > dot(ca[ia], n)) ia = k; if (dot(cb[k], n) < dot(cb[ib], n)) ib = k; }
    if (insideBox(A, cb[ib])) pt = cb[ib];
    else if (insideBox(B, ca[ia])) pt = ca[ia];
    else pt = (ca[ia] + cb[ib]) * 0.5f;
    return true;
}
// Impuls mit Rotation und Coulomb-Reibung. Liefert die Annaeherungsgeschwindigkeit entlang der Normalen (m/s)
float resolveContact(Body& A, Body& B, Vec3 n, float pen, Vec3 pt, bool moveB = true) {
    float ima = 1.0f / A.m, imb = moveB ? 1.0f / B.m : 0.0f;
    float sum = ima + imb;
    A.pos = A.pos - n * (pen * ima / sum * 0.8f);
    if (moveB) B.pos += n * (pen * imb / sum * 0.8f);
    Vec3 rA = pt - A.pos, rB = pt - B.pos;
    Vec3 vA = A.vel + Vec3{A.w * rA.z, 0, -A.w * rA.x}, vB = B.vel + Vec3{B.w * rB.z, 0, -B.w * rB.x};
    Vec3 vr = vB - vA;
    float vn = dot(vr, n);
    if (vn >= 0) return 0;
    float iA = 1.0f / A.I(), iB = moveB ? 1.0f / B.I() : 0.0f;
    float rAn = rA.z * n.x - rA.x * n.z, rBn = rB.z * n.x - rB.x * n.z;
    float j = -(1.0f + 0.16f) * vn / (ima + imb + rAn * rAn * iA + rBn * rBn * iB);
    Vec3 J = n * j;
    A.vel = A.vel - J * ima; A.w -= (rA.z * J.x - rA.x * J.z) * iA;
    if (moveB) { B.vel += J * imb; B.w += (rB.z * J.x - rB.x * J.z) * iB; }
    // Reibung entlang der Beruehrflaeche (Lack schleift, Fahrzeuge werden mitgenommen)
    Vec3 t = vr - n * vn;
    float tl = length(t);
    if (tl > 0.05f) {
        t = t * (1.0f / tl);
        float rAt = rA.z * t.x - rA.x * t.z, rBt = rB.z * t.x - rB.x * t.z;
        float jt = -dot(vr, t) / (ima + imb + rAt * rAt * iA + rBt * rBt * iB);
        jt = clampv(jt, -0.4f * j, 0.4f * j);
        Vec3 Jt = t * jt;
        A.vel = A.vel - Jt * ima; A.w -= (rA.z * Jt.x - rA.x * Jt.z) * iA;
        if (moveB) { B.vel += Jt * imb; B.w += (rB.z * Jt.x - rB.x * Jt.z) * iB; }
    }
    return -vn;
}
Body aiBody(const AI& c) {
    Body b;
    b.fwd = {-std::sin(c.yaw), 0, -std::cos(c.yaw)};
    b.pos = c.pos;
    if (c.kind > 0 && !c.police) { b.hl = 1.0f; b.hw = 0.4f; b.m = 220; }
    b.vel = c.freeBody ? c.vel : b.fwd * c.v;
    b.w = c.freeBody ? c.yawRate : 0;
    return b;
}
void storeAi(AI& c, const Body& b) {
    c.pos.x = b.pos.x; c.pos.z = b.pos.z;
    c.vel = Vec3{b.vel.x, 0, b.vel.z};
    c.yawRate = b.w;
    c.v = length(c.vel);
    if (!c.freeBody) { c.freeBody = true; c.freeT = 0; c.slowT = 0; }
}
// Frei beweglicher Wagen: Reifen bauen Querbewegung ab, Drehung klingt ab, Gebaeude lassen ihn abprallen
void stepFree(AI& c, float dt) {
    Vec3 fwd{-std::sin(c.yaw), 0, -std::cos(c.yaw)}, right{-fwd.z, 0, fwd.x};
    float vf = dot(c.vel, fwd), vl = dot(c.vel, right);
    auto toward0 = [](float v, float d) { return v > 0 ? std::max(0.0f, v - d) : std::min(0.0f, v + d); };
    vl = toward0(vl, 7.0f * dt);
    vf = toward0(vf, (c.freeT < 0.4f ? 1.5f : 4.0f) * dt);
    c.vel = fwd * vf + right * vl;
    float sp = length(c.vel);
    c.yawRate *= std::exp(-(0.9f + 0.45f * sp) * dt);
    Vec3 prev = c.pos;
    float prevYaw = c.yaw;
    c.yaw += c.yawRate * dt;
    c.pos += c.vel * dt;
    Vec3 nf{-std::sin(c.yaw), 0, -std::cos(c.yaw)};
    if (world.insideBuilding(c.pos.x, c.pos.z) || world.insideBuilding(c.pos.x + nf.x * 2, c.pos.z + nf.z * 2) || world.insideBuilding(c.pos.x - nf.x * 2, c.pos.z - nf.z * 2)) {
        c.pos = prev; c.yaw = prevYaw;
        c.vel = c.vel * -0.25f; c.yawRate *= -0.3f;
    }
    float h;
    if (world.surface(c.pos.x, c.pos.z, c.pos.y + 1.2f, h)) { float d = h - c.pos.y, lim = 0.45f * std::fabs(c.v) * dt + 0.01f; if (std::fabs(d) > 2.5f) c.pos.y = h; else c.pos.y += clampv(d * std::min(1.0f, dt * 12), -lim, lim); }   // Steigung begrenzt: keine Spruenge an Rampen
    c.v = length(c.vel);
    c.wheelRot = std::fmod(c.wheelRot + vf / 0.316f * dt, 2 * PI);
    c.braking = true; c.steerVis *= 0.9f;
    c.freeT += dt;
}
// Wieder auf die Strasse setzen, wenn das Fahrzeug steht und auf einer Fahrbahn liegt
bool reattach(AI& c) {
    Vec3 f{-std::sin(c.yaw), 0, -std::cos(c.yaw)};
    int best = -1;
    float bd = 3.0f;
    for (size_t e = 0; e < E.size(); e++) {
        const Edge& ed = E[e];
        float t = clampv((c.pos.x - ed.s.ax) * ed.dx + (c.pos.z - ed.s.az) * ed.dz, 0.0f, ed.len);
        float dd = std::hypot(c.pos.x - (ed.s.ax + ed.dx * t), c.pos.z - (ed.s.az + ed.dz * t)) - ed.s.hw;
        float al = f.x * ed.dx + f.z * ed.dz;
        if (dd < bd && std::fabs(al) > 0.5f && allowed((int)e, al < 0)) { bd = dd; best = (int)e; }
    }
    if (best < 0) return false;
    c.e = best;
    c.rev = f.x * E[best].dx + f.z * E[best].dz < 0;
    c.s = clampv(dot(c.pos - nodeP[fromNode(c.e, c.rev)], dirOf(c.e, c.rev)), 0.0f, E[best].len - 0.5f);
    c.freeBody = false; c.v = 0; c.vel = {}; c.yawRate = 0; c.latBias = 0; c.crashWait = 1.5f; c.waited = 0; c.stopDone = false;
    chooseNext(c);
    return true;
}

float stopLaw(float dist) { return std::sqrt(2.0f * 2.6f * std::max(0.0f, dist)); }
void endCase(AI* p, float coolDown, const char* msg) {
    if (msg) hud.msgs.push_back({msg, 5.0f});
    pc.state = P_NONE; pc.heat = 0; pc.speeding = 0; pc.still = 0; pc.cooldown = simTime + coolDown;
    if (p) { p->police = 1; p->ne = -1; chooseNext(*p); }
}

// Streife in der Naehe, die den Spieler wirklich sehen konnte (Entfernung und freie Sicht, keine Gebaeude dazwischen)
AI* witness(float maxDist) {
    AI* best = nullptr;
    float bd = maxDist;
    for (AI& c : cars) {
        if (c.police != 1) continue;
        float d = std::hypot(c.pos.x - car.pos.x, c.pos.z - car.pos.z);
        if (d >= bd) continue;
        bool clear = true;
        for (int k = 1; k < 8 && clear; k++) {
            float t = k / 8.0f;
            if (world.insideBuilding(c.pos.x + (car.pos.x - c.pos.x) * t, c.pos.z + (car.pos.z - c.pos.z) * t)) clear = false;
        }
        if (clear) { bd = d; best = &c; }
    }
    return best;
}

void policeLogic(float dt, const RoadHit& ph) {
    float spd = std::fabs(car.speed()) * 3.6f;
    // Tempoverstoss zaehlt nur, wenn eine Streife in der Naehe ist und freie Sicht hat
    bool fast = ph.found && ph.dist < ph.seg->hw + 1.5f && spd > ph.seg->maxspeed + 25 && spd > 45;
    if (fast && witness(55.0f)) pc.speeding += dt; else pc.speeding = std::max(0.0f, pc.speeding - dt);
    if (pc.speeding > 3.0f) { pc.heat += dt * 0.9f; if (AI* w = witness(55.0f)) pc.witness = w->uid; }
    else if (pc.state == P_NONE) pc.heat = std::max(0.0f, pc.heat - dt * 0.3f);

    AI* p = policeCar();
    { static int lastState = -1; if (pc.state != lastState) { lastState = pc.state; logMsg("Polizei-Zustand: %d (heat %.1f)", pc.state, pc.heat); } }
    if (pc.state == P_NONE) {
        if (simTime < pc.cooldown) { pc.heat = 0; return; }
        if (pc.heat < 2.0f) return;
        // Die Streife, die es gesehen hat, faehrt unauffaellig hinterher (kein Blaulicht, keine Anzeige)
        AI* best = nullptr;
        for (AI& c : cars) if (c.police == 1 && c.uid == pc.witness) best = &c;
        if (!best) { pc.heat = 0; return; }
        best->police = 3;
        best->ne = -1;
        chooseNext(*best);
        logMsg("Polizei: Streife folgt unauffaellig");
        pc.uid = best->uid; pc.state = P_FOLLOW; pc.t0 = simTime; pc.still = 0; pc.farSince = -1; pc.passed = false;
        return;
    }
    if (!p) { endCase(nullptr, 30, nullptr); return; }
    float d = std::hypot(p->pos.x - car.pos.x, p->pos.z - car.pos.z);
    Vec3 pfw{-std::sin(p->yaw), 0, -std::cos(p->yaw)};
    float ahead = dot(Vec3{car.pos.x - p->pos.x, 0, car.pos.z - p->pos.z}, pfw); // > 0: Spieler liegt vor der Streife
    bool stopped = std::fabs(car.speed()) < 1.2f;
    if (pc.state == P_FOLLOW) {
        // Sie braucht eine Weile, bis sie aufgeholt hat. Erst kurz hinter dem Spieler schaltet sie Blaulicht ein.
        if (d > 320 || simTime - pc.t0 > 240) { endCase(p, 45, nullptr); return; }
        if (d < 30 && ahead > 3) {
            p->police = 2;
            pc.state = P_ORDER; pc.t0 = simTime; pc.still = 0; pc.farSince = -1; pc.passed = false;
            logMsg("Polizei: Anhalten angeordnet");
            hud.msgs.push_back({"Polizei: Bitte rechts ranfahren und anhalten!", 6.0f});
        }
    } else if (pc.state == P_ORDER) {
        if (ahead < -10) pc.passed = true; // Streife hat ueberholt und setzt sich vor den Spieler
        pc.still = (stopped && d < 90) ? pc.still + dt : 0;
        if (pc.still > 1.0f) { pc.state = P_STOPPED; pc.stopHold = 0; hud.msgs.push_back({"Verkehrskontrolle", 4.0f}); }
        else if (simTime - pc.t0 > 35) {
            pc.state = P_CHASE; pc.t0 = simTime; pc.farSince = -1; pc.still = 0; pc.passed = false;
            hud.msgs.push_back({"Die Polizei verfolgt dich!", 5.0f});
            script.event("police_ignored");
        }
    } else if (pc.state == P_STOPPED) {
        pc.stopHold += dt;
        if (!stopped && car.speed() > 3.0f && pc.stopHold < 4.0f) { // zu frueh losgefahren
            pc.state = P_CHASE; pc.t0 = simTime; pc.farSince = -1; pc.still = 0; pc.passed = false;
            hud.msgs.push_back({"Du bist abgehauen - Verfolgung!", 5.0f});
            script.event("police_ignored");
        } else if (pc.stopHold > 6.0f) endCase(p, 240, "Kontrolle beendet. Gute Fahrt!");
    } else if (pc.state == P_CHASE) {
        pc.still = (stopped && d < 30) ? pc.still + dt : 0;
        if (pc.still > 3.0f) { pc.state = P_STOPPED; pc.stopHold = 0; hud.msgs.push_back({"Gestoppt - Verkehrskontrolle", 4.0f}); }
        if (d > 220) {
            if (pc.farSince < 0) pc.farSince = simTime;
            else if (simTime - pc.farSince > 15) endCase(p, 90, "Polizei abgeh\xC3\xA4ngt");
        } else pc.farSince = -1;
    }
}
} // namespace

namespace traffic {

int count() { return (int)cars.size(); }

namespace { double lastRemoteT = -100; bool wasPuppet = false; }
bool puppetMode() { return simTime - lastRemoteT < 2.5; }
void snapshot(std::vector<NpcState>& out) {
    for (const AI& c : cars) {
        if (c.puppet) continue;
        NpcState n{};
        n.uid = (uint16_t)c.uid;
        world.toLatLon(c.pos.x, c.pos.z, n.lat, n.lon);
        n.y = c.pos.y;
        n.yaw = (int16_t)clampv((int)std::lround(std::remainder(c.yaw, 2 * PI) * 5000.0f), -32000, 32000);
        n.v = (int16_t)clampv((int)std::lround(c.v * 100.0f), -32000, 32000);
        n.flags = (c.braking ? 1 : 0) | (c.turn < 0 ? 2 : 0) | (c.turn > 0 ? 4 : 0) | (c.passenger ? 8 : 0) | (c.pullover ? 16 : 0);
        n.model = (uint8_t)clampv(c.model, 0, 255); n.kind = (uint8_t)c.kind; n.police = (uint8_t)c.police;
        n.c1[0] = c.paint.r; n.c1[1] = c.paint.g; n.c1[2] = c.paint.b;
        n.c2[0] = c.shirt.r; n.c2[1] = c.shirt.g; n.c2[2] = c.shirt.b;
        out.push_back(n);
    }
}
void applyRemote(const NpcState* s, int n) {
    lastRemoteT = simTime;
    if (!wasPuppet) { cars.clear(); wasPuppet = true; pc = PoliceCase{}; }
    for (int i = 0; i < n; i++) {
        AI* a = nullptr;
        for (AI& c : cars) if (c.puppet && c.uid == s[i].uid) { a = &c; break; }
        if (!a) { cars.push_back(AI{}); a = &cars.back(); a->puppet = true; a->uid = s[i].uid; a->seen = -1; }
        float x, z;
        world.toLocal(s[i].lat, s[i].lon, x, z);
        a->tgt = {x, s[i].y, z};
        a->tyaw = s[i].yaw / 5000.0f;
        a->v = s[i].v / 100.0f;
        if (a->seen < 0 || length(a->pos - a->tgt) > 40) { a->pos = a->tgt; a->yaw = a->tyaw; }
        a->seen = simTime;
        a->braking = (s[i].flags & 1) != 0;
        a->turn = (s[i].flags & 2) ? -1 : (s[i].flags & 4) ? 1 : 0;
        a->passenger = (s[i].flags & 8) != 0; a->pullover = (s[i].flags & 16) != 0;
        a->model = s[i].model; a->kind = s[i].kind; a->police = s[i].police;
        a->paint = Color{s[i].c1[0], s[i].c1[1], s[i].c1[2], 255};
        a->shirt = Color{s[i].c2[0], s[i].c2[1], s[i].c2[2], 255};
    }
}
static void stepPuppets(float dt) {
    for (size_t i = 0; i < cars.size();) {
        AI& c = cars[i];
        if (!c.puppet || simTime - c.seen > 2.0) { cars.erase(cars.begin() + i); continue; }
        Vec3 fwd{-std::sin(c.tyaw), 0, -std::cos(c.tyaw)};
        Vec3 t = c.tgt + fwd * (c.v * (float)std::min(0.3, simTime - c.seen));
        float k = 1.0f - std::exp(-dt * 12.0f);
        c.pos += (t - c.pos) * k;
        c.yaw += std::remainder(c.tyaw - c.yaw, 2 * PI) * k;
        c.wheelRot += c.v / 0.32f * dt;
        i++;
    }
}

void update(float dt) {
    if (builtGen != world.geo().gen) { builtGen = world.geo().gen; cars.clear(); builtVersion = -1; pc = PoliceCase{}; }
    if (wanted <= 0) { cars.clear(); pc = PoliceCase{}; return; }
    if (puppetMode()) { stepPuppets(dt); return; }   // Mitspieler: der Host simuliert den Verkehr
    if (wasPuppet) { wasPuppet = false; cars.clear(); }
    if (builtVersion != world.roadVersion()) { builtVersion = world.roadVersion(); rebuild(); }
    if ((int)cars.size() < wanted) spawn();

    Vec3 pf = car.forward();
    pf.y = 0;
    pf = normalize(pf);
    RoadHit ph = world.nearestRoad(car.pos.x, car.pos.z, 12, 7, car.pos.y - 0.55f);
    if (!cfg.police) { for (size_t i = 0; i < cars.size();) { if (cars[i].police) cars.erase(cars.begin() + i); else i++; } pc = PoliceCase{}; }
    else policeLogic(dt, ph); // kann Fahrzeuge anlegen: deshalb vor dem Sammeln der Teilnehmer

    // --- alle Verkehrsteilnehmer sammeln ---
    std::vector<Obs> obs;
    for (size_t i = 0; i < cars.size(); i++) {
        const AI& c = cars[i];
        obs.push_back({c.pos, {-std::sin(c.yaw), 0, -std::cos(c.yaw)}, c.v, toNode(c.e, c.rev), E[c.e].len - c.s, E[c.e].s.klass, c.ctl, false, (int)i, c.e, c.waited > 3});
    }
    Obs player{car.pos, pf, std::fabs(car.speed()), -1, 1e9f, 5, false, true, -1, -1, false};
    if (ph.found) {
        bool fwdDir = pf.x * ph.dx + pf.z * ph.dz >= 0;
        float ex = fwdDir ? ph.seg->bx : ph.seg->ax, ez = fwdDir ? ph.seg->bz : ph.seg->az;
        auto it = nodeIdx.find(nodeKey(ex, ez));
        if (it != nodeIdx.end()) { player.toNode = it->second; player.dNode = std::hypot(ex - car.pos.x, ez - car.pos.z); }
        player.klass = ph.seg->klass;
    }
    obs.push_back(player);
    { // Fussgaenger auf der Fahrbahn: Autos muessen warten
        std::vector<std::pair<Vec3, Vec3>> pcs;
        ped::crossing(pcs);
        for (auto& pp : pcs) obs.push_back({pp.first, pp.second, 1.4f, -1, 1e9f, 5, false, false, -1, -1, false});
    }
    for (const NetCar& n : netplay::cars())
        if (n.placed) obs.push_back({n.pos, qrot(n.qs, {0, 0, -1}), std::fabs(n.speed), -1, 1e9f, 5, false, false, -1, -1, false});

    for (size_t i = 0; i < cars.size();) {
        AI& c = cars[i];
        if (c.freeBody) { // nach Zusammenstoss: Starrkoerper
            stepFree(c, dt);
            obs[i].pos = c.pos; obs[i].v = c.v; obs[i].fwd = {-std::sin(c.yaw), 0, -std::cos(c.yaw)};
            bool gone = std::hypot(c.pos.x - playerPos().x, c.pos.z - playerPos().z) > 700.0f || c.freeT > 60;
            if (c.v < 1.0f) { c.slowT += dt; if (c.slowT > 1.5f && reattach(c)) { i++; continue; } if (c.slowT > 14) gone = true; } else c.slowT = 0;
            if (gone) { cars.erase(cars.begin() + i); obs.erase(obs.begin() + i); for (Obs& o : obs) if (o.ai > (int)i) o.ai--; continue; }
            i++;
            continue;
        }
        const Edge& ed = E[c.e];
        Vec3 dir = dirOf(c.e, c.rev), fwd{-std::sin(c.yaw), 0, -std::cos(c.yaw)}, right{-fwd.z, 0, fwd.x};
        int N = toNode(c.e, c.rev);
        float dA = ed.len - c.s;
        float vT = std::min(ed.s.maxspeed, 130.0f) / 3.6f * 0.92f;
        bool emergency = c.police == 2, follow = c.police == 3;
        c.pullover = false;
        float latT = 0;
        if (c.kind == 1) vT = std::min(vT, 12.5f); // Moped: maximal ca. 45 km/h
        if (follow) { // unauffaellig hinterher: etwas schneller als erlaubt, haelt sich aber an Ampeln
            float pv = std::fabs(car.speed());
            vT = std::min(std::max(vT * 1.25f, std::min(pv + 4.0f, 24.0f)), 28.0f);
        }
        if (emergency) {
            float pv = std::fabs(car.speed());
            bool order = pc.state == P_ORDER && c.uid == pc.uid;
            if (order && !pc.passed) { vT = std::min(std::max(pv + 9.0f, 16.0f), 34.0f); latT = -2.4f; }        // links ueberholen
            else if (order) { vT = std::max(0.0f, std::min(c.v, pv) - 3.0f); latT = 1.4f; c.pullover = true; }    // vor dem Spieler rechts ranfahren, anhalten
            else {
                vT = std::min(std::max(std::max(vT * 1.6f, 13.0f), pv + 3.0f), pc.state == P_CHASE ? 38.0f : 30.0f);
                if (pc.state == P_STOPPED && c.uid == pc.uid) vT = std::hypot(c.pos.x - car.pos.x, c.pos.z - car.pos.z) < 18 ? 0.0f : 6.0f;
            }
        }
        c.latBias += (latT - c.latBias) * std::min(1.0f, dt * 1.6f);
        c.turn = 0;
        if (c.ne >= 0) { // vor Kurven und beim Abbiegen langsamer, Blinker setzen
            Vec3 nd = dirOf(c.ne, c.nrev);
            float cs = dot(dir, nd);
            if (cs < 0.9f && dA < 8 + c.v * 1.6f) vT = std::min(vT, cs < 0.35f ? 3.6f : 6.5f);
            if (cs < 0.8f && dA < 38 && adj[N].size() >= 3) c.turn = (dir.x * nd.z - dir.z * nd.x) > 0 ? 1 : -1;
        } else if (dA < 25) vT = std::min(vT, stopLaw(dA - 3));

        // --- Abstand zum Vordermann ---
        for (const Obs& o : obs) {
            if (o.ai == (int)i) continue;
            if (emergency && o.player && pc.state == P_ORDER) continue; // ueberholen: Spieler nicht als Vordermann behandeln
            Vec3 rel = o.pos - c.pos;
            float ahead = dot(rel, fwd), lat = dot(rel, right);
            if (ahead > 0.5f && ahead < 9 + c.v * 2.6f && std::fabs(lat) < 1.7f) {
                float gap = ahead - 4.7f;
                vT = std::min(vT, std::max(0.0f, o.v * std::max(0.0f, dot(o.fwd, fwd)) + (gap - 2.2f) * 0.9f));
            }
        }
        // --- Ampeln und Schilder ---
        bool signalAhead = false;
        c.ctl = false;
        if (!emergency) world.forControls(c.pos, 45, [&](const Control& k) {
            if (fwd.x * k.dx + fwd.z * k.dz < 0.8f) return;
            float dd = (k.pos.x - c.pos.x) * k.dx + (k.pos.z - c.pos.z) * k.dz - 2.2f;
            float lat = (k.pos.x - c.pos.x) * (-k.dz) + (k.pos.z - c.pos.z) * k.dx;
            if (dd < -2.5f || dd > 42 || std::fabs(lat) > k.hw + 2) return;
            if (k.type == 0) {
                signalAhead = true;
                int st = World::signalState(k, simTime);
                if (st == 0 || st == 1 || (st == 3 && dd > c.v * 1.4f)) vT = std::min(vT, stopLaw(dd - 0.8f));
            } else {
                c.ctl = true;
                if (k.type == 1 && !c.stopDone) {
                    vT = std::min(vT, stopLaw(dd - 0.5f));
                    if (dd < 3 && c.v < 0.3f) { c.stopTimer += dt; if (c.stopTimer > 1.2f) c.stopDone = true; }
                } else if (k.type == 2) vT = std::min(vT, std::max(4.5f, stopLaw(dd) + 3));
            }
        });
        // --- Vorrang an Kreuzungen ohne Ampel ---
        bool mustStop = false;
        if (adj[N].size() >= 3 && dA < 32 && !signalAhead && !emergency) {
            for (const Obs& o : obs) {
                if (o.ai == (int)i) continue;
                bool inJunction = std::hypot(o.pos.x - nodeP[N].x, o.pos.z - nodeP[N].z) < 7.5f && o.edge != c.e && o.v > 0.5f;
                if (inJunction && dA > 4 && dA < 24 && dot(o.pos - c.pos, fwd) > 0) { mustStop = true; continue; }
                if (o.toNode != N || o.dNode > 34 || o.edge == c.e) continue;
                bool fromRight = dot(o.pos - c.pos, Vec3{-dir.z, 0, dir.x}) > 0 && std::fabs(dot(dir, o.fwd)) < 0.75f;
                bool oPrio = c.ctl ? true : o.ctl ? false : (o.klass != ed.s.klass ? o.klass < ed.s.klass : fromRight);
                float tO = o.dNode / std::max(o.v, 2.0f);
                if (oPrio && tO < 5.0f && !o.yielding) mustStop = true;
                // Spieler nimmt einem bevorrechtigten Fahrzeug die Vorfahrt
                if (!oPrio && o.player && o.dNode < 9 && o.v > 2.5f && c.v > 3 && dA < 24 && dA / std::max(c.v, 2.0f) < 3.5f &&
                    std::fabs(dot(dir, o.fwd)) < 0.75f && simTime > priorityCooldown) {
                    priorityCooldown = simTime + 8;
                    script.event("priority");
                }
            }
        }
        c.yielding = mustStop && c.waited < 7;
        if (c.yielding) {
            vT = std::min(vT, stopLaw(dA - 7.5f));
            if (c.v < 0.4f) c.waited += dt;
        }
        if (c.crashWait > 0) { c.crashWait -= dt; vT = 0; }

        // --- Bewegung ---
        if (c.v < vT) c.v += std::min(1.9f * dt, vT - c.v);
        else c.v -= std::min((vT < c.v - 3 ? 7.0f : 3.2f) * dt, c.v - vT);
        c.braking = vT < c.v - 0.2f || c.v < 0.2f;
        Vec3 target = pathPoint(c, clampv(3.5f + c.v * 0.55f, 4.0f, 12.0f));
        float want = std::atan2(-(target.x - c.pos.x), -(target.z - c.pos.z));
        float err = std::remainder(want - c.yaw, 2 * PI);
        float rate = clampv(c.v / 5.2f, 0.25f, 2.6f);
        float dy = clampv(err, -rate * dt, rate * dt);
        c.yaw += dy;
        { float dq = std::max(dt, 0.001f), yr = dy / dq; // Insassen: Beschleunigung und Kurvenfliehkraft
          c.sg.step(clampv((c.v - c.prevV) / dq, -10.0f, 10.0f), dt);
          c.sl.step(clampv(-c.v * yr, -10.0f, 10.0f), dt);
          c.prevV = c.v;
          c.headYaw += (clampv(yr * 0.7f, -0.6f, 0.6f) - c.headYaw) * std::min(1.0f, dt * 5.0f); }
        c.steerVis += (clampv(dt > 0 ? -dy / dt * 2.6f / std::max(c.v, 1.0f) : 0, -0.55f, 0.55f) - c.steerVis) * std::min(1.0f, dt * 8);
        c.pos += Vec3{-std::sin(c.yaw), 0, -std::cos(c.yaw)} * (c.v * dt);
        float h;
        if (world.surface(c.pos.x, c.pos.z, c.pos.y + 1.2f, h)) { float d = h - c.pos.y, lim = 0.45f * std::fabs(c.v) * dt + 0.01f; if (std::fabs(d) > 2.5f) c.pos.y = h; else c.pos.y += clampv(d * std::min(1.0f, dt * 12), -lim, lim); }   // Steigung begrenzt: keine Spruenge an Rampen
        c.wheelRot = std::fmod(c.wheelRot + c.v / 0.316f * dt, 2 * PI);
        c.s = dot(c.pos - nodeP[fromNode(c.e, c.rev)], dir);
        c.stuck = c.v < 0.2f ? c.stuck + dt : 0;

        bool remove = std::hypot(c.pos.x - playerPos().x, c.pos.z - playerPos().z) > (c.police == 2 ? 900.0f : 440.0f) || c.stuck > 45;
        if (!remove && c.s >= ed.len - 0.4f) { // naechster Abschnitt
            if (c.ne < 0) remove = true;
            else {
                c.e = c.ne; c.rev = c.nrev;
                c.s = dot(c.pos - nodeP[fromNode(c.e, c.rev)], dirOf(c.e, c.rev));
                c.waited = 0; c.stopTimer = 0; c.stopDone = false;
                chooseNext(c);
            }
        }
        if (remove) { cars.erase(cars.begin() + i); obs.erase(obs.begin() + i); for (Obs& o : obs) if (o.ai > (int)i) o.ai--; continue; }
        obs[i].pos = c.pos;
        i++;
    }

    // --- Zusammenstoesse: Spieler gegen KI, Spieler gegen Mitspieler, KI gegen KI (nur mit freien Fahrzeugen) ---
    {
        Body P;
        P.fwd = pf; P.pos = car.pos; P.vel = Vec3{car.vel.x, 0, car.vel.z}; P.w = car.angVel.y;
        if (car.kind > 0) { P.hl = 1.0f; P.hw = 0.4f; P.m = 220; }
        bool pHit = false;
        float hardest = 0;
        for (AI& c : cars) {
            if (std::fabs(c.pos.x - P.pos.x) > 6 || std::fabs(c.pos.z - P.pos.z) > 6 || std::fabs(car.pos.y - 0.54f - c.pos.y) > 1.5f) continue;
            Body B = aiBody(c);
            Vec3 n, pt;
            float pen;
            if (!obbContact(P, B, n, pen, pt)) continue;
            hardest = std::max(hardest, resolveContact(P, B, n, pen, pt));
            storeAi(c, B);
            pHit = true;
        }
        for (const NetCar& nc : netplay::cars()) {
            if (!nc.placed || std::fabs(nc.pos.x - P.pos.x) > 6 || std::fabs(nc.pos.z - P.pos.z) > 6 || std::fabs(car.pos.y - nc.pos.y) > 1.5f) continue;
            Vec3 f = qrot(nc.qs, {0, 0, -1});
            f.y = 0;
            if (length(f) < 0.1f) continue;
            Body B;
            B.fwd = normalize(f); B.pos = nc.pos; B.vel = B.fwd * nc.speed;
            if (nc.kind > 0) { B.hl = 1.0f; B.hw = 0.4f; B.m = 220; }
            Vec3 n, pt;
            float pen;
            if (!obbContact(P, B, n, pen, pt)) continue;
            hardest = std::max(hardest, resolveContact(P, B, n, pen, pt, false));
            pHit = true;
        }
        if (pHit) {
            car.pos.x = P.pos.x; car.pos.z = P.pos.z;
            car.vel.x = P.vel.x; car.vel.z = P.vel.z;
            car.angVel.y = P.w;
            if (hardest > 1.5f && simTime > crashCooldown) { crashCooldown = simTime + 2.5; script.event("crash"); }
        }
        // KI gegen KI: nur wenn mindestens ein Fahrzeug frei ist (ein Unfall zieht andere mit hinein)
        for (size_t i = 0; i < cars.size(); i++)
            for (size_t j = i + 1; j < cars.size(); j++) {
                AI &a = cars[i], &b = cars[j];
                if (!a.freeBody && !b.freeBody) continue;
                if (std::fabs(a.pos.x - b.pos.x) > 6 || std::fabs(a.pos.z - b.pos.z) > 6 || std::fabs(a.pos.y - b.pos.y) > 1.5f) continue;
                Body A = aiBody(a), B = aiBody(b);
                Vec3 n, pt;
                float pen;
                if (!obbContact(A, B, n, pen, pt)) continue;
                resolveContact(A, B, n, pen, pt);
                storeAi(a, A); storeAi(b, B);
            }
    }
}

void draw(const Mat4& vp, Vec3 cam, float maxDist) {
    bool blink = std::fmod(simTime, 0.74) < 0.37;
    for (const AI& c : cars) {
        if (length(c.pos - cam) > maxDist) continue;
        CarVisual v;
        v.M = fromQuatPos(qaxis({0, 1, 0}, c.yaw), c.pos + Vec3{0, 0.54f, 0});
        v.steer = c.steerVis;
        for (float& r : v.wheelRot) r = c.wheelRot;
        v.paint = c.paint; v.tinted = c.police == 0; v.model = c.model; v.siren = c.police == 2;
        v.kind = c.police ? 0 : c.kind;
        if (v.kind == 0) { v.driver = true; v.passenger = c.passenger && !c.police; v.occX = c.sl.x; v.occZ = -c.sg.x; v.headYaw = c.headYaw; v.occShirt = c.police ? Color{60, 80, 120, 255} : c.shirt; }
        v.lean = clampv(-c.steerVis * c.v * 0.16f, -0.55f, 0.55f);
        v.brake = c.braking;
        v.lights = envir::night() > 0.55f || c.police == 2;
        v.blinkL = (c.turn < 0 || c.pullover) && blink;
        v.blinkR = (c.turn > 0 || c.pullover) && blink;
        carDraw(v, vp, false, nullptr);
    }
}


// Verstoss des Spielers melden (0 rote Ampel, 1 Stoppschild, 2 Unfall)
void offence(int kind) {
    if (!cfg.police) return;
    static const float w[3] = {2.2f, 1.2f, 1.8f};
    AI* wt = witness(kind == 2 ? 80.0f : 55.0f); // nur wenn eine Streife in der Naehe war und es sehen konnte
    if (!wt) return;
    pc.heat += w[clampv(kind, 0, 2)];
    pc.witness = wt->uid;
}
std::string policeStatus() {
    const AI* p = nullptr;
    for (const AI& c : cars) if (c.uid == pc.uid && c.police > 0) p = &c;
    std::string where;
    if (p) {
        Vec3 f = car.forward();
        f.y = 0;
        f = normalize(f);
        float d = std::hypot(p->pos.x - car.pos.x, p->pos.z - car.pos.z);
        bool behind = dot(p->pos - car.pos, f) < 0;
        char b[64];
        snprintf(b, sizeof b, "  \xC2\xB7  %d m %s", (int)(std::round(d / 10.0f) * 10), behind ? "hinter dir" : "vor dir");
        where = b;
    }
    switch (pc.state) {
    case P_ORDER: return "Polizei: Bitte rechts ranfahren und anhalten" + where;
    case P_CHASE: return "Polizei verfolgt dich" + where;
    case P_STOPPED: return "Verkehrskontrolle";
    default: return "";
    }
}
// Position und Alarmzustand aller Polizeiautos (fuer die Minimap)
void policeMarks(std::vector<std::pair<Vec3, bool>>& out) {
    for (const AI& c : cars) if (c.police == 2) out.push_back({c.pos, true}); // Streifen und Verfolger ohne Blaulicht sind nicht zu sehen
}
bool policeActive() { return pc.state != P_NONE; }
float sirenLevel() { // Lautstaerke der naechsten Sirene (0..1)
    float best = 0;
    for (const AI& c : cars) {
        if (c.police != 2) continue;
        float d = std::hypot(c.pos.x - car.pos.x, c.pos.z - car.pos.z);
        best = std::max(best, clampv(1.0f - d / 320.0f, 0.0f, 1.0f));
    }
    return best * best;
}
} // namespace traffic

namespace traffic {
// Trifft ein fahrendes KI-Auto den Punkt p (Fussgaenger)? Liefert die Fahrzeuggeschwindigkeit als Stossrichtung.
bool hitTest(Vec3 p, float r, Vec3& vel) {
    for (const AI& c : cars) {
        if (c.v < 2.0f) continue;
        Vec3 fwd{-std::sin(c.yaw), 0, -std::cos(c.yaw)}, right{-fwd.z, 0, fwd.x};
        Vec3 rel = p - c.pos;
        float along = dot(rel, fwd), lat = dot(rel, right);
        float h = p.y - c.pos.y;
        if (std::fabs(along) < 2.3f + r && std::fabs(lat) < 0.95f + r && h > -1.2f && h < 1.6f) { vel = fwd * c.v; return true; }
    }
    return false;
}
} // namespace traffic
