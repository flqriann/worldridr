#pragma once
#include "gfx.h"

// Einfache Ragdoll-Physik fuer Menschen: 14 Gelenkpunkte (Verlet), Abstandsbedingungen, Bodenkontakt mit Reibung.
// Wird fuer angefahrene Passanten und fuer den Spieler selbst verwendet.
struct Ragdoll {
    static const int N = 14;
    Vec3 p[N], q[N];
    bool active = false;
    float age = 0;
    Color shirt{70, 100, 170, 255}, pants{40, 46, 70, 255}, skin{236, 190, 160, 255}, hair{60, 40, 28, 255};

    // Figur steht bei 'feet' mit Blickrichtung yaw und wird mit impact (m/s) getroffen
    void start(Vec3 feet, float yaw, Vec3 impact);
    void step(float dt);
    Vec3 center() const;
    float speed() const;          // mittlere Geschwindigkeit (m/s)
    void draw(Batch& b) const;
};
