#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

constexpr float PI = 3.14159265358979f;

template <class T> inline T clampv(T v, T lo, T hi) { return v < lo ? lo : (v > hi ? hi : v); }

struct Vec3 {
    float x = 0, y = 0, z = 0;
    Vec3() = default;
    Vec3(float x, float y, float z) : x(x), y(y), z(z) {}
    Vec3 operator+(Vec3 o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3 operator-(Vec3 o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
    Vec3 operator-() const { return {-x, -y, -z}; }
    Vec3& operator+=(Vec3 o) { x += o.x; y += o.y; z += o.z; return *this; }
};
inline float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline float length(Vec3 a) { return std::sqrt(dot(a, a)); }
inline Vec3 normalize(Vec3 a) { float l = length(a); return l > 1e-9f ? a * (1.0f / l) : Vec3{0, 1, 0}; }

struct Quat { float w = 1, x = 0, y = 0, z = 0; };
inline Quat qmul(Quat a, Quat b) {
    return {a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z, a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x, a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w};
}
inline Quat qconj(Quat q) { return {q.w, -q.x, -q.y, -q.z}; }
inline Quat qnorm(Quat q) {
    float l = std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
    if (l < 1e-9f) return {};
    return {q.w / l, q.x / l, q.y / l, q.z / l};
}
inline Quat qaxis(Vec3 a, float ang) { float s = std::sin(ang * 0.5f); return {std::cos(ang * 0.5f), a.x * s, a.y * s, a.z * s}; }
inline Vec3 qrot(Quat q, Vec3 v) {
    Vec3 u{q.x, q.y, q.z};
    Vec3 t = cross(u, v) * 2.0f;
    return v + t * q.w + cross(u, t);
}

// Spaltenweise 4x4-Matrix: Element (Zeile r, Spalte c) = m[c*4+r]
struct Mat4 { float m[16]; };
inline Mat4 identity() { Mat4 r{}; r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1; return r; }
inline Mat4 mul(const Mat4& a, const Mat4& b) {
    Mat4 r;
    for (int c = 0; c < 4; c++)
        for (int k = 0; k < 4; k++)
            r.m[c * 4 + k] = a.m[k] * b.m[c * 4] + a.m[4 + k] * b.m[c * 4 + 1] + a.m[8 + k] * b.m[c * 4 + 2] + a.m[12 + k] * b.m[c * 4 + 3];
    return r;
}
inline Mat4 translate(Vec3 t) { Mat4 r = identity(); r.m[12] = t.x; r.m[13] = t.y; r.m[14] = t.z; return r; }
inline Mat4 scaleM(float x, float y, float z) { Mat4 r = identity(); r.m[0] = x; r.m[5] = y; r.m[10] = z; return r; }
inline Mat4 fromQuatPos(Quat q, Vec3 p) {
    Vec3 a = qrot(q, {1, 0, 0}), b = qrot(q, {0, 1, 0}), c = qrot(q, {0, 0, 1});
    Mat4 r = identity();
    r.m[0] = a.x; r.m[1] = a.y; r.m[2] = a.z;
    r.m[4] = b.x; r.m[5] = b.y; r.m[6] = b.z;
    r.m[8] = c.x; r.m[9] = c.y; r.m[10] = c.z;
    r.m[12] = p.x; r.m[13] = p.y; r.m[14] = p.z;
    return r;
}
inline Mat4 rigidInverse(const Mat4& a) {
    Mat4 r = identity();
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) r.m[i * 4 + j] = a.m[j * 4 + i];
    for (int i = 0; i < 3; i++) r.m[12 + i] = -(r.m[i] * a.m[12] + r.m[4 + i] * a.m[13] + r.m[8 + i] * a.m[14]);
    return r;
}
inline Vec3 xformPoint(const Mat4& a, Vec3 p) {
    return {a.m[0] * p.x + a.m[4] * p.y + a.m[8] * p.z + a.m[12], a.m[1] * p.x + a.m[5] * p.y + a.m[9] * p.z + a.m[13],
            a.m[2] * p.x + a.m[6] * p.y + a.m[10] * p.z + a.m[14]};
}
inline Vec3 xformDir(const Mat4& a, Vec3 p) {
    return {a.m[0] * p.x + a.m[4] * p.y + a.m[8] * p.z, a.m[1] * p.x + a.m[5] * p.y + a.m[9] * p.z, a.m[2] * p.x + a.m[6] * p.y + a.m[10] * p.z};
}
// Projektion aus Tangens der Halbwinkel (l,d negativ) - passt auch fuer asymmetrische VR-Frusta
inline Mat4 frustumTan(float l, float r, float d, float u, float n, float f) {
    Mat4 m{};
    m.m[0] = 2 / (r - l); m.m[5] = 2 / (u - d);
    m.m[8] = (r + l) / (r - l); m.m[9] = (u + d) / (u - d);
    m.m[10] = (f + n) / (n - f); m.m[11] = -1; m.m[14] = 2 * f * n / (n - f);
    return m;
}
inline Mat4 perspective(float fovy, float aspect, float n, float f) {
    float t = std::tan(fovy * 0.5f);
    return frustumTan(-t * aspect, t * aspect, -t, t, n, f);
}
inline Mat4 ortho(float l, float r, float b, float t) {
    Mat4 m = identity();
    m.m[0] = 2 / (r - l); m.m[5] = 2 / (t - b); m.m[10] = -1;
    m.m[12] = -(r + l) / (r - l); m.m[13] = -(t + b) / (t - b);
    return m;
}
inline Mat4 invert(const Mat4& a) {
    const float* m = a.m; float inv[16];
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    Mat4 r = identity();
    if (std::fabs(det) < 1e-20f) return r;
    det = 1.0f / det;
    for (int i = 0; i < 16; i++) r.m[i] = inv[i] * det;
    return r;
}

void logMsg(const char* fmt, ...);

struct WheelCfg {
    int steerAxis = 0, throttleAxis = 1, brakeAxis = 2, clutchAxis = 3;
    bool invertPedals = true;
    float ffbStrength = 0.8f;
    bool ffbInvert = false;
    bool hShifter = false;
    int bShiftUp = 4, bShiftDown = 5, bBlinkL = 2, bBlinkR = 1, bHandbrake = 0, bIgnition = 3;
    int bBelt = 8, bHorn = 9, bLights = 7, bCamera = 6, bReset = 10;
    int gearButtons[6] = {12, 13, 14, 15, 16, 17};
    int reverseButton = 18;
};

struct Config {
    double lat = 48.2010, lon = 16.3695;
    std::string startPlace;
    std::string googleKey;
    bool automatic = false;
    int autoClutch = -1; // -1 = automatisch (an ohne Lenkrad, aus mit Lenkrad)
    bool vr = false, fullscreen = false, mirrors = true;
    int width = 1600, height = 900;
    int nearZoom = 18;
    float fov = 62, volume = 0.6f;
    std::string playerName = "Fahrer";
    bool startAuto = true; // Startort aus der IP-Adresse (config.lua: start = "auto")
    std::string carModel = "sedan"; // Datei in assets/cars (ohne .obj), "builtin" = Eigenbau
    int port = 27015;
    int traffic = 22; // Anzahl KI-Fahrzeuge in der Umgebung (0 = aus)
    int peds = 10;    // Anzahl Fussgaenger in der Umgebung (0 = aus)
    int quality = 1;  // 0 niedrig, 1 mittel, 2 hoch
    float renderScale = 0.85f, bldDist = 800, roadDist = 1000;
    bool msaa = false, police = true, faultPoints = false, minimap = true, mapQuestions = true, diesel = false;
    int hudMode = 0;  // 0 ruhig, 1 normal
    bool server = false;
    WheelCfg wheel;
};
