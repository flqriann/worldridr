#pragma once
#include "common.h"

class World;

struct Wheel {
    Vec3 mount;
    float k = 0, c = 0;
    bool driven = false, front = false;
    float omega = 0, comp = 0, steer = 0, rot = 0;
    bool hasGround = false, contact = false;
    Vec3 gp, gn{0, 1, 0};
    float mu = 1, crr = 0.012f, slip = 0;
};

// Kompaktwagen mit Frontantrieb, 6-Gang-Schaltgetriebe (oder Automatik), Starrkoerper mit 4 Radaufhaengungen
struct Vehicle {
    Vec3 pos, vel, angVel;
    Quat q;
    Wheel w[4]; // VL, VR, HL, HR
    float engW = 0;
    bool engineOn = false;
    int gear = 0; // -1 R, 0 N, 1..6

    // Eingaben
    float throttle = 0, brake = 0, clutch = 0, steer = 0;
    bool handbrake = true, starter = false;
    bool automatic = false, autoClutch = true;
    int selector = 0; // Automatik: 0 P, 1 R, 2 N, 3 D

    // Ausgaben
    float ffb = 0;        // Lenkmoment (Nm am Lenkrad, positiv = nach rechts)
    bool evStall = false, evGrind = false;
    float odo = 0;
    bool onRoad = true;

    Vehicle();
    int kind = 0;               // 0 Auto, 1 Moped, 2 Motorrad
    void setKind(int k);
    int engineType = 0;         // 0 Benzin, 1 Diesel (nur Auto)
    void setEngine(int e);
    float rpm() const { return engW * 9.5493f; }
    float speed() const; // m/s vorwaerts (negativ = rueckwaerts)
    Vec3 forward() const { return qrot(q, {0, 0, -1}); }
    void place(Vec3 p, float yaw);
    void shift(int dir);
    void requestGear(int g);
    void update(float dt, World& world);
    Mat4 transform() const { return fromQuatPos(q, pos); }
    Vec3 hubLocal(int i) const;
    static float wheelRadius();

private:
    void substep(float dt);
    void logic(float dt);
    float shiftTimer = 0, autoEngage = 0, autoShiftCd = 0;
    float manifold = 0, boost = 0; // Saugrohrfuellung und Ladedruck (0..1)
    int autoGear = 1;
};
