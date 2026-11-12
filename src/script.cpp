#include "script.h"
#include <lua.hpp>

static Script* self(lua_State* L) { return (Script*)lua_touserdata(L, lua_upvalueindex(1)); }

static int l_instruction(lua_State* L) { self(L)->hud->instruction = luaL_optstring(L, 1, ""); return 0; }
static int l_title(lua_State* L) { self(L)->hud->title = luaL_optstring(L, 1, ""); return 0; }
static int l_message(lua_State* L) {
    HudState* h = self(L)->hud;
    h->msgs.push_back({luaL_checkstring(L, 1), (float)luaL_optnumber(L, 2, 4.0)});
    if (h->msgs.size() > 4) h->msgs.erase(h->msgs.begin());
    return 0;
}
static int l_fault(lua_State* L) {
    HudState* h = self(L)->hud;
    int pts = (int)luaL_optinteger(L, 2, 1);
    h->points += pts;
    h->faults.push_back(std::string(luaL_checkstring(L, 1)) + "  (+" + std::to_string(pts) + ")");
    if (h->faults.size() > 6) h->faults.erase(h->faults.begin());
    return 0;
}
static int l_points(lua_State* L) { lua_pushinteger(L, self(L)->hud->points); return 1; }
static int l_clearFaults(lua_State* L) { self(L)->hud->faults.clear(); self(L)->hud->points = 0; return 0; }
static int l_resetCar(lua_State* L) { self(L)->wantReset = true; return 0; }
static int l_time(lua_State* L) { lua_pushnumber(L, self(L)->time); return 1; }
static int l_log(lua_State* L) { logMsg("[lua] %s", luaL_checkstring(L, 1)); return 0; }

bool Script::init(HudState* h) {
    hud = h;
    L = luaL_newstate();
    luaL_openlibs(L);
    static const luaL_Reg fns[] = {{"instruction", l_instruction}, {"title", l_title}, {"message", l_message}, {"fault", l_fault},
                                   {"points", l_points}, {"clear_faults", l_clearFaults}, {"reset_car", l_resetCar}, {"time", l_time},
                                   {"log", l_log}, {nullptr, nullptr}};
    lua_newtable(L);
    lua_pushlightuserdata(L, this);
    luaL_setfuncs(L, fns, 1);
    lua_setglobal(L, "sim");
    lua_newtable(L);
    lua_setglobal(L, "car");
    if (luaL_dofile(L, "scripts/main.lua")) {
        logMsg("Lua-Fehler: %s", lua_tostring(L, -1));
        hud->instruction = std::string("Lua-Fehler: ") + lua_tostring(L, -1);
        lua_pop(L, 1);
        return false;
    }
    return true;
}

void Script::carBegin() { lua_getglobal(L, "car"); }
void Script::carNum(const char* k, double v) { lua_pushnumber(L, v); lua_setfield(L, -2, k); }
void Script::carBool(const char* k, bool v) { lua_pushboolean(L, v); lua_setfield(L, -2, k); }
void Script::carStr(const char* k, const std::string& v) { lua_pushstring(L, v.c_str()); lua_setfield(L, -2, k); }
void Script::carEnd() { lua_pop(L, 1); }

static void call(lua_State* L, HudState* hud, int nargs) {
    if (lua_pcall(L, nargs, 0, 0)) {
        logMsg("Lua-Fehler: %s", lua_tostring(L, -1));
        hud->msgs.push_back({std::string("Lua-Fehler: ") + lua_tostring(L, -1), 8.0f});
        lua_pop(L, 1);
    }
}

void Script::update(float dt) {
    time += dt;
    lua_getglobal(L, "on_update");
    if (!lua_isfunction(L, -1)) { lua_pop(L, 1); return; }
    lua_pushnumber(L, dt);
    call(L, hud, 1);
}

void Script::event(const char* name, const char* arg) {
    lua_getglobal(L, "on_event");
    if (!lua_isfunction(L, -1)) { lua_pop(L, 1); return; }
    lua_pushstring(L, name);
    if (arg) lua_pushstring(L, arg); else lua_pushnil(L);
    call(L, hud, 2);
}
