#pragma once

// Internal to the scripting module: converting values and entities between
// C++ and the Luau stack.

#include "aether/script/luau_host.h"

struct lua_State;

namespace aether::script {

void PushScriptValue(lua_State* L, const ScriptValue& value);
ScriptValue ReadScriptValue(lua_State* L, int index);

// world_bindings.cpp: entity userdata (nil if no world is bound).
void PushEntityValue(lua_State* L, Entity entity);
bool ReadEntityValue(lua_State* L, int index, Entity& out);

} // namespace aether::script
