#pragma once
// Aenderbare Tastenbelegung. Jede Aktion hat eine Haupttaste (im Menue aenderbar) und optional eine feste Zweittaste (z. B. Pfeiltasten).
#include <SDL3/SDL.h>
#include <cstdio>
#include <string>

enum Act {
    A_THROTTLE, A_BRAKE, A_LEFT, A_RIGHT, A_CLUTCH, A_SHIFTUP, A_SHIFTDOWN, A_HANDBRAKE, A_IGNITION, A_BLINK_L, A_BLINK_R, A_HAZARD, A_LIGHTS,
    A_BELT, A_HORN, A_CAMERA, A_RESET, A_MAP, A_MINIMAP, A_PLACE, A_ACTION, A_FUELNAV, A_EXIT, A_LOCK, A_CHAT, A_SPRINT, A_COUNT
};

namespace keys {
SDL_Keycode get(Act a);
bool is(Act a, SDL_Keycode k);       // Taste gehoert zur Aktion (Haupt- oder Zweittaste)
bool held(Act a);                    // Taste gerade gedrueckt (Zustand)
void set(Act a, SDL_Keycode k);
void resetAll();
const char* label(Act a);            // Beschriftung im Menue
std::string name(Act a);             // Name der Haupttaste
bool load(const std::string& key, const std::string& value);   // "key_<id>" aus settings.cfg
void save(FILE* f);
}
