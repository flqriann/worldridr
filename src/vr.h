#pragma once
#include "common.h"

// VR ueber OpenXR (Meta Horizon Link / Air Link, SteamVR, ...). OpenGL-Anbindung unter Windows.
struct VREye {
    Mat4 pose;  // Auge im Sitz-Koordinatensystem (nach Zentrierung)
    Mat4 proj;
    int w = 0, h = 0;
    unsigned fbo = 0;
};

struct VR {
    bool available = false; // Laufzeit + Headset gefunden
    bool running = false;   // Sitzung laeuft (Headset aufgesetzt)
    VREye eye[2];

    bool init();
    bool beginFrame();        // true => Frame begonnen (endFrame muss folgen)
    bool shouldRender() const;
    void bindEye(int i);      // Framebuffer + Viewport setzen
    void releaseEye(int i);
    void endFrame();
    void recenter() { wantRecenter = true; }

private:
    struct Impl;
    Impl* d = nullptr;
    bool wantRecenter = true;
    void poll();
};
