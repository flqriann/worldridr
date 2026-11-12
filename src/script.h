#pragma once
#include "common.h"

struct lua_State;

struct HudState {
    std::string title, instruction;
    struct Msg { std::string text; float ttl; };
    std::vector<Msg> msgs;
    std::vector<std::string> faults;
    int points = 0;
};

// Lua-Anbindung: Fahrstunden und Regelpruefung liegen in scripts/*.lua
struct Script {
    lua_State* L = nullptr;
    HudState* hud = nullptr;
    bool wantReset = false;
    double time = 0;

    bool init(HudState* h);
    void carBegin();
    void carNum(const char* k, double v);
    void carBool(const char* k, bool v);
    void carStr(const char* k, const std::string& v);
    void carEnd();
    void update(float dt);
    void event(const char* name, const char* arg = nullptr);
};
