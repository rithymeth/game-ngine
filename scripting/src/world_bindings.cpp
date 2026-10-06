// Reflection-driven Luau bindings for the ECS (Phase 11 step 2,
// docs/design/PHASE_SPECS.md §11.1).

#include "aether/script/luau_host.h"

#include "script_values.h"

#include "aether/ecs/world.h"
#include "aether/reflection/serialize.h"
#include "aether/scene/entity_guid.h"

#include <lua.h>
#include <lualib.h>

#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>

namespace aether::script {

using reflect::FieldInfo;
using reflect::FunctionInfo;
using reflect::Json;
using reflect::TypeInfo;
using reflect::TypeKind;

// Private access for the free functions below.
struct BindingAccess {
    static World* WorldOf(LuauHost& h) { return h.world_; }
    static GuidIndex* GuidsOf(LuauHost& h) { return h.guids_; }
    static u64 Generation(LuauHost& h) { return h.binding_generation_; }
    static std::function<void(const LuauHost::DebugDrawCall&)>& DrawSink(LuauHost& h) { return h.debug_draw_; }
};

namespace {

constexpr const char* kEntityMeta = "Aether.Entity";
constexpr const char* kComponentMeta = "Aether.Component";

struct EntityHandle {
    u64 binding;
    Entity entity;
};

struct ComponentHandle {
    u64 binding;
    Entity entity;
    ComponentId component;
};

LuauHost& Host(lua_State* L) { return *static_cast<LuauHost*>(lua_callbacks(L)->userdata); }

const char* ComponentName(ComponentId id) {
    const ComponentInfo& info = GetComponentInfo(id);
    return info.reflected != nullptr ? info.reflected->name : info.name;
}

World& RequireWorld(lua_State* L) {
    World* world = BindingAccess::WorldOf(Host(L));
    if (world == nullptr) {
        luaL_error(L, "no world is bound to this script host");
    }
    return *world;
}

// The entity behind a handle, or a script error saying why it's gone.
Entity ResolveEntity(lua_State* L, u64 binding, Entity entity) {
    World& world = RequireWorld(L);
    if (binding != BindingAccess::Generation(Host(L))) {
        luaL_error(L, "this entity belongs to a world that is no longer bound");
    }
    if (!world.IsAlive(entity)) {
        luaL_error(L, "attempt to use an entity that was destroyed");
    }
    return entity;
}

EntityHandle* CheckEntity(lua_State* L, int index) {
    return static_cast<EntityHandle*>(luaL_checkudata(L, index, kEntityMeta));
}

// The component's memory, re-validated on every access.
void* ResolveComponent(lua_State* L, const ComponentHandle& handle) {
    World& world = RequireWorld(L);
    if (handle.binding != BindingAccess::Generation(Host(L))) {
        luaL_error(L, "this %s belongs to a world that is no longer bound", ComponentName(handle.component));
    }
    if (!world.IsAlive(handle.entity)) {
        luaL_error(L, "attempt to use %s of an entity that was destroyed", ComponentName(handle.component));
    }
    if (!world.HasComponentRaw(handle.entity, handle.component)) {
        luaL_error(L, "the entity no longer has a %s", ComponentName(handle.component));
    }
    return world.GetComponentRaw(handle.entity, handle.component);
}

// Field lookup by name, precomputed per type (§11.1).
const FieldInfo* FindField(const TypeInfo& type, const std::string& name) {
    static std::unordered_map<const TypeInfo*, std::unordered_map<std::string, const FieldInfo*>> cache;
    auto [it, inserted] = cache.try_emplace(&type);
    if (inserted) {
        for (const FieldInfo& field : type.fields) {
            it->second.emplace(field.name, &field);
        }
    }
    auto found = it->second.find(name);
    return found != it->second.end() ? found->second : nullptr;
}

// ---------------------------------------------------------------------------
// Values: typed fast paths, JSON (tables) for everything else
// ---------------------------------------------------------------------------

void PushJson(lua_State* L, const Json& json) {
    switch (json.type()) {
    case Json::value_t::boolean: lua_pushboolean(L, json.get<bool>() ? 1 : 0); break;
    case Json::value_t::number_integer:
    case Json::value_t::number_unsigned:
    case Json::value_t::number_float: lua_pushnumber(L, json.get<f64>()); break;
    case Json::value_t::string: {
        const std::string& text = json.get_ref<const std::string&>();
        lua_pushlstring(L, text.data(), text.size());
        break;
    }
    case Json::value_t::array: {
        lua_createtable(L, static_cast<int>(json.size()), 0);
        for (usize i = 0; i < json.size(); ++i) {
            PushJson(L, json[i]);
            lua_rawseti(L, -2, static_cast<int>(i + 1));
        }
        break;
    }
    case Json::value_t::object: {
        lua_createtable(L, 0, static_cast<int>(json.size()));
        for (auto it = json.begin(); it != json.end(); ++it) {
            PushJson(L, it.value());
            lua_setfield(L, -2, it.key().c_str());
        }
        break;
    }
    default: lua_pushnil(L); break;
    }
}

Json NumberJson(f64 n) {
    if (std::floor(n) == n && std::fabs(n) < 9.0e15) {
        return static_cast<i64>(n);
    }
    return n;
}

Json ToJson(lua_State* L, int index, int depth = 0) {
    index = lua_absindex(L, index);
    switch (lua_type(L, index)) {
    case LUA_TBOOLEAN: return lua_toboolean(L, index) != 0;
    case LUA_TNUMBER: return NumberJson(lua_tonumber(L, index));
    case LUA_TSTRING: return std::string(lua_tostring(L, index));
    case LUA_TVECTOR: {
        const float* v = lua_tovector(L, index);
        return Json::array({v[0], v[1], v[2]});
    }
    case LUA_TTABLE: {
        if (depth > 32) {
            return nullptr;
        }
        const int length = lua_objlen(L, index);
        if (length > 0) {
            Json array = Json::array();
            for (int i = 1; i <= length; ++i) {
                lua_rawgeti(L, index, i);
                array.push_back(ToJson(L, -1, depth + 1));
                lua_pop(L, 1);
            }
            return array;
        }
        Json object = Json::object();
        lua_pushnil(L);
        while (lua_next(L, index) != 0) {
            if (lua_type(L, -2) == LUA_TSTRING) {
                object[lua_tostring(L, -2)] = ToJson(L, -1, depth + 1);
            }
            lua_pop(L, 1);
        }
        return object;
    }
    default: return nullptr;
    }
}

bool IsVec3(const TypeInfo& type) { return &type == &reflect::Reflect<Vec3>(); }

void PushValue(lua_State* L, const TypeInfo& type, const void* ptr) {
    switch (type.kind) {
    case TypeKind::Bool: lua_pushboolean(L, *static_cast<const bool*>(ptr) ? 1 : 0); return;
    case TypeKind::Int:
        switch (type.size) {
        case 1: lua_pushnumber(L, *static_cast<const i8*>(ptr)); return;
        case 2: lua_pushnumber(L, *static_cast<const i16*>(ptr)); return;
        case 4: lua_pushnumber(L, *static_cast<const i32*>(ptr)); return;
        default: lua_pushnumber(L, static_cast<f64>(*static_cast<const i64*>(ptr))); return;
        }
    case TypeKind::UInt:
        switch (type.size) {
        case 1: lua_pushnumber(L, *static_cast<const u8*>(ptr)); return;
        case 2: lua_pushnumber(L, *static_cast<const u16*>(ptr)); return;
        case 4: lua_pushnumber(L, *static_cast<const u32*>(ptr)); return;
        default: lua_pushnumber(L, static_cast<f64>(*static_cast<const u64*>(ptr))); return;
        }
    case TypeKind::Float:
        lua_pushnumber(L, type.size == 4 ? *static_cast<const f32*>(ptr) : *static_cast<const f64*>(ptr));
        return;
    case TypeKind::String: {
        const std::string& text = *static_cast<const std::string*>(ptr);
        lua_pushlstring(L, text.data(), text.size());
        return;
    }
    case TypeKind::FixedString: {
        const char* text = static_cast<const char*>(ptr);
        lua_pushlstring(L, text, strnlen(text, type.size));
        return;
    }
    case TypeKind::Enum: {
        i64 value = 0;
        std::memcpy(&value, ptr, std::min<usize>(type.size, sizeof(value))); // little-endian
        if (const char* name = type.EnumName(value)) {
            lua_pushstring(L, name);
        } else {
            lua_pushnumber(L, static_cast<f64>(value));
        }
        return;
    }
    default: break;
    }
    if (IsVec3(type)) {
        const Vec3& v = *static_cast<const Vec3*>(ptr);
        lua_pushvector(L, v.x, v.y, v.z);
        return;
    }
    PushJson(L, reflect::ToJson(type, ptr));
}

template <typename T>
void StoreInt(void* ptr, f64 n) {
    *static_cast<T*>(ptr) = static_cast<T>(n);
}

// Writes the Luau value at `index` into `ptr` (of `type`). On a type
// mismatch, raises a script error naming `what`.
void ReadValue(lua_State* L, int index, const TypeInfo& type, void* ptr, const char* what) {
    const int lua_kind = lua_type(L, index);
    auto mismatch = [&](const char* expected) {
        luaL_error(L, "%s expects %s, got %s", what, expected, lua_typename(L, lua_kind));
    };
    switch (type.kind) {
    case TypeKind::Bool:
        if (lua_kind != LUA_TBOOLEAN) {
            mismatch("a boolean");
        }
        *static_cast<bool*>(ptr) = lua_toboolean(L, index) != 0;
        return;
    case TypeKind::Int:
    case TypeKind::UInt: {
        if (lua_kind != LUA_TNUMBER) {
            mismatch("a number");
        }
        const f64 n = lua_tonumber(L, index);
        if (std::floor(n) != n) {
            luaL_error(L, "%s expects a whole number, got %g", what, n);
        }
        if (type.kind == TypeKind::UInt && n < 0) {
            luaL_error(L, "%s can't be negative", what);
        }
        if (type.kind == TypeKind::Int) {
            switch (type.size) {
            case 1: StoreInt<i8>(ptr, n); return;
            case 2: StoreInt<i16>(ptr, n); return;
            case 4: StoreInt<i32>(ptr, n); return;
            default: StoreInt<i64>(ptr, n); return;
            }
        }
        switch (type.size) {
        case 1: StoreInt<u8>(ptr, n); return;
        case 2: StoreInt<u16>(ptr, n); return;
        case 4: StoreInt<u32>(ptr, n); return;
        default: StoreInt<u64>(ptr, n); return;
        }
    }
    case TypeKind::Float:
        if (lua_kind != LUA_TNUMBER) {
            mismatch("a number");
        }
        if (type.size == 4) {
            *static_cast<f32*>(ptr) = static_cast<f32>(lua_tonumber(L, index));
        } else {
            *static_cast<f64*>(ptr) = lua_tonumber(L, index);
        }
        return;
    case TypeKind::String:
        if (lua_kind != LUA_TSTRING) {
            mismatch("a string");
        }
        *static_cast<std::string*>(ptr) = lua_tostring(L, index);
        return;
    case TypeKind::FixedString: {
        if (lua_kind != LUA_TSTRING) {
            mismatch("a string");
        }
        size_t length = 0;
        const char* text = lua_tolstring(L, index, &length);
        char* out = static_cast<char*>(ptr);
        const usize n = std::min<usize>(length, type.size - 1);
        std::memcpy(out, text, n);
        std::memset(out + n, 0, type.size - n);
        return;
    }
    case TypeKind::Enum: {
        i64 value = 0;
        if (lua_kind == LUA_TSTRING) {
            if (!type.EnumValueOf(lua_tostring(L, index), value)) {
                luaL_error(L, "%s has no value named '%s'", what, lua_tostring(L, index));
            }
        } else if (lua_kind == LUA_TNUMBER) {
            value = static_cast<i64>(lua_tonumber(L, index));
        } else {
            mismatch("an enum name");
        }
        std::memcpy(ptr, &value, std::min<usize>(type.size, sizeof(value)));
        return;
    }
    default: break;
    }
    if (IsVec3(type)) {
        Vec3& v = *static_cast<Vec3*>(ptr);
        if (lua_kind == LUA_TVECTOR) {
            const float* c = lua_tovector(L, index);
            v = Vec3{c[0], c[1], c[2]};
            return;
        }
        if (lua_kind != LUA_TTABLE) {
            mismatch("a vector");
        }
    }
    if (lua_kind != LUA_TTABLE) {
        mismatch("a table");
    }
    reflect::LoadReport report;
    if (!reflect::FromJson(type, ptr, ToJson(L, index), &report)) {
        luaL_error(L, "%s: the table doesn't match %s", what, type.name);
    }
    if (!report.warnings.empty()) {
        luaL_error(L, "%s: %s", what, report.warnings.front().c_str());
    }
}

// ---------------------------------------------------------------------------
// Userdata
// ---------------------------------------------------------------------------

void PushComponent(lua_State* L, Entity entity, ComponentId id) {
    auto* handle = static_cast<ComponentHandle*>(lua_newuserdata(L, sizeof(ComponentHandle)));
    *handle = {BindingAccess::Generation(Host(L)), entity, id};
    luaL_getmetatable(L, kComponentMeta);
    lua_setmetatable(L, -2);
}

ComponentId CheckComponentName(lua_State* L, int index, bool must_be_reflected = true) {
    const char* name = luaL_checkstring(L, index);
    const ComponentId id = FindComponentIdByName(name);
    if (id == kInvalidComponentId) {
        luaL_error(L, "there's no component type named '%s'", name);
    }
    if (must_be_reflected && GetComponentInfo(id).reflected == nullptr) {
        luaL_error(L, "component '%s' isn't reflected, so scripts can't use it", name);
    }
    return id;
}

int CallMethod(lua_State* L) {
    const auto* function = static_cast<const FunctionInfo*>(lua_touserdata(L, lua_upvalueindex(1)));
    auto* handle = static_cast<ComponentHandle*>(luaL_checkudata(L, 1, kComponentMeta));
    void* self = ResolveComponent(L, *handle);
    const int given = lua_gettop(L) - 1;
    if (given != static_cast<int>(function->params.size())) {
        luaL_error(L, "%s:%s takes %d argument(s), got %d", ComponentName(handle->component), function->name,
                   static_cast<int>(function->params.size()), given);
    }
    std::vector<reflect::Any> args;
    args.reserve(function->params.size());
    for (usize i = 0; i < function->params.size(); ++i) {
        const reflect::ParamInfo& param = function->params[i];
        args.push_back(reflect::Any::DefaultOf(*param.type));
        const std::string what = std::string(function->name) + " argument '" + param.name + "'";
        ReadValue(L, static_cast<int>(i) + 2, *param.type, args.back().Data(), what.c_str());
    }
    reflect::Any ret;
    if (!function->Invoke(function->HasFlag(reflect::Fn_Static) ? nullptr : self, args, &ret)) {
        luaL_error(L, "calling %s failed", function->name);
    }
    if (function->return_type == nullptr || !ret.HasValue()) {
        return 0;
    }
    PushValue(L, *ret.Type(), ret.Data());
    return 1;
}

int ComponentIndex(lua_State* L) {
    auto* handle = static_cast<ComponentHandle*>(luaL_checkudata(L, 1, kComponentMeta));
    const char* key = luaL_checkstring(L, 2);
    void* data = ResolveComponent(L, *handle);
    const TypeInfo& type = *GetComponentInfo(handle->component).reflected;
    if (const FieldInfo* field = FindField(type, key)) {
        PushValue(L, *field->type, field->Ptr(data));
        return 1;
    }
    if (const FunctionInfo* function = type.FindFunction(key)) {
        lua_pushlightuserdata(L, const_cast<FunctionInfo*>(function));
        lua_pushcclosure(L, &CallMethod, function->name, 1);
        return 1;
    }
    luaL_error(L, "%s has no field or function named '%s'", type.name, key);
    return 0;
}

int ComponentNewIndex(lua_State* L) {
    auto* handle = static_cast<ComponentHandle*>(luaL_checkudata(L, 1, kComponentMeta));
    const char* key = luaL_checkstring(L, 2);
    void* data = ResolveComponent(L, *handle);
    const TypeInfo& type = *GetComponentInfo(handle->component).reflected;
    const FieldInfo* field = FindField(type, key);
    if (field == nullptr) {
        luaL_error(L, "%s has no field named '%s'", type.name, key);
    }
    const std::string what = std::string(type.name) + "." + key;
    ReadValue(L, 3, *field->type, field->Ptr(data), what.c_str());
    return 0;
}

int ComponentToString(lua_State* L) {
    auto* handle = static_cast<ComponentHandle*>(luaL_checkudata(L, 1, kComponentMeta));
    lua_pushfstring(L, "%s(entity %u)", ComponentName(handle->component), handle->entity.index);
    return 1;
}

int ComponentEq(lua_State* L) {
    auto* a = static_cast<ComponentHandle*>(luaL_checkudata(L, 1, kComponentMeta));
    auto* b = static_cast<ComponentHandle*>(luaL_checkudata(L, 2, kComponentMeta));
    lua_pushboolean(L, a->binding == b->binding && a->entity == b->entity && a->component == b->component);
    return 1;
}

// Entity methods.
int EntityGet(lua_State* L) {
    EntityHandle* handle = CheckEntity(L, 1);
    const ComponentId id = CheckComponentName(L, 2);
    World& world = RequireWorld(L);
    const Entity e = ResolveEntity(L, handle->binding, handle->entity);
    if (!world.HasComponentRaw(e, id)) {
        lua_pushnil(L);
        return 1;
    }
    PushComponent(L, e, id);
    return 1;
}

int EntityHas(lua_State* L) {
    EntityHandle* handle = CheckEntity(L, 1);
    const ComponentId id = CheckComponentName(L, 2, /*must_be_reflected=*/false);
    const Entity e = ResolveEntity(L, handle->binding, handle->entity);
    lua_pushboolean(L, RequireWorld(L).HasComponentRaw(e, id) ? 1 : 0);
    return 1;
}

int EntityAdd(lua_State* L) {
    EntityHandle* handle = CheckEntity(L, 1);
    const ComponentId id = CheckComponentName(L, 2);
    const Entity e = ResolveEntity(L, handle->binding, handle->entity);
    RequireWorld(L).AddComponentRaw(e, id); // a no-op if it's already there
    PushComponent(L, e, id);
    return 1;
}

int EntityRemove(lua_State* L) {
    EntityHandle* handle = CheckEntity(L, 1);
    const ComponentId id = CheckComponentName(L, 2, /*must_be_reflected=*/false);
    if (id == GetComponentId<IdComponent>()) {
        luaL_error(L, "an entity's IdComponent can't be removed");
    }
    const Entity e = ResolveEntity(L, handle->binding, handle->entity);
    const bool had = RequireWorld(L).HasComponentRaw(e, id);
    if (had) {
        RequireWorld(L).RemoveComponentRaw(e, id);
    }
    lua_pushboolean(L, had ? 1 : 0);
    return 1;
}

int EntityIsValid(lua_State* L) {
    EntityHandle* handle = CheckEntity(L, 1);
    World* world = BindingAccess::WorldOf(Host(L));
    lua_pushboolean(L, world != nullptr && handle->binding == BindingAccess::Generation(Host(L)) &&
                           world->IsAlive(handle->entity));
    return 1;
}

int EntityGuidMethod(lua_State* L) {
    EntityHandle* handle = CheckEntity(L, 1);
    const Entity e = ResolveEntity(L, handle->binding, handle->entity);
    const std::string guid = ToString(EnsureGuid(RequireWorld(L), e, BindingAccess::GuidsOf(Host(L))));
    lua_pushlstring(L, guid.data(), guid.size());
    return 1;
}

int EntityToString(lua_State* L) {
    EntityHandle* handle = CheckEntity(L, 1);
    lua_pushfstring(L, "Entity(%u)", handle->entity.index);
    return 1;
}

int EntityEq(lua_State* L) {
    EntityHandle* a = CheckEntity(L, 1);
    EntityHandle* b = CheckEntity(L, 2);
    lua_pushboolean(L, a->binding == b->binding && a->entity == b->entity);
    return 1;
}

// World functions (called as world:Spawn() etc.; arg 1 is the world table).
int WorldSpawn(lua_State* L) {
    World& world = RequireWorld(L);
    const Entity e = world.CreateEntity(IdComponent{NewEntityGuid()});
    if (GuidIndex* guids = BindingAccess::GuidsOf(Host(L))) {
        guids->Add(world.GetComponent<IdComponent>(e)->guid, e);
    }
    PushEntityValue(L, e);
    return 1;
}

int WorldDestroy(lua_State* L) {
    EntityHandle* handle = CheckEntity(L, 2);
    World& world = RequireWorld(L);
    const Entity e = ResolveEntity(L, handle->binding, handle->entity);
    if (GuidIndex* guids = BindingAccess::GuidsOf(Host(L))) {
        if (const IdComponent* id = world.GetComponent<IdComponent>(e)) {
            guids->Remove(id->guid);
        }
    }
    world.DestroyEntity(e);
    return 0;
}

int WorldFind(lua_State* L) {
    const char* text = luaL_checkstring(L, 2);
    World& world = RequireWorld(L);
    EntityGuid guid;
    GuidIndex* guids = BindingAccess::GuidsOf(Host(L));
    const Entity e = guids != nullptr && ParseEntityGuid(text, guid) ? guids->Find(world, guid) : kNullEntity;
    if (e.IsNull()) {
        lua_pushnil(L);
    } else {
        PushEntityValue(L, e);
    }
    return 1;
}

int WorldEntitiesWith(lua_State* L) {
    const ComponentId id = CheckComponentName(L, 2, /*must_be_reflected=*/false);
    World& world = RequireWorld(L);
    std::vector<Entity> found;
    world.ForEachArchetype([&](const Archetype& archetype) {
        if (!archetype.Mask().test(id)) {
            return;
        }
        for (usize c = 0; c < archetype.ChunkCount(); ++c) {
            const Entity* list = archetype.EntityArray(c);
            found.insert(found.end(), list, list + archetype.ChunkEntityCount(c));
        }
    });
    std::sort(found.begin(), found.end(), [](Entity a, Entity b) { return a.index < b.index; });
    lua_createtable(L, static_cast<int>(found.size()), 0);
    for (usize i = 0; i < found.size(); ++i) {
        PushEntityValue(L, found[i]);
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
    return 1;
}

void SetFunction(lua_State* L, const char* name, lua_CFunction fn) {
    lua_pushcfunction(L, fn, name);
    lua_setfield(L, -2, name);
}

// --- engine.debug_draw (Phase 23 step 6) --------------------------------
// Each function takes its primitive's pins, then optional `color`
// (0xRRGGBBAA, default white) and `duration` (seconds, default 0 = this
// frame). Output goes to the host's debug-draw sink; with no sink connected
// (null handler) calls are no-ops. Nothing here needs the bound world.

// False when no debug-draw sink is connected (calls become no-ops).
bool Sink(lua_State* L) { return static_cast<bool>(BindingAccess::DrawSink(Host(L))); }
Vec3 CheckVec3(lua_State* L, int index, const char* what) {
    if (lua_type(L, index) != LUA_TVECTOR) {
        luaL_error(L, "engine.debug_draw.%s expects a vector, got %s", what, lua_typename(L, lua_type(L, index)));
    }
    const float* v = lua_tovector(L, index);
    return {v[0], v[1], v[2]};
}
void ReadDrawTail(lua_State* L, LuauHost::DebugDrawCall& call) {
    // Color is 0xRRGGBBAA: read as a double so values above INT_MAX (white
    // = 0xFFFFFFFF) round-trip, then cast to the u32 the sink expects.
    if (lua_type(L, 3) != LUA_TNONE) call.color = static_cast<u32>(lua_tonumber(L, 3));
    if (lua_type(L, 4) != LUA_TNONE) call.duration = static_cast<f32>(lua_tonumber(L, 4));
}

int DrawLine(lua_State* L) { // from, to [, color [, duration]]
    LuauHost::DebugDrawCall call;
    call.kind = LuauHost::DebugDrawCall::Kind::Line;
    call.from = CheckVec3(L, 1, "Line");
    call.to = CheckVec3(L, 2, "Line");
    ReadDrawTail(L, call);
    if (Sink(L)) BindingAccess::DrawSink(Host(L))(call);
    return 0;
}
int DrawBox(lua_State* L) { // center, halfSize [, color [, duration]]
    LuauHost::DebugDrawCall call;
    call.kind = LuauHost::DebugDrawCall::Kind::Box;
    call.from = CheckVec3(L, 1, "Box");
    call.size = CheckVec3(L, 2, "Box");
    ReadDrawTail(L, call);
    if (Sink(L)) BindingAccess::DrawSink(Host(L))(call);
    return 0;
}
int DrawSphere(lua_State* L) { // center, radius [, color [, duration]]
    LuauHost::DebugDrawCall call;
    call.kind = LuauHost::DebugDrawCall::Kind::Sphere;
    call.from = CheckVec3(L, 1, "Sphere");
    if (lua_type(L, 2) != LUA_TNUMBER) {
        luaL_error(L, "engine.debug_draw.Sphere expects a number radius, got %s", lua_typename(L, lua_type(L, 2)));
    }
    call.to.x = static_cast<f32>(lua_tonumber(L, 2));
    ReadDrawTail(L, call);
    if (Sink(L)) BindingAccess::DrawSink(Host(L))(call);
    return 0;
}
int DrawPoint(lua_State* L) { // location, size [, color [, duration]]
    LuauHost::DebugDrawCall call;
    call.kind = LuauHost::DebugDrawCall::Kind::Point;
    call.from = CheckVec3(L, 1, "Point");
    if (lua_type(L, 2) != LUA_TNUMBER) {
        luaL_error(L, "engine.debug_draw.Point expects a number size, got %s", lua_typename(L, lua_type(L, 2)));
    }
    call.to.x = static_cast<f32>(lua_tonumber(L, 2));
    ReadDrawTail(L, call);
    if (Sink(L)) BindingAccess::DrawSink(Host(L))(call);
    return 0;
}
int DrawRect(lua_State* L) { // center, size [, color [, duration]]
    LuauHost::DebugDrawCall call;
    call.kind = LuauHost::DebugDrawCall::Kind::Rect;
    call.from = CheckVec3(L, 1, "Rect");
    call.size = CheckVec3(L, 2, "Rect");
    ReadDrawTail(L, call);
    if (Sink(L)) BindingAccess::DrawSink(Host(L))(call);
    return 0;
}
int DrawText(lua_State* L) { // location, text [, color [, duration]]
    LuauHost::DebugDrawCall call;
    call.kind = LuauHost::DebugDrawCall::Kind::Text;
    call.from = CheckVec3(L, 1, "Text");
    if (lua_type(L, 2) != LUA_TSTRING) {
        luaL_error(L, "engine.debug_draw.Text expects a string, got %s", lua_typename(L, lua_type(L, 2)));
    }
    call.text = lua_tostring(L, 2);
    ReadDrawTail(L, call);
    if (Sink(L)) BindingAccess::DrawSink(Host(L))(call);
    return 0;
}

} // namespace

void PushEntityValue(lua_State* L, Entity entity) {
    if (BindingAccess::WorldOf(Host(L)) == nullptr || entity.IsNull()) {
        lua_pushnil(L);
        return;
    }
    auto* handle = static_cast<EntityHandle*>(lua_newuserdata(L, sizeof(EntityHandle)));
    *handle = {BindingAccess::Generation(Host(L)), entity};
    luaL_getmetatable(L, kEntityMeta);
    lua_setmetatable(L, -2);
}

bool ReadEntityValue(lua_State* L, int index, Entity& out) {
    if (lua_type(L, index) != LUA_TUSERDATA || lua_getmetatable(L, index) == 0) {
        return false;
    }
    luaL_getmetatable(L, kEntityMeta);
    const bool is_entity = lua_rawequal(L, -1, -2) != 0;
    lua_pop(L, 2);
    if (is_entity) {
        out = static_cast<EntityHandle*>(lua_touserdata(L, index))->entity;
    }
    return is_entity;
}

void LuauHost::InstallWorldBindings() {
    lua_State* L = state_;
    luaL_newmetatable(L, kEntityMeta);
    lua_createtable(L, 0, 6);
    SetFunction(L, "Get", &EntityGet);
    SetFunction(L, "Has", &EntityHas);
    SetFunction(L, "Add", &EntityAdd);
    SetFunction(L, "Remove", &EntityRemove);
    SetFunction(L, "IsValid", &EntityIsValid);
    SetFunction(L, "Guid", &EntityGuidMethod);
    lua_setfield(L, -2, "__index");
    SetFunction(L, "__tostring", &EntityToString);
    SetFunction(L, "__eq", &EntityEq);
    lua_pop(L, 1);

    luaL_newmetatable(L, kComponentMeta);
    SetFunction(L, "__index", &ComponentIndex);
    SetFunction(L, "__newindex", &ComponentNewIndex);
    SetFunction(L, "__tostring", &ComponentToString);
    SetFunction(L, "__eq", &ComponentEq);
    lua_pop(L, 1);

    lua_createtable(L, 0, 4);
    SetFunction(L, "Spawn", &WorldSpawn);
    SetFunction(L, "Destroy", &WorldDestroy);
    SetFunction(L, "Find", &WorldFind);
    SetFunction(L, "EntitiesWith", &WorldEntitiesWith);
    lua_setglobal(L, "world");

    // engine.debug_draw: primitives for the host's debug-draw sink (Phase
    // 23 step 6). Read by the host's SetDebugDrawHandler.
    lua_createtable(L, 0, 1); // engine
    lua_createtable(L, 0, 6); // engine.debug_draw
    SetFunction(L, "Line", &DrawLine);
    SetFunction(L, "Box", &DrawBox);
    SetFunction(L, "Sphere", &DrawSphere);
    SetFunction(L, "Point", &DrawPoint);
    SetFunction(L, "Rect", &DrawRect);
    SetFunction(L, "Text", &DrawText);
    lua_setfield(L, -2, "debug_draw");
    lua_setglobal(L, "engine");
}

void LuauHost::BindWorld(World* world, GuidIndex* guids) {
    world_ = world;
    guids_ = guids;
    ++binding_generation_; // handles from before no longer resolve
}

} // namespace aether::script
