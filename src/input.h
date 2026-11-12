#pragma once
#include "common.h"
#include <SDL3/SDL.h>

// Lenkrad (Logitech G920 u. a.) mit Force Feedback, plus Tastatur als Rueckfall
struct Input {
    // Analogwerte
    float steer = 0, throttle = 0, brake = 0, clutch = 0;
    bool horn = false, starter = false;
    int hGear = -99; // H-Schaltung: -99 = nicht benutzt
    float lookYaw = 0;
    // Einmal-Ereignisse (werden pro Frame zurueckgesetzt)
    bool shiftUp = false, shiftDown = false, handbrake = false, blinkL = false, blinkR = false, hazard = false;
    bool lights = false, belt = false, reset = false, camera = false;

    bool wheelPresent = false;
    std::string wheelName;
    float axes[8] = {};
    int naxes = 0;
    uint32_t buttons = 0;
    bool ffbOk = false;

    void init(const WheelCfg& cfg);
    void beginFrame();
    void handleEvent(const SDL_Event& e, bool typing);
    void update(float dt, float speedMs, bool typing);
    void setFFB(float torqueNm, float dt);
    // H-Schaltung: Tastennummern fuer Gang 1..6 und Rueckwaertsgang festlegen (aus der Einrichtung im Menue)
    void setShifter(const int gears[6], int reverse) {
        for (int i = 0; i < 6; i++) cfg.gearButtons[i] = gears[i];
        cfg.reverseButton = reverse;
        cfg.hShifter = true;
        hUsed = false;
    }
    bool shifterReady() const { return cfg.hShifter; }

private:
    WheelCfg cfg;
    SDL_Joystick* joy = nullptr;
    SDL_Haptic* haptic = nullptr;
    int effect = -1;
    SDL_JoystickID joyId = 0;
    bool axisTouched[8] = {};
    Sint16 axisInit[8] = {};
    bool hUsed = false;
    float kbSteer = 0, kbThrottle = 0, kbBrake = 0, kbClutch = 0, lastSteer = 0;
    void openWheel(SDL_JoystickID id);
    float pedal(int axis);
};
