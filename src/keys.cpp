#include "keys.h"

namespace {
struct Def { const char* id; const char* label; SDL_Keycode def, alt; };
const Def DEFS[A_COUNT] = {
    {"throttle", "Gas / vorw\xC3\xA4rts", SDLK_W, SDLK_UP},
    {"brake", "Bremse / r\xC3\xBC" "ckw\xC3\xA4rts", SDLK_S, SDLK_DOWN},
    {"left", "Links lenken / gehen", SDLK_A, SDLK_LEFT},
    {"right", "Rechts lenken / gehen", SDLK_D, SDLK_RIGHT},
    {"clutch", "Kupplung", SDLK_X, SDLK_UNKNOWN},
    {"shiftup", "Hochschalten", SDLK_LSHIFT, SDLK_PAGEUP},
    {"shiftdown", "Runterschalten", SDLK_LCTRL, SDLK_PAGEDOWN},
    {"handbrake", "Handbremse", SDLK_SPACE, SDLK_UNKNOWN},
    {"ignition", "Motor starten / abstellen", SDLK_I, SDLK_UNKNOWN},
    {"blinkl", "Blinker links", SDLK_Q, SDLK_UNKNOWN},
    {"blinkr", "Blinker rechts", SDLK_E, SDLK_UNKNOWN},
    {"hazard", "Warnblinker", SDLK_F, SDLK_UNKNOWN},
    {"lights", "Licht", SDLK_L, SDLK_UNKNOWN},
    {"belt", "Gurt", SDLK_G, SDLK_UNKNOWN},
    {"horn", "Hupe", SDLK_H, SDLK_UNKNOWN},
    {"camera", "Kamera wechseln", SDLK_V, SDLK_UNKNOWN},
    {"reset", "Zur\xC3\xBC" "ck auf die Stra\xC3\x9F" "e", SDLK_R, SDLK_UNKNOWN},
    {"map", "Weltkarte", SDLK_M, SDLK_UNKNOWN},
    {"minimap", "Minimap-Zoom", SDLK_N, SDLK_UNKNOWN},
    {"place", "Ort eingeben", SDLK_T, SDLK_UNKNOWN},
    {"action", "Tanken / Lieferauftrag", SDLK_J, SDLK_UNKNOWN},
    {"fuelnav", "Navigation zur Tankstelle", SDLK_K, SDLK_UNKNOWN},
    {"exit", "Aussteigen / Einsteigen", SDLK_O, SDLK_UNKNOWN},
    {"lock", "Auto zu- / aufsperren", SDLK_Y, SDLK_UNKNOWN},
    {"chat", "Chat", SDLK_RETURN, SDLK_KP_ENTER},
    {"sprint", "Rennen (zu Fu\xC3\x9F)", SDLK_LSHIFT, SDLK_RSHIFT},
};
SDL_Keycode cur[A_COUNT];
bool inited = false;
void init() {
    if (inited) return;
    inited = true;
    for (int i = 0; i < A_COUNT; i++) cur[i] = DEFS[i].def;
}
} // namespace

namespace keys {
SDL_Keycode get(Act a) { init(); return cur[a]; }
bool is(Act a, SDL_Keycode k) { init(); return k != SDLK_UNKNOWN && (k == cur[a] || k == DEFS[a].alt); }
bool held(Act a) {
    init();
    const bool* st = SDL_GetKeyboardState(nullptr);
    SDL_Scancode s = SDL_GetScancodeFromKey(cur[a], nullptr);
    if (s != SDL_SCANCODE_UNKNOWN && st[s]) return true;
    if (DEFS[a].alt != SDLK_UNKNOWN) {
        s = SDL_GetScancodeFromKey(DEFS[a].alt, nullptr);
        if (s != SDL_SCANCODE_UNKNOWN && st[s]) return true;
    }
    return false;
}
void set(Act a, SDL_Keycode k) { init(); cur[a] = k; }
void resetAll() { init(); for (int i = 0; i < A_COUNT; i++) cur[i] = DEFS[i].def; }
const char* label(Act a) { return DEFS[a].label; }
std::string name(Act a) {
    init();
    const char* n = SDL_GetKeyName(cur[a]);
    return n && *n ? n : "?";
}
bool load(const std::string& key, const std::string& value) {
    init();
    if (key.rfind("key_", 0) != 0) return false;
    std::string id = key.substr(4);
    for (int i = 0; i < A_COUNT; i++)
        if (id == DEFS[i].id) {
            SDL_Keycode k = SDL_GetKeyFromName(value.c_str());
            if (k != SDLK_UNKNOWN) cur[i] = k;
            return true;
        }
    return false;
}
void save(FILE* f) {
    init();
    for (int i = 0; i < A_COUNT; i++)
        if (cur[i] != DEFS[i].def) fprintf(f, "key_%s=%s\n", DEFS[i].id, SDL_GetKeyName(cur[i]));
}
} // namespace keys
