// Fussgaenger: laufen auf den Gehsteigen entlang des Strassennetzes, ueberqueren an Kreuzungen die Fahrbahn
// (Autos der KI halten dafuer an) und werden bei einem Zusammenstoss mit dem Spieler umgestossen.
#include "game.h"
#include "ragdoll.h"
#include <array>
#include <cstring>
#include <random>
#include <unordered_map>

namespace ped {
int wanted = 10;
}

namespace {
struct Edge { RoadSeg s; int n0, n1; float len, dx, dz; };
std::vector<Edge> E;
std::vector<std::vector<int>> adj;
std::vector<Vec3> nodeP;
std::unordered_map<uint64_t, int> nodeIdx;
int builtVersion = -1, builtGen = -1;
std::mt19937 rng{777};

struct Ped {
    int e = 0;
    bool rev = false;
    float s = 0, side = 1, speed = 1.4f, phase = 0, yaw = 0;
    Vec3 pos;
    Color shirt, pants, skin, hair;
    bool crossing = false, down = false, frozen = false;
    Ragdoll rag;
    Vec3 cFrom, cTo;
    float cT = 0, cDur = 5, downT = 0;
    int nextEdge = 0;
    bool nextRev = false;
    int uid = 0, ski = 0, hai = 0;
    bool puppet = false;
    Vec3 tgt; float tyaw = 0; double seen = 0; float v = 0;
};
int nextPedUid = 1;
const Color SKINS[4] = {{236, 190, 160, 255}, {210, 160, 120, 255}, {160, 110, 80, 255}, {110, 76, 56, 255}};
const Color HAIRS[4] = {{40, 30, 24, 255}, {90, 62, 34, 255}, {200, 170, 100, 255}, {120, 120, 120, 255}};
double lastRemoteT = -100; bool wasPuppet = false;
std::vector<Ped> peds;
Batch pb;
float spawnTimer = 0;

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
Vec3 dirOf(int e, bool rev) { return rev ? Vec3{-E[e].dx, 0, -E[e].dz} : Vec3{E[e].dx, 0, E[e].dz}; }
int fromNode(int e, bool rev) { return rev ? E[e].n1 : E[e].n0; }
int toNode(int e, bool rev) { return rev ? E[e].n0 : E[e].n1; }
float offsetOf(int e) { return E[e].s.hw + 1.1f; }
Mat4 rotY(float a) { return fromQuatPos(qaxis({0, 1, 0}, a), {}); }
Mat4 rotX(float a) { return fromQuatPos(qaxis({1, 0, 0}, a), {}); }

void rebuild() {
    std::vector<RoadSeg> segs;
    world.collectSegs(segs);
    E.clear(); adj.clear(); nodeP.clear(); nodeIdx.clear();
    for (const RoadSeg& s : segs) {
        if (s.klass < 2 || s.klass > 5) continue; // nur Strassen mit Gehsteig
        float dx = s.bx - s.ax, dz = s.bz - s.az, len = std::hypot(dx, dz);
        if (len < 1.0f) continue;
        Edge ed{s, nodeOf(s.ax, s.az), nodeOf(s.bx, s.bz), len, dx / len, dz / len};
        if (ed.n0 == ed.n1) continue;
        adj[ed.n0].push_back((int)E.size());
        adj[ed.n1].push_back((int)E.size());
        E.push_back(ed);
    }
    peds.clear(); // Verbindungen haben sich geaendert: neu verteilen
}

Vec3 walkTarget(const Ped& p) {
    Vec3 d = dirOf(p.e, p.rev), right{-d.z, 0, d.x};
    float sd = p.side;
    uint8_t rg = E[p.e].s.ring; // im Kreisverkehr immer auf dem Gehsteig ausserhalb, nie auf der Mittelinsel
    if (rg) sd = (rg == 1) ? (p.rev ? -1.0f : 1.0f) : (p.rev ? 1.0f : -1.0f);
    return nodeP[fromNode(p.e, p.rev)] + d * p.s + right * (sd * offsetOf(p.e));
}


// ---------- Eigenes Fussgaengermodell (assets/pedestrians/pedestrian_walk.obj) ----------
// Das Modell hat keine Materialzuordnung pro Flaeche; die Bereiche (Haut, Hemd, Hose, Schuhe, Haare) werden nach der Hoehe
// erkannt und je Fussgaenger eingefaerbt. Die Beine schwingen um die Huefte.
struct PedTri { Vec3 p[3], n[3]; uint8_t region, part; }; // region: 0 Haut, 1 Hemd, 2 Hose, 3 Schuh, 4 Sohle, 5 Haar; part: 0 Koerper, 1/2 Bein +x/-x, 3/4 Arm +x/-x
std::vector<PedTri> pedTris;
bool pedModelTried = false;
Color pedShoe{212, 200, 176, 255}, pedSole{40, 35, 30, 255};

void loadPedModel() {
    if (pedModelTried) return;
    pedModelTried = true;
    FILE* f = fopen("assets/pedestrians/pedestrian_walk.obj", "rb");
    if (!f) return;
    std::string text;
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
    fclose(f);
    std::vector<Vec3> P, N;
    std::vector<std::array<int, 6>> faces;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t e = text.find('\n', pos);
        if (e == std::string::npos) e = text.size();
        std::string line = text.substr(pos, e - pos);
        pos = e + 1;
        const char* s = line.c_str();
        if (!strncmp(s, "v ", 2)) { float x, y, z; if (sscanf(s + 2, "%f %f %f", &x, &y, &z) == 3) P.push_back({x, y, z}); }
        else if (!strncmp(s, "vn ", 3)) { float x, y, z; if (sscanf(s + 3, "%f %f %f", &x, &y, &z) == 3) N.push_back({x, y, z}); }
        else if (!strncmp(s, "f ", 2)) {
            int a[3] = {0, 0, 0}, b[3] = {0, 0, 0};
            const char* p = s + 2;
            int k = 0;
            while (*p && k < 3) {
                char* end;
                a[k] = (int)strtol(p, &end, 10); p = end;
                if (*p == '/') { p++; if (*p != '/') strtol(p, &end, 10), p = end; if (*p == '/') { p++; b[k] = (int)strtol(p, &end, 10); p = end; } }
                while (*p == ' ') p++;
                k++;
            }
            if (k == 3) faces.push_back({a[0], a[1], a[2], b[0], b[1], b[2]});
        }
    }
    if (faces.empty()) return;
    for (auto& fc : faces) {
        PedTri t;
        bool ok = true;
        Vec3 c{};
        for (int q = 0; q < 3; q++) {
            if (fc[q] < 1 || fc[q] > (int)P.size()) { ok = false; break; }
            Vec3 p = P[fc[q] - 1], nn = fc[3 + q] >= 1 && fc[3 + q] <= (int)N.size() ? N[fc[3 + q] - 1] : Vec3{0, 1, 0};
            t.p[q] = {-p.x, p.y, -p.z};   // Modell blickt nach +z, unser Vorwaerts ist -z
            t.n[q] = normalize({-nn.x, nn.y, -nn.z});
            c += p * (1.0f / 3);
        }
        if (!ok) continue;
        float ax = std::fabs(c.x);
        if (c.y < 0.045f) t.region = 4;
        else if (c.y < 0.12f) t.region = 3;
        else if (c.y < 0.93f) t.region = (ax > 0.2f && c.y > 0.7f) ? 0 : 2;   // Haende neben der Hose
        else if (c.y < 1.5f) t.region = (ax > 0.2f && c.y < 1.12f) ? 0 : 1;  // Unterarme/Haende nackt, sonst Hemd
        else if (c.y > 1.72f || (c.y > 1.62f && c.z < 0.0f)) t.region = 5;
        else t.region = 0;
        t.part = (c.y < 0.93f && ax < 0.2f) ? (c.x > 0 ? 1 : 2) : ((ax > 0.2f && c.y > 0.62f && c.y < 1.52f) ? (c.x > 0 ? 3 : 4) : 0); // 1/2 Beine, 3/4 Arme
        pedTris.push_back(t);
    }
    logMsg("Fussgaengermodell: %d Dreiecke", (int)pedTris.size());
}
void spawn() {
    if (E.empty()) return;
    std::uniform_int_distribution<int> pick(0, (int)E.size() - 1);
    static const Color shirts[8] = {{196, 70, 62, 255}, {60, 100, 170, 255}, {220, 220, 214, 255}, {70, 140, 90, 255}, {230, 180, 60, 255}, {90, 90, 100, 255}, {170, 90, 150, 255}, {50, 50, 56, 255}};
    static const Color pantsC[5] = {{40, 46, 70, 255}, {30, 30, 34, 255}, {96, 84, 70, 255}, {70, 76, 90, 255}, {120, 120, 126, 255}};
    static const Color skins[4] = {{236, 190, 160, 255}, {210, 160, 120, 255}, {160, 110, 80, 255}, {110, 76, 56, 255}};
    static const Color hairs[4] = {{40, 30, 24, 255}, {90, 62, 34, 255}, {200, 170, 100, 255}, {120, 120, 120, 255}};
    for (int tries = 0; tries < 30; tries++) {
        const Edge& ed = E[pick(rng)];
        int e = (int)(&ed - E.data());
        float s = std::uniform_real_distribution<float>(0, ed.len)(rng);
        Ped p;
        p.e = e;
        p.rev = (rng() & 1) != 0;
        p.s = s;
        p.side = (rng() & 1) ? 1.0f : -1.0f;
        p.pos = walkTarget(p);
        float dp = std::hypot(p.pos.x - playerPos().x, p.pos.z - playerPos().z);
        if (dp < 25 || dp > 170) continue;
        float h;
        if (!world.surface(p.pos.x, p.pos.z, 1e9f, h)) continue;
        p.pos.y = h + 0.05f;
        p.speed = 1.15f + (rng() % 100) / 100.0f * 0.6f;
        p.phase = (rng() % 628) / 100.0f;
        p.shirt = shirts[rng() % 8]; p.pants = pantsC[rng() % 5]; p.ski = rng() % 4; p.hai = rng() % 4; p.skin = SKINS[p.ski]; p.hair = HAIRS[p.hai]; p.uid = nextPedUid++;
        peds.push_back(p);
        return;
    }
}

void nextSegment(Ped& p) {
    int n = toNode(p.e, p.rev);
    std::vector<std::pair<int, bool>> cand;
    for (int e2 : adj[n]) {
        if (e2 == p.e) continue;
        cand.push_back({e2, E[e2].n1 == n});
    }
    if (cand.empty()) { p.rev = !p.rev; p.s = 0; p.side = -p.side; return; } // Sackgasse: umkehren
    auto c = cand[rng() % cand.size()];
    int crossingNow = 0;
    for (const Ped& o : peds) if (o.crossing) crossingNow++;
    if (adj[n].size() >= 3 && crossingNow < 3 && !E[p.e].s.ring && !E[c.first].s.ring && rng() % 100 < 28) { // Strasse ueberqueren
        Vec3 d2 = dirOf(c.first, c.second), r2{-d2.z, 0, d2.x};
        float off = offsetOf(c.first);
        p.crossing = true;
        p.cFrom = nodeP[n] + d2 * 3.0f + r2 * (p.side * off);
        p.cTo = nodeP[n] + d2 * 3.0f - r2 * (p.side * off);
        p.cT = 0;
        p.cDur = 2 * off / 1.35f;
        p.nextEdge = c.first; p.nextRev = c.second;
        return;
    }
    p.e = c.first; p.rev = c.second; p.s = 0;
}
} // namespace

namespace ped {

bool puppetMode() { return simTime - lastRemoteT < 2.5; }
void snapshot(std::vector<NpcState>& out) {
    for (const Ped& p : peds) {
        if (p.puppet) continue;
        NpcState n{};
        n.uid = (uint16_t)p.uid;
        world.toLatLon(p.pos.x, p.pos.z, n.lat, n.lon);
        n.y = p.pos.y;
        n.yaw = (int16_t)clampv((int)std::lround(std::remainder(p.yaw, 2 * PI) * 5000.0f), -32000, 32000);
        n.v = (int16_t)std::lround(p.speed * 100.0f);
        n.flags = (p.down ? 32 : 0) | (p.crossing ? 64 : 0);
        n.model = (uint8_t)(p.ski | (p.hai << 4));
        n.c1[0] = p.shirt.r; n.c1[1] = p.shirt.g; n.c1[2] = p.shirt.b;
        n.c2[0] = p.pants.r; n.c2[1] = p.pants.g; n.c2[2] = p.pants.b;
        out.push_back(n);
    }
}
void applyRemote(const NpcState* s, int n) {
    lastRemoteT = simTime;
    if (!wasPuppet) { peds.clear(); wasPuppet = true; }
    for (int i = 0; i < n; i++) {
        Ped* a = nullptr;
        for (Ped& p : peds) if (p.puppet && p.uid == s[i].uid) { a = &p; break; }
        if (!a) { peds.push_back(Ped{}); a = &peds.back(); a->puppet = true; a->uid = s[i].uid; a->seen = -1; }
        float x, z;
        world.toLocal(s[i].lat, s[i].lon, x, z);
        Vec3 t{x, s[i].y, z};
        a->tyaw = s[i].yaw / 5000.0f; a->v = s[i].v / 100.0f; a->speed = a->v;
        a->shirt = Color{s[i].c1[0], s[i].c1[1], s[i].c1[2], 255}; a->pants = Color{s[i].c2[0], s[i].c2[1], s[i].c2[2], 255};
        a->skin = SKINS[s[i].model & 3]; a->hair = HAIRS[(s[i].model >> 4) & 3];
        a->crossing = (s[i].flags & 64) != 0;
        if (s[i].flags & 32) { // vom Auto erfasst: ab hier lokal als Ragdoll
            if (!a->down) {
                a->down = true; a->downT = 0; a->rag.shirt = a->shirt; a->rag.pants = a->pants; a->rag.skin = a->skin; a->rag.hair = a->hair;
                Vec3 fwd{-std::sin(a->tyaw), 0, -std::cos(a->tyaw)}; a->rag.start(a->pos, a->yaw, fwd * 6.0f);
            }
            a->seen = simTime;
            continue;
        }
        a->tgt = t;
        if (a->seen < 0 || length(a->pos - t) > 30) { a->pos = t; a->yaw = a->tyaw; }
        a->seen = simTime;
    }
}
static void stepPuppets(float dt) {
    for (size_t i = 0; i < peds.size();) {
        Ped& p = peds[i];
        if (!p.puppet || simTime - p.seen > 2.0 || (p.down && p.downT > 45)) { peds.erase(peds.begin() + i); continue; }
        if (p.down) { p.rag.step(dt); p.downT += dt; p.pos = p.rag.center(); i++; continue; }
        Vec3 fwd{-std::sin(p.tyaw), 0, -std::cos(p.tyaw)};
        Vec3 t = p.tgt + fwd * (p.v * (float)std::min(0.3, simTime - p.seen));
        float k = 1.0f - std::exp(-dt * 10.0f);
        p.pos += (t - p.pos) * k;
        p.yaw += std::remainder(p.tyaw - p.yaw, 2 * PI) * k;
        p.phase += dt * p.v * 4.6f;
        i++;
    }
}

void update(float dt) {
    if (builtGen != world.geo().gen) { builtGen = world.geo().gen; peds.clear(); builtVersion = -1; }
    if (wanted <= 0) { peds.clear(); return; }
    if (puppetMode()) { stepPuppets(dt); return; }
    if (wasPuppet) { wasPuppet = false; peds.clear(); }
    if (builtVersion != world.roadVersion()) { builtVersion = world.roadVersion(); rebuild(); }
    spawnTimer -= dt;
    if ((int)peds.size() < wanted && spawnTimer <= 0) { spawnTimer = 0.25f; spawn(); }
    Vec3 cf = car.forward();
    for (size_t i = 0; i < peds.size();) {
        Ped& p = peds[i];
        float dCar = std::hypot(p.pos.x - car.pos.x, p.pos.z - car.pos.z);
        if (std::hypot(p.pos.x - playerPos().x, p.pos.z - playerPos().z) > 230 || (p.down && p.downT > 45)) { peds.erase(peds.begin() + i); continue; }
        if (p.down) { p.rag.step(dt); p.downT += dt; p.pos = p.rag.center(); i++; continue; }
        { Vec3 hv; if (!p.frozen && traffic::hitTest(p.pos, 0.4f, hv)) { // von einem KI-Auto erfasst
            p.down = true; p.downT = 0; p.rag.shirt = p.shirt; p.rag.pants = p.pants; p.rag.skin = p.skin; p.rag.hair = p.hair; p.rag.start(p.pos, p.yaw, hv * 0.9f); i++; continue; } }
        if (p.frozen) { p.phase += dt * 4.0f; i++; continue; } // Testfigur
        Vec3 moveDir{0, 0, 0};
        if (p.crossing) {
            p.cT += dt / p.cDur;
            Vec3 prev = p.pos;
            p.pos.x = p.cFrom.x + (p.cTo.x - p.cFrom.x) * std::min(1.0f, p.cT);
            p.pos.z = p.cFrom.z + (p.cTo.z - p.cFrom.z) * std::min(1.0f, p.cT);
            moveDir = normalize({p.cTo.x - p.cFrom.x, 0, p.cTo.z - p.cFrom.z});
            if (p.cT >= 1.0f) {
                p.crossing = false;
                p.e = p.nextEdge; p.rev = p.nextRev; p.s = 3.0f; p.side = -p.side;
            }
            (void)prev;
        } else {
            p.s += p.speed * dt;
            if (p.s >= E[p.e].len) nextSegment(p);
            if (!p.crossing) {
                Vec3 tgt = walkTarget(p);
                float k = std::min(1.0f, dt * 5.0f);
                Vec3 old = p.pos;
                p.pos.x += (tgt.x - p.pos.x) * k;
                p.pos.z += (tgt.z - p.pos.z) * k;
                moveDir = dirOf(p.e, p.rev);
                (void)old;
            }
        }
        if (length(moveDir) > 0.01f) {
            float want = std::atan2(-moveDir.x, -moveDir.z), err = std::remainder(want - p.yaw, 2 * PI);
            p.yaw += err * std::min(1.0f, dt * 8.0f);
        }
        float h;
        if (world.surface(p.pos.x, p.pos.z, p.pos.y + 1.5f, h)) p.pos.y = h + 0.05f;
        p.phase += dt * p.speed * 4.6f;
        // Zusammenstoss mit dem Spieler
        float dy = std::fabs(p.pos.y - (car.pos.y - 0.5f));
        if (dCar < 2.1f && dy < 2.0f) {
            Vec3 rel{p.pos.x - car.pos.x, 0, p.pos.z - car.pos.z};
            float along = dot(rel, normalize({cf.x, 0, cf.z}));
            float lat = std::fabs(rel.x * cf.z - rel.z * cf.x) / std::max(0.01f, std::hypot(cf.x, cf.z));
            if (std::fabs(along) < 2.4f && lat < 1.15f && std::fabs(car.speed()) > 1.5f) {
                p.down = true; p.downT = 0;
                Vec3 imp = car.vel; float il = length(imp); if (il > 22.0f) imp = imp * (22.0f / il);
                p.rag.shirt = p.shirt; p.rag.pants = p.pants; p.rag.skin = p.skin; p.rag.hair = p.hair;
                p.rag.start(p.pos, p.yaw, imp * 0.95f);
                hud.msgs.push_back({"Fu\xC3\x9Fg\xC3\xA4nger angefahren!", 4.0f});
                script.event("pedestrian");
                traffic::offence(2);
                car.vel = car.vel * 0.85f;
            }
        }
        i++;
    }
}

// Fussgaenger, die gerade die Fahrbahn queren (damit KI-Autos anhalten)
void crossing(std::vector<std::pair<Vec3, Vec3>>& out) {
    for (const Ped& p : peds)
        if (p.crossing && !p.down) out.push_back({p.pos, normalize({p.cTo.x - p.cFrom.x, 0, p.cTo.z - p.cFrom.z})});
}

int count() { return (int)peds.size(); }

// Selbsttest: eine stehende Figur 6 m vor dem Auto
void debugSpawnAhead() {
    Ped p;
    Vec3 f = car.forward();
    p.pos = car.pos + f * 6.0f + Vec3{0, -0.5f, 0};
    p.yaw = std::atan2(f.x, f.z) + 0.6f;
    p.shirt = Color{196, 70, 62, 255}; p.pants = Color{40, 46, 70, 255}; p.skin = Color{236, 190, 160, 255}; p.hair = Color{60, 40, 28, 255};
    p.frozen = true;
    p.phase = 0.8f;
    peds.push_back(p);
}

// Figur (Spieler zu Fuss) in einen Batch zeichnen: detailliertes Modell mit Geh-Animation, sonst einfache Koerper
void appendFigure(Batch& out, Vec3 pos, float yaw, float phase, float swingAmount, Color shirt, Color pants, Color skin, Color hair) {
    loadPedModel();
    Mat4 base = fromQuatPos(qaxis({0, 1, 0}, yaw), pos);
    float sw = std::sin(phase) * 0.55f * swingAmount;
    if (!pedTris.empty()) {
        float bob = std::fabs(std::sin(phase)) * 0.025f * swingAmount;
        Mat4 mb = mul(base, translate({0, bob, 0}));
        Mat4 mA = mul(mb, mul(translate({0, 0.92f, 0}), mul(rotX(sw), translate({0, -0.92f, 0}))));
        Mat4 mB = mul(mb, mul(translate({0, 0.92f, 0}), mul(rotX(-sw), translate({0, -0.92f, 0}))));
        Mat4 armA = mul(mb, mul(translate({0, 1.45f, 0}), mul(rotX(-sw * 0.85f), translate({0, -1.45f, 0}))));
        Mat4 armB = mul(mb, mul(translate({0, 1.45f, 0}), mul(rotX(sw * 0.85f), translate({0, -1.45f, 0}))));
        for (const PedTri& t : pedTris) {
            Color c = t.region == 0 ? skin : t.region == 1 ? shirt : t.region == 2 ? pants : t.region == 3 ? pedShoe : t.region == 4 ? pedSole : hair;
            const Mat4& m = t.part == 1 ? mA : t.part == 2 ? mB : t.part == 3 ? armA : t.part == 4 ? armB : mb;
            for (int q = 0; q < 3; q++) out.vert(xformPoint(m, t.p[q]), normalize(xformDir(m, t.n[q])), 0, 0, c);
        }
        return;
    }
    out.box(base, {0, 1.2f, 0}, {0.19f, 0.31f, 0.11f}, shirt);
    out.box(base, {-0.09f, 0.45f, 0}, {0.065f, 0.44f, 0.075f}, pants);
    out.box(base, {0.09f, 0.45f, 0}, {0.065f, 0.44f, 0.075f}, pants);
    out.box(base, {0, 1.64f, 0}, {0.095f, 0.11f, 0.095f}, skin);
}

void draw(const Mat4& vp, Vec3 cam, float maxDist) {
    (void)vp;
    pb.clear();
    const Mat4 I = identity();
    (void)I;
    for (const Ped& p : peds) {
        float d = std::hypot(p.pos.x - cam.x, p.pos.z - cam.z);
        if (d > maxDist) continue;
        if (p.down) { p.rag.draw(pb); continue; } // Ragdoll nach dem Aufprall
        Mat4 base = fromQuatPos(qaxis({0, 1, 0}, p.yaw), p.pos);
        if (p.down) { // liegt am Boden
            base = mul(base, mul(translate({0, 0.16f, 0}), rotX(-PI / 2)));
        }
        float sw = p.down ? 0.0f : std::sin(p.phase) * 0.55f;
        loadPedModel();
        if (!pedTris.empty() && d < 90.0f) { // detailliertes Modell
            float sc = 0.94f + 0.12f * ((p.shirt.r + p.pants.g) % 7) / 6.0f;
            float bob = p.down ? 0.0f : std::fabs(std::sin(p.phase)) * 0.025f;
            Mat4 mb = mul(base, mul(translate({0, bob, 0}), scaleM(sc, sc, sc)));
            Mat4 swingA = mul(translate({0, 0.92f, 0}), mul(rotX(sw), translate({0, -0.92f, 0})));
            Mat4 swingB = mul(translate({0, 0.92f, 0}), mul(rotX(-sw), translate({0, -0.92f, 0})));
            Mat4 mA = mul(mb, swingA), mB = mul(mb, swingB);
            Mat4 armA = mul(mb, mul(translate({0, 1.45f, 0}), mul(rotX(-sw * 0.85f), translate({0, -1.45f, 0}))));
            Mat4 armB = mul(mb, mul(translate({0, 1.45f, 0}), mul(rotX(sw * 0.85f), translate({0, -1.45f, 0}))));
            for (const PedTri& t : pedTris) {
                Color c = t.region == 0 ? p.skin : t.region == 1 ? p.shirt : t.region == 2 ? p.pants : t.region == 3 ? pedShoe : t.region == 4 ? pedSole : p.hair;
                const Mat4& m = t.part == 1 ? mA : t.part == 2 ? mB : t.part == 3 ? armA : t.part == 4 ? armB : mb;
                for (int q = 0; q < 3; q++) {
                    Vec3 pp = xformPoint(m, t.p[q]), nn = normalize(xformDir(m, t.n[q]));
                    pb.vert(pp, nn, 0, 0, c);
                }
            }
            continue;
        }
        auto limb = [&](Vec3 hip, float ang, Vec3 half, Color c) {
            Mat4 m = mul(base, mul(translate(hip), rotX(ang)));
            pb.box(m, {0, -half.y, 0}, half, c);
        };
        limb({-0.09f, 0.88f, 0}, sw, {0.065f, 0.44f, 0.075f}, p.pants);
        limb({0.09f, 0.88f, 0}, -sw, {0.065f, 0.44f, 0.075f}, p.pants);
        pb.box(base, {0, 1.2f, 0}, {0.19f, 0.31f, 0.11f}, p.shirt);
        limb({-0.26f, 1.46f, 0}, -sw * 0.8f, {0.05f, 0.30f, 0.055f}, p.shirt);
        limb({0.26f, 1.46f, 0}, sw * 0.8f, {0.05f, 0.30f, 0.055f}, p.shirt);
        pb.box(base, {0, 1.64f, 0}, {0.095f, 0.11f, 0.095f}, p.skin);
        pb.box(base, {0, 1.72f, -0.01f}, {0.10f, 0.05f, 0.10f}, p.hair);
    }
    foot::appendToBatch(pb);
    if (pb.v.empty()) return;
    gfxModel(identity());
    gfxBias(0);
    gfxMode(0, FLAG_LIT | FLAG_FOG);
    pb.draw();
}

} // namespace ped
