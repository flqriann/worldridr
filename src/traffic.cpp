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
    bool braking = false, ctl = false, stopDone = false, yielding = false;
    int turn = 0;
};
std::vector<AI> cars;

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
    return a + dir * d + right * laneOffset(E[e].s);
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

void spawn() {
    if (E.empty()) return;
    std::uniform_int_distribution<int> pick(0, (int)E.size() - 1);
    static const Color pal[10] = {{40, 90, 190, 255}, {228, 228, 232, 255}, {28, 30, 34, 255}, {150, 154, 162, 255}, {230, 170, 30, 255},
                                  {40, 120, 80, 255}, {120, 30, 36, 255}, {90, 96, 104, 255}, {200, 205, 210, 255}, {30, 60, 110, 255}};
    for (int tries = 0; tries < 40; tries++) {
        int e = pick(rng);
        const Edge& ed = E[e];
        if (ed.len < 8) continue;
        Vec3 mid{(ed.s.ax + ed.s.bx) * 0.5f, 0, (ed.s.az + ed.s.bz) * 0.5f};
        float dp = std::hypot(mid.x - car.pos.x, mid.z - car.pos.z);
        if (dp < 80 || dp > 330) continue;
        AI c;
        c.e = e;
        c.rev = (rng() & 1) != 0;
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
        cars.push_back(c);
        return;
    }
}

float stopLaw(float dist) { return std::sqrt(2.0f * 2.6f * std::max(0.0f, dist)); }
} // namespace

namespace traffic {

int count() { return (int)cars.size(); }

void update(float dt) {
    if (builtGen != world.geo().gen) { builtGen = world.geo().gen; cars.clear(); builtVersion = -1; }
    if (wanted <= 0) { cars.clear(); return; }
    if (builtVersion != world.roadVersion()) { builtVersion = world.roadVersion(); rebuild(); }
    if ((int)cars.size() < wanted) spawn();

    // --- alle Verkehrsteilnehmer sammeln ---
    std::vector<Obs> obs;
    for (size_t i = 0; i < cars.size(); i++) {
        const AI& c = cars[i];
        obs.push_back({c.pos, {-std::sin(c.yaw), 0, -std::cos(c.yaw)}, c.v, toNode(c.e, c.rev), E[c.e].len - c.s, E[c.e].s.klass, c.ctl, false, (int)i, c.e, c.waited > 3});
    }
    Vec3 pf = car.forward();
    pf.y = 0;
    pf = normalize(pf);
    Obs player{car.pos, pf, std::fabs(car.speed()), -1, 1e9f, 5, false, true, -1, -1, false};
    RoadHit ph = world.nearestRoad(car.pos.x, car.pos.z, 12);
    if (ph.found) {
        bool fwdDir = pf.x * ph.dx + pf.z * ph.dz >= 0;
        float ex = fwdDir ? ph.seg->bx : ph.seg->ax, ez = fwdDir ? ph.seg->bz : ph.seg->az;
        auto it = nodeIdx.find(nodeKey(ex, ez));
        if (it != nodeIdx.end()) { player.toNode = it->second; player.dNode = std::hypot(ex - car.pos.x, ez - car.pos.z); }
        player.klass = ph.seg->klass;
    }
    obs.push_back(player);
    for (const NetCar& n : netplay::cars())
        if (n.placed) obs.push_back({n.pos, qrot(n.qs, {0, 0, -1}), std::fabs(n.speed), -1, 1e9f, 5, false, false, -1, -1, false});

    for (size_t i = 0; i < cars.size();) {
        AI& c = cars[i];
        const Edge& ed = E[c.e];
        Vec3 dir = dirOf(c.e, c.rev), fwd{-std::sin(c.yaw), 0, -std::cos(c.yaw)}, right{-fwd.z, 0, fwd.x};
        int N = toNode(c.e, c.rev);
        float dA = ed.len - c.s;
        float vT = std::min(ed.s.maxspeed, 130.0f) / 3.6f * 0.92f;
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
        world.forControls(c.pos, 45, [&](const Control& k) {
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
        if (adj[N].size() >= 3 && dA < 32 && !signalAhead) {
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
        c.steerVis += (clampv(dt > 0 ? -dy / dt * 2.6f / std::max(c.v, 1.0f) : 0, -0.55f, 0.55f) - c.steerVis) * std::min(1.0f, dt * 8);
        c.pos += Vec3{-std::sin(c.yaw), 0, -std::cos(c.yaw)} * (c.v * dt);
        float h;
        if (world.surface(c.pos.x, c.pos.z, c.pos.y + 1.2f, h)) c.pos.y += (h - c.pos.y) * std::min(1.0f, dt * 12);
        c.wheelRot = std::fmod(c.wheelRot + c.v / 0.316f * dt, 2 * PI);
        c.s = dot(c.pos - nodeP[fromNode(c.e, c.rev)], dir);
        c.stuck = c.v < 0.2f ? c.stuck + dt : 0;

        bool remove = std::hypot(c.pos.x - car.pos.x, c.pos.z - car.pos.z) > 440 || c.stuck > 45;
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

    // --- Zusammenstoss mit dem Spieler: zwei Kreise pro Fahrzeug ---
    for (AI& c : cars) {
        Vec3 f{-std::sin(c.yaw), 0, -std::cos(c.yaw)};
        for (float sa : {-1.2f, 1.2f})
            for (float sb : {-1.2f, 1.2f}) {
                Vec3 a = car.pos + pf * sa, b = c.pos + f * sb;
                float dx = a.x - b.x, dz = a.z - b.z, d = std::hypot(dx, dz);
                if (d >= 1.9f || d < 1e-3f || std::fabs(car.pos.y - 0.54f - c.pos.y) > 1.5f) continue;
                float pen = 1.9f - d;
                Vec3 n{dx / d, 0, dz / d};
                car.pos += n * pen;
                float vn = dot(car.vel, n);
                if (vn < 0) car.vel += n * (-vn * 1.2f);
                car.vel = car.vel * 0.9f;
                c.v = 0; c.crashWait = 3;
                if (simTime > crashCooldown && (std::fabs(vn) > 0.8f || c.v > 1)) { crashCooldown = simTime + 2.5; script.event("crash"); }
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
        v.paint = c.paint;
        v.brake = c.braking;
        v.blinkL = c.turn < 0 && blink;
        v.blinkR = c.turn > 0 && blink;
        carDraw(v, vp, false, nullptr);
    }
}

} // namespace traffic
