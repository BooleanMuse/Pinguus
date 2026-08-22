#pragma once
// ScriptEngine.h
//
// MODS EN LUA (Fase 1: reactivos / generativos).
//
// Corre ENTERAMENTE en el hilo principal, nunca en el de audio: los noteys se
// mueven en el callback de audio para timing perfecto, y ahí no es seguro
// correr Lua (su GC/allocaciones romperían el tiempo real). Por eso los mods
// solo REACCIONAN y MANEJAN el mundo desde el hilo principal (spawnear noteys,
// pintar/editar celdas, disparar notas, tocar el patrón linear, reaccionar al
// playhead), empujando los MISMOS comandos que usa la UI. Hay ~1 frame de
// latencia, imperceptible salvo en noteys muy rápidos.
//
// Puente Lua <-> app: las funciones C de la API no pueden capturar los lambdas
// de main(), así que llaman a un ScriptContext global (un struct de
// std::function que main() rellena capturando sus lambdas/estado). Cada mod se
// carga en el mismo lua_State y registra callbacks con pinguus.on_update(fn);
// update(dt) los llama a todos con pcall (un error deshabilita ese callback y
// se reporta, no tumba la app).

#include <string>
#include <vector>
#include <functional>
#include <cstdio>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

// Fase 2 (celdas nativas precisas): un mod REGISTRA un tipo de celda que mapea
// a una primitiva del motor (girar/hold/mute/teleport) con sus parámetros. La
// celda se coloca como una herramienta y corre por-paso con timing perfecto.
struct ModCellAction {
    int behavior = 0;     // 0=turn 1=hold 2=mute 3=teleport 4=fx 5=note
    float seconds = 1.0f; // hold
    int dir = 0;          // turn (0=R 1=D 2=L 3=U)
    int tdx = 0, tdy = 0; // teleport: desplazamiento relativo
    int fx = 0;           // fx: 1=reverb 2=echo 3=reverse 4=chorus
    int semitone = 0;     // note: tono relativo
    int slot = -1;        // note: slot (-1 = el slot activo)
};
struct ModCellDef {
    std::string name;     // id único (sin espacios)
    std::string glyph;    // etiqueta corta dibujada en la celda
    unsigned char r = 180, g = 120, b = 255;
    std::vector<ModCellAction> actions; // Fase 3: una o varias primitivas en orden
};

// Todo lo que un mod puede leer/hacer del mundo. main() rellena estos callbacks
// capturando sus lambdas; las funciones C de Lua los invocan por el puntero
// global g_scriptCtx.
struct ScriptContext {
    std::function<void(const ModCellDef&)> registerCell;
    // Acciones
    std::function<void(int, int, int, int, float)> spawn; // x,y,dir(0..3),slot,speed
    std::function<void()> clearNoteys;
    std::function<void(int, int, int, int)> paint;             // x,y,semitone,slot
    std::function<void(int, int)> erase;                       // x,y
    std::function<void(int, int)> playLive;                    // slot,semitone
    std::function<void(int, int, int, int)> linearSet;         // col,row,slot,semitone
    std::function<void()> linearClear;
    std::function<void(float)> setBpm;
    std::function<void(const char*)> status;
    // Lecturas
    std::function<int()> gridW;
    std::function<int()> gridH;
    std::function<float()> bpm;
    std::function<bool()> playing;
    std::function<int()> playheadLinear;
    std::function<int()> playheadTracker;
    std::function<int()> noteyCount;
    // notey(i) -> devuelve por punteros; true si i es válido
    std::function<bool(int, int&, int&, int&, int&, int&, bool&)> notey; // i -> x,y,dx,dy,slot,playing
};

inline ScriptContext* g_scriptCtx = nullptr;

// ---- Funciones C expuestas a Lua (tabla global `pinguus`) ----
namespace scriptapi {

inline int l_spawn(lua_State* L) {
    if (g_scriptCtx && g_scriptCtx->spawn) {
        int x = (int)luaL_checkinteger(L, 1);
        int y = (int)luaL_checkinteger(L, 2);
        int dir = (int)luaL_optinteger(L, 3, 0);
        int slot = (int)luaL_optinteger(L, 4, 0);
        float speed = (float)luaL_optnumber(L, 5, 1.0);
        g_scriptCtx->spawn(x, y, dir, slot, speed);
    }
    return 0;
}
inline int l_clear_noteys(lua_State*) { if (g_scriptCtx && g_scriptCtx->clearNoteys) g_scriptCtx->clearNoteys(); return 0; }
inline int l_paint(lua_State* L) {
    if (g_scriptCtx && g_scriptCtx->paint)
        g_scriptCtx->paint((int)luaL_checkinteger(L, 1), (int)luaL_checkinteger(L, 2),
                           (int)luaL_optinteger(L, 3, 0), (int)luaL_optinteger(L, 4, 0));
    return 0;
}
inline int l_erase(lua_State* L) {
    if (g_scriptCtx && g_scriptCtx->erase) g_scriptCtx->erase((int)luaL_checkinteger(L, 1), (int)luaL_checkinteger(L, 2));
    return 0;
}
inline int l_play(lua_State* L) {
    if (g_scriptCtx && g_scriptCtx->playLive) g_scriptCtx->playLive((int)luaL_checkinteger(L, 1), (int)luaL_optinteger(L, 2, 0));
    return 0;
}
inline int l_linear_set(lua_State* L) {
    if (g_scriptCtx && g_scriptCtx->linearSet)
        g_scriptCtx->linearSet((int)luaL_checkinteger(L, 1), (int)luaL_checkinteger(L, 2),
                               (int)luaL_optinteger(L, 3, 0), (int)luaL_optinteger(L, 4, 0));
    return 0;
}
inline int l_linear_clear(lua_State*) { if (g_scriptCtx && g_scriptCtx->linearClear) g_scriptCtx->linearClear(); return 0; }
inline int l_set_bpm(lua_State* L) { if (g_scriptCtx && g_scriptCtx->setBpm) g_scriptCtx->setBpm((float)luaL_checknumber(L, 1)); return 0; }
inline int l_status(lua_State* L) { if (g_scriptCtx && g_scriptCtx->status) g_scriptCtx->status(luaL_checkstring(L, 1)); return 0; }

inline int l_grid_w(lua_State* L) { lua_pushinteger(L, g_scriptCtx && g_scriptCtx->gridW ? g_scriptCtx->gridW() : 0); return 1; }
inline int l_grid_h(lua_State* L) { lua_pushinteger(L, g_scriptCtx && g_scriptCtx->gridH ? g_scriptCtx->gridH() : 0); return 1; }
inline int l_bpm(lua_State* L) { lua_pushnumber(L, g_scriptCtx && g_scriptCtx->bpm ? g_scriptCtx->bpm() : 120.0); return 1; }
inline int l_playing(lua_State* L) { lua_pushboolean(L, g_scriptCtx && g_scriptCtx->playing ? g_scriptCtx->playing() : false); return 1; }
inline int l_playhead_linear(lua_State* L) { lua_pushinteger(L, g_scriptCtx && g_scriptCtx->playheadLinear ? g_scriptCtx->playheadLinear() : -1); return 1; }
inline int l_playhead_tracker(lua_State* L) { lua_pushinteger(L, g_scriptCtx && g_scriptCtx->playheadTracker ? g_scriptCtx->playheadTracker() : -1); return 1; }
inline int l_notey_count(lua_State* L) { lua_pushinteger(L, g_scriptCtx && g_scriptCtx->noteyCount ? g_scriptCtx->noteyCount() : 0); return 1; }
inline int l_notey(lua_State* L) {
    int i = (int)luaL_checkinteger(L, 1);
    int x, y, dx, dy, slot; bool playing;
    if (g_scriptCtx && g_scriptCtx->notey && g_scriptCtx->notey(i, x, y, dx, dy, slot, playing)) {
        lua_newtable(L);
        lua_pushinteger(L, x); lua_setfield(L, -2, "x");
        lua_pushinteger(L, y); lua_setfield(L, -2, "y");
        lua_pushinteger(L, dx); lua_setfield(L, -2, "dx");
        lua_pushinteger(L, dy); lua_setfield(L, -2, "dy");
        lua_pushinteger(L, slot); lua_setfield(L, -2, "slot");
        lua_pushboolean(L, playing); lua_setfield(L, -2, "playing");
        return 1;
    }
    lua_pushnil(L);
    return 1;
}
// Lee una acción (behavior/seconds/dir/dx/dy/effect/note/slot) de la tabla en
// el índice ABSOLUTO `idx`.
inline ModCellAction readAction(lua_State* L, int idx) {
    ModCellAction a;
    lua_getfield(L, idx, "behavior");
    if (lua_isstring(L, -1)) {
        std::string b = lua_tostring(L, -1);
        a.behavior = (b == "hold") ? 1 : (b == "mute") ? 2 : (b == "teleport") ? 3 : (b == "fx") ? 4 : (b == "note") ? 5 : 0;
    }
    lua_pop(L, 1);
    lua_getfield(L, idx, "seconds"); if (lua_isnumber(L, -1)) a.seconds = (float)lua_tonumber(L, -1); lua_pop(L, 1);
    lua_getfield(L, idx, "dir");     if (lua_isnumber(L, -1)) a.dir = (int)lua_tointeger(L, -1);       lua_pop(L, 1);
    lua_getfield(L, idx, "dx");      if (lua_isnumber(L, -1)) a.tdx = (int)lua_tointeger(L, -1);       lua_pop(L, 1);
    lua_getfield(L, idx, "dy");      if (lua_isnumber(L, -1)) a.tdy = (int)lua_tointeger(L, -1);       lua_pop(L, 1);
    lua_getfield(L, idx, "note");    if (lua_isnumber(L, -1)) a.semitone = (int)lua_tointeger(L, -1); lua_pop(L, 1);
    lua_getfield(L, idx, "slot");    if (lua_isnumber(L, -1)) a.slot = (int)lua_tointeger(L, -1);     lua_pop(L, 1);
    lua_getfield(L, idx, "effect");
    if (lua_isstring(L, -1)) {
        std::string e = lua_tostring(L, -1);
        a.fx = (e == "reverb") ? 1 : (e == "echo") ? 2 : (e == "reverse") ? 3 : (e == "chorus") ? 4 : 0;
    }
    lua_pop(L, 1);
    return a;
}

// pinguus.register_cell{ name=, glyph=, color={r,g,b}, behavior=..., seconds=,
//   dir=, dx=, dy=, effect=, note=, slot= }  -- una primitiva; O BIEN
// pinguus.register_cell{ name=, glyph=, color=, actions={ {behavior=...}, ... } } -- Fase 3
inline int l_register_cell(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    ModCellDef d;
    lua_getfield(L, 1, "name");  if (lua_isstring(L, -1)) d.name = lua_tostring(L, -1);  lua_pop(L, 1);
    lua_getfield(L, 1, "glyph"); if (lua_isstring(L, -1)) d.glyph = lua_tostring(L, -1); lua_pop(L, 1);
    lua_getfield(L, 1, "color");
    if (lua_istable(L, -1)) {
        lua_rawgeti(L, -1, 1); if (lua_isnumber(L, -1)) d.r = (unsigned char)lua_tointeger(L, -1); lua_pop(L, 1);
        lua_rawgeti(L, -1, 2); if (lua_isnumber(L, -1)) d.g = (unsigned char)lua_tointeger(L, -1); lua_pop(L, 1);
        lua_rawgeti(L, -1, 3); if (lua_isnumber(L, -1)) d.b = (unsigned char)lua_tointeger(L, -1); lua_pop(L, 1);
    }
    lua_pop(L, 1);
    lua_getfield(L, 1, "actions");
    if (lua_istable(L, -1)) {
        int at = lua_gettop(L);              // índice absoluto de la tabla actions
        int n = (int)lua_rawlen(L, at);
        for (int i = 1; i <= n; i++) {
            lua_rawgeti(L, at, i);
            if (lua_istable(L, -1)) d.actions.push_back(readAction(L, lua_gettop(L)));
            lua_pop(L, 1);
        }
    } else {
        d.actions.push_back(readAction(L, 1)); // una sola primitiva (Fase 2)
    }
    lua_pop(L, 1); // actions
    if (d.glyph.size() > 3) d.glyph = d.glyph.substr(0, 3);
    if (!d.name.empty() && !d.actions.empty() && g_scriptCtx && g_scriptCtx->registerCell) g_scriptCtx->registerCell(d);
    return 0;
}
// pinguus.on_update(fn): registra un callback llamado cada frame con dt.
inline int l_on_update(lua_State* L) {
    luaL_checktype(L, 1, LUA_TFUNCTION);
    lua_getfield(L, LUA_REGISTRYINDEX, "pinguus_updates"); // tabla de callbacks
    int n = (int)lua_rawlen(L, -1);
    lua_pushvalue(L, 1);           // el fn
    lua_rawseti(L, -2, n + 1);     // updates[n+1] = fn
    lua_pop(L, 1);
    return 0;
}

} // namespace scriptapi

class ScriptEngine {
public:
    struct Mod { std::string name; std::string path; };

    bool init() {
        if (L) return true;
        L = luaL_newstate();
        if (!L) return false;
        luaL_openlibs(L);
        registerApi();
        return true;
    }

    void shutdown() {
        if (L) { lua_close(L); L = nullptr; }
        mods.clear();
    }

    // Carga y ejecuta un .lua (registra el mod). Devuelve false y setea err si falla.
    bool loadFile(const char* path, std::string& err) {
        if (!L && !init()) { err = "no lua state"; return false; }
        if (luaL_dofile(L, path) != LUA_OK) {
            err = lua_tostring(L, -1) ? lua_tostring(L, -1) : "unknown error";
            lua_pop(L, 1);
            return false;
        }
        // Nombre = basename.
        std::string p = path, name = p;
        size_t s = p.find_last_of("/\\");
        if (s != std::string::npos) name = p.substr(s + 1);
        mods.push_back({name, p});
        return true;
    }

    // Recarga TODOS los mods desde cero (para editar en caliente).
    bool reloadAll(std::string& err) {
        std::vector<Mod> keep = mods;
        shutdown();
        if (!init()) { err = "reinit failed"; return false; }
        for (const Mod& m : keep) {
            std::string e;
            if (!loadFile(m.path.c_str(), e)) { err = m.name + ": " + e; return false; }
        }
        return true;
    }

    // Llama a cada callback on_update(dt). Un error deshabilita ese callback.
    void update(float dt) {
        if (!L || mods.empty()) return;
        lua_getfield(L, LUA_REGISTRYINDEX, "pinguus_updates");
        int n = (int)lua_rawlen(L, -1);
        for (int i = 1; i <= n; i++) {
            lua_rawgeti(L, -1, i);           // callback i
            if (lua_isfunction(L, -1)) {
                lua_pushnumber(L, dt);
                if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
                    if (g_scriptCtx && g_scriptCtx->status)
                        g_scriptCtx->status(lua_tostring(L, -1) ? lua_tostring(L, -1) : "mod error");
                    printf("MOD error: %s\n", lua_tostring(L, -1) ? lua_tostring(L, -1) : "?");
                    lua_pop(L, 1);           // el mensaje de error
                    // Reemplaza el callback fallido por un no-op para no repetir.
                    lua_pushnil(L);
                    lua_rawseti(L, -2, i);
                }
            } else {
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 1); // la tabla updates
    }

    const std::vector<Mod>& list() const { return mods; }
    bool ready() const { return L != nullptr; }

private:
    lua_State* L = nullptr;
    std::vector<Mod> mods;

    void registerApi() {
        // Tabla vacía para los callbacks on_update.
        lua_newtable(L);
        lua_setfield(L, LUA_REGISTRYINDEX, "pinguus_updates");

        lua_newtable(L); // la tabla `pinguus`
        auto reg = [&](const char* n, lua_CFunction f) { lua_pushcfunction(L, f); lua_setfield(L, -2, n); };
        reg("spawn", scriptapi::l_spawn);
        reg("clear_noteys", scriptapi::l_clear_noteys);
        reg("paint", scriptapi::l_paint);
        reg("erase", scriptapi::l_erase);
        reg("play", scriptapi::l_play);
        reg("linear_set", scriptapi::l_linear_set);
        reg("linear_clear", scriptapi::l_linear_clear);
        reg("set_bpm", scriptapi::l_set_bpm);
        reg("status", scriptapi::l_status);
        reg("grid_w", scriptapi::l_grid_w);
        reg("grid_h", scriptapi::l_grid_h);
        reg("bpm", scriptapi::l_bpm);
        reg("playing", scriptapi::l_playing);
        reg("playhead_linear", scriptapi::l_playhead_linear);
        reg("playhead_tracker", scriptapi::l_playhead_tracker);
        reg("notey_count", scriptapi::l_notey_count);
        reg("notey", scriptapi::l_notey);
        reg("on_update", scriptapi::l_on_update);
        reg("register_cell", scriptapi::l_register_cell);
        lua_setglobal(L, "pinguus");
    }
};
