// The Luau `Draw` table (Phase 23 step 3): debug lines, shapes and text.
//
//   Draw.line(from, to [, color [, seconds]])     Draw.arrow(from, to [, color [, seconds]])
//   Draw.box(center, half_extents [, color [, seconds]])
//   Draw.sphere(center, radius [, color [, seconds]])
//   Draw.point(position [, size [, color [, seconds]]])
//   Draw.text(position, text [, color [, seconds]])   Draw.screen(text [, color [, seconds]])
//
// Positions and colors are vectors (colors 0..1 RGB, white by default);
// seconds default to 0, one frame.

#include "aether/debug/debug_draw.h"

#include <lua.h>
#include <lualib.h>

namespace aether::script {

namespace {

Vec3 CheckVec(lua_State* L, int i) {
    const float* v = luaL_checkvector(L, i);
    return Vec3(v[0], v[1], v[2]);
}

u32 OptColor(lua_State* L, int i) {
    if (lua_isnoneornil(L, i)) return 0xFFFFFFFFu;
    return DebugColor(CheckVec(L, i));
}

f32 OptSeconds(lua_State* L, int i) { return static_cast<f32>(luaL_optnumber(L, i, 0.0)); }

int Line(lua_State* L) {
    DebugDrawList::Get().Line(CheckVec(L, 1), CheckVec(L, 2), OptColor(L, 3), OptSeconds(L, 4));
    return 0;
}
int Arrow(lua_State* L) {
    DebugDrawList::Get().Arrow(CheckVec(L, 1), CheckVec(L, 2), OptColor(L, 3), 0.25f, OptSeconds(L, 4));
    return 0;
}
int Box(lua_State* L) {
    DebugDrawList::Get().Box(CheckVec(L, 1), CheckVec(L, 2), OptColor(L, 3), OptSeconds(L, 4));
    return 0;
}
int Sphere(lua_State* L) {
    DebugDrawList::Get().Sphere(CheckVec(L, 1), static_cast<f32>(luaL_checknumber(L, 2)), OptColor(L, 3), OptSeconds(L, 4));
    return 0;
}
int Point(lua_State* L) {
    DebugDrawList::Get().Point(CheckVec(L, 1), static_cast<f32>(luaL_optnumber(L, 2, 0.2)), OptColor(L, 3), OptSeconds(L, 4));
    return 0;
}
int Text(lua_State* L) {
    DebugDrawList::Get().Text(CheckVec(L, 1), luaL_checkstring(L, 2), OptColor(L, 3), OptSeconds(L, 4));
    return 0;
}
int Screen(lua_State* L) {
    DebugDrawList::Get().ScreenText(luaL_checkstring(L, 1), OptColor(L, 2), OptSeconds(L, 3));
    return 0;
}

} // namespace

void InstallDebugDrawBindings(lua_State* L) {
    lua_newtable(L);
    const std::pair<const char*, lua_CFunction> fns[] = {{"line", Line},   {"arrow", Arrow}, {"box", Box},      {"sphere", Sphere},
                                                         {"point", Point}, {"text", Text},   {"screen", Screen}};
    for (const auto& [name, fn] : fns) {
        lua_pushcfunction(L, fn, name);
        lua_setfield(L, -2, name);
    }
    lua_setreadonly(L, -1, true);
    lua_setglobal(L, "Draw");
}

} // namespace aether::script
