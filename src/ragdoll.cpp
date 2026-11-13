#include "ragdoll.h"
#include "game.h"
#include <random>

namespace {
// Ruhehaltung (x rechts, y oben, z vorwaerts = -z), Meter
const Vec3 REST[Ragdoll::N] = {
    {0, 1.66f, 0}, {0, 1.50f, 0}, {0, 1.28f, 0}, {0, 0.95f, 0},          // Kopf, Hals, Brust, Becken
    {-0.20f, 1.44f, 0}, {0.20f, 1.44f, 0}, {-0.26f, 1.16f, 0}, {0.26f, 1.16f, 0}, {-0.27f, 0.90f, 0}, {0.27f, 0.90f, 0}, // Schultern, Ellbogen, Haende
    {-0.10f, 0.50f, 0}, {0.10f, 0.50f, 0}, {-0.10f, 0.05f, 0}, {0.10f, 0.05f, 0}};                                        // Knie, Fuesse
const int LINKS[][2] = {{0, 1}, {1, 2}, {2, 3}, {1, 4}, {1, 5}, {4, 6}, {5, 7}, {6, 8}, {7, 9}, {3, 10}, {3, 11}, {10, 12}, {11, 13},
                        {4, 5}, {4, 2}, {5, 2}, {4, 3}, {5, 3}, {0, 2}, {10, 11}, {2, 10}, {2, 11}};
const int NL = sizeof LINKS / sizeof LINKS[0];
std::mt19937 rng{99};

float groundAt(const Vec3& v) {
    float h = v.y - 1.0f;
    if (!world.surface(v.x, v.z, v.y + 0.8f, h)) return v.y - 1.0f;
    return h;
}
} // namespace

void Ragdoll::start(Vec3 feet, float yaw, Vec3 impact) {
    Quat qy = qaxis({0, 1, 0}, yaw);
    float spd = length(Vec3{impact.x, 0, impact.z});
    Vec3 dir = spd > 0.01f ? Vec3{impact.x / spd, 0, impact.z / spd} : Vec3{0, 0, -1};
    Vec3 com{};
    for (int i = 0; i < N; i++) { p[i] = feet + qrot(qy, REST[i]); com += p[i] * (1.0f / N); }
    // Wurfgeschwindigkeit: Oberkoerper schneller als die Fuesse, dazu Hochwurf und Drehung (Ueberschlag)
    float up = 1.8f + 0.16f * spd;
    Vec3 omega = cross(Vec3{0, 1, 0}, dir) * (spd * 0.35f + 1.0f);
    const float dt0 = 1.0f / 60.0f;
    for (int i = 0; i < N; i++) {
        float hf = clampv(REST[i].y / 1.7f, 0.0f, 1.0f);
        Vec3 v = impact * (0.6f + 0.4f * hf) + Vec3{0, up * (0.7f + 0.3f * hf), 0} + cross(omega, p[i] - com);
        v += Vec3{(float)((int)(rng() % 100) - 50) * 0.01f, 0, (float)((int)(rng() % 100) - 50) * 0.01f};
        q[i] = p[i] - v * dt0;
    }
    active = true;
    age = 0;
}

void Ragdoll::step(float dtIn) {
    if (!active) return;
    age += dtIn;
    float rest[NL];
    for (int i = 0; i < NL; i++) rest[i] = length(REST[LINKS[i][0]] - REST[LINKS[i][1]]);
    int sub = std::max(1, (int)std::ceil(dtIn / (1.0f / 90.0f)));
    float dt = dtIn / sub;
    for (int s = 0; s < sub; s++) {
        for (int i = 0; i < N; i++) {
            Vec3 v = (p[i] - q[i]) * 0.9985f;
            q[i] = p[i];
            p[i] += v + Vec3{0, -9.81f * dt * dt, 0};
        }
        for (int it = 0; it < 6; it++) {
            for (int k = 0; k < NL; k++) {
                int a = LINKS[k][0], b = LINKS[k][1];
                Vec3 d = p[b] - p[a];
                float l = length(d);
                if (l < 1e-5f) continue;
                Vec3 corr = d * ((l - rest[k]) / l * 0.5f);
                p[a] += corr; p[b] = p[b] - corr;
            }
            for (int i = 0; i < N; i++) { // Boden
                float g = groundAt(p[i]) + 0.06f;
                if (p[i].y < g) {
                    p[i].y = g;
                    q[i].x += (p[i].x - q[i].x) * 0.18f; // Reibung: Rutschen und Rollen bremsen ab
                    q[i].z += (p[i].z - q[i].z) * 0.18f;
                    q[i].y = p[i].y + (q[i].y - p[i].y) * 0.4f; // wenig Abprall
                }
            }
        }
    }
}

Vec3 Ragdoll::center() const {
    Vec3 c{};
    for (int i = 0; i < N; i++) c += p[i] * (1.0f / N);
    return c;
}

float Ragdoll::speed() const {
    float s = 0;
    for (int i = 0; i < N; i++) s += length(p[i] - q[i]);
    return s / N * 60.0f;
}

void Ragdoll::draw(Batch& b) const {
    const Mat4 I = identity();
    Vec3 head = p[0], neck = p[1], chest = p[2], pelvis = p[3];
    b.beam(I, pelvis, chest, 0.11f, shirt);
    b.beam(I, chest, neck, 0.06f, shirt);
    b.beam(I, p[4], p[5], 0.045f, shirt);
    Vec3 hu = normalize(head - neck);
    b.box(I, head + hu * 0.04f, {0.10f, 0.11f, 0.10f}, skin);
    b.box(I, head + hu * 0.12f, {0.105f, 0.045f, 0.105f}, hair);
    for (int s = 0; s < 2; s++) {
        int sh = 4 + s, el = 6 + s, ha = 8 + s, kn = 10 + s, ft = 12 + s;
        b.beam(I, p[sh], p[el], 0.045f, shirt);
        b.beam(I, p[el], p[ha], 0.038f, skin);
        b.beam(I, pelvis, p[kn], 0.065f, pants);
        b.beam(I, p[kn], p[ft], 0.055f, pants);
        b.box(I, p[ft] + Vec3{0, 0.02f, 0}, {0.05f, 0.035f, 0.10f}, Color{34, 32, 30, 255});
    }
}
