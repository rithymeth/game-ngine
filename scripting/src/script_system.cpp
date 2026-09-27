#include "aether/script/script_system.h"

#include "aether/core/log.h"

#include "script_values.h"

#include <lua.h>
#include <lualib.h>

#include <algorithm>
#include <regex>
#include <sstream>

namespace aether::script {

namespace {

constexpr const char* kCallbacks[] = {"OnCreate", "OnEnable", "OnStart", "OnUpdate", "OnFixedUpdate",
                                      "OnLateUpdate", "OnDisable", "OnDestroy", "OnReload"};

void PushJsonValue(lua_State* L, const nlohmann::json& value) {
    if (value.is_boolean()) {
        lua_pushboolean(L, value.get<bool>() ? 1 : 0);
    } else if (value.is_number()) {
        lua_pushnumber(L, value.get<f64>());
    } else if (value.is_string()) {
        lua_pushstring(L, value.get_ref<const std::string&>().c_str());
    } else if (value.is_array() && value.size() == 3) {
        lua_pushvector(L, value[0].get<f32>(), value[1].get<f32>(), value[2].get<f32>());
    } else {
        lua_pushnil(L);
    }
}

// Metadata comments above field assignments: --@range a b, --@tooltip text.
struct Metadata {
    bool has_range = false;
    f64 min = 0.0;
    f64 max = 0.0;
    std::string tooltip;
};

std::unordered_map<std::string, Metadata> ParseMetadata(const std::string& source) {
    static const std::regex kRange(R"(^\s*--@range\s+(-?[0-9.]+)\s+(-?[0-9.]+)\s*$)");
    static const std::regex kTooltip(R"(^\s*--@tooltip\s+(.*?)\s*$)");
    static const std::regex kField(R"(^\s*(?:[A-Za-z_]\w*\.)?([A-Za-z_]\w*)\s*=)");
    std::unordered_map<std::string, Metadata> result;
    Metadata pending;
    bool has_pending = false;
    std::istringstream lines(source);
    std::string line;
    std::smatch match;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (std::regex_match(line, match, kRange)) {
            pending.has_range = true;
            pending.min = std::stod(match[1]);
            pending.max = std::stod(match[2]);
            has_pending = true;
        } else if (std::regex_match(line, match, kTooltip)) {
            pending.tooltip = match[1];
            has_pending = true;
        } else if (line.find_first_not_of(" \t") == std::string::npos) {
            continue; // blank lines don't break the association
        } else {
            if (has_pending && std::regex_search(line, match, kField)) {
                result[match[1]] = pending;
            }
            pending = Metadata{};
            has_pending = false;
        }
    }
    return result;
}

} // namespace

ScriptClassInfo ScriptSystem::DescribeSource(LuauHost& host, const std::string& source, const std::string& name,
                                             int* ref) {
    ScriptClassInfo info;
    if (ref != nullptr) {
        *ref = LUA_NOREF;
    }
    lua_State* L = host.State();
    const int top = lua_gettop(L);
    if (!host.LoadChunk(source, name, &info.error) || !host.ProtectedCall(0, 1, &info.error)) {
        lua_settop(L, top);
        return info;
    }
    if (lua_type(L, -1) != LUA_TTABLE) {
        info.error = name + ": a script must return a table (its class), not " + luaL_typename(L, -1);
        lua_settop(L, top);
        return info;
    }
    const auto metadata = ParseMetadata(source);
    lua_pushnil(L);
    while (lua_next(L, -2) != 0) {
        if (lua_type(L, -2) == LUA_TSTRING) {
            const std::string key = lua_tostring(L, -2);
            const int kind = lua_type(L, -1);
            if (kind == LUA_TFUNCTION) {
                if (std::find(std::begin(kCallbacks), std::end(kCallbacks), key) != std::end(kCallbacks)) {
                    info.callbacks.push_back(key);
                }
            } else if (!key.empty() && key.front() != '_') { // _private fields aren't exposed
                ExposedVariable variable;
                variable.name = key;
                bool supported = true;
                if (kind == LUA_TNUMBER) {
                    variable.kind = ExposedVariable::Kind::Number;
                    variable.default_value = lua_tonumber(L, -1);
                } else if (kind == LUA_TBOOLEAN) {
                    variable.kind = ExposedVariable::Kind::Bool;
                    variable.default_value = lua_toboolean(L, -1) != 0;
                } else if (kind == LUA_TSTRING) {
                    variable.kind = ExposedVariable::Kind::String;
                    variable.default_value = lua_tostring(L, -1);
                } else if (kind == LUA_TVECTOR) {
                    const float* v = lua_tovector(L, -1);
                    variable.kind = ExposedVariable::Kind::Vector;
                    variable.default_value = nlohmann::json::array({v[0], v[1], v[2]});
                } else {
                    supported = false; // tables, userdata: not Inspector material
                }
                if (supported) {
                    if (auto it = metadata.find(key); it != metadata.end()) {
                        variable.has_range = it->second.has_range;
                        variable.range_min = it->second.min;
                        variable.range_max = it->second.max;
                        variable.tooltip = it->second.tooltip;
                    }
                    info.variables.push_back(std::move(variable));
                }
            }
        }
        lua_pop(L, 1);
    }
    std::sort(info.variables.begin(), info.variables.end(),
              [](const ExposedVariable& a, const ExposedVariable& b) { return a.name < b.name; });
    std::sort(info.callbacks.begin(), info.callbacks.end());
    info.ok = true;
    if (ref != nullptr) {
        *ref = lua_ref(L, -1);
    }
    lua_settop(L, top);
    return info;
}

ScriptSystem::ScriptSystem(LuauHost& host, World& world, GuidIndex& guids, SourceLoader loader)
    : host_(host), world_(world), guids_(guids), loader_(std::move(loader)) {
    host_.BindWorld(&world_, &guids_);
    InstallApi();
}

ScriptSystem::~ScriptSystem() {
    BindInput(nullptr);
    lua_State* L = host_.State();
    // Scripts may still hold events or timers: they now report that no
    // script system is running instead of reaching this one.
    lua_pushnil(L);
    lua_setfield(L, LUA_REGISTRYINDEX, "Aether.ScriptSystem");
    for (auto& [id, event] : events_) {
        for (const Connection& c : event.connections) {
            lua_unref(L, c.function_ref);
        }
    }
    for (const Timer& timer : timers_) {
        lua_unref(L, timer.function_ref);
    }
    for (auto& [guid, instance] : instances_) {
        lua_unref(L, instance.ref);
    }
    for (auto& [guid, cls] : classes_) {
        if (cls.ref != LUA_NOREF) {
            lua_unref(L, cls.ref);
        }
        if (cls.meta_ref != LUA_NOREF) {
            lua_unref(L, cls.meta_ref);
        }
    }
}

ScriptSystem::Class* ScriptSystem::LoadClass(const assets::AssetGuid& script) {
    if (auto it = classes_.find(script); it != classes_.end()) {
        return it->second.info.ok ? &it->second : nullptr;
    }
    Class& cls = classes_[script];
    std::string source;
    if (!loader_ || !loader_(script, source, cls.name)) {
        cls.info.error = "Script " + assets::ToString(script) + " isn't available";
        errors_.push_back(cls.info.error);
        return nullptr;
    }
    cls.info = DescribeSource(host_, source, cls.name, &cls.ref);
    if (!cls.info.ok) {
        errors_.push_back(cls.info.error);
        return nullptr;
    }
    lua_State* L = host_.State();
    lua_createtable(L, 0, 1); // instances' metatable: {__index = class}
    lua_getref(L, cls.ref);
    lua_setfield(L, -2, "__index");
    cls.meta_ref = lua_ref(L, -1);
    lua_pop(L, 1);
    return &cls;
}

ScriptClassInfo ScriptSystem::ClassInfo(const assets::AssetGuid& script) {
    LoadClass(script);
    return classes_[script].info;
}

ScriptSystem::Instance* ScriptSystem::Find(Entity entity) {
    const IdComponent* id = world_.IsAlive(entity) ? world_.GetComponent<IdComponent>(entity) : nullptr;
    if (id == nullptr) {
        return nullptr;
    }
    auto it = instances_.find(id->guid);
    return it != instances_.end() ? &it->second : nullptr;
}

void ScriptSystem::Create(Entity entity) {
    const ScriptComponent* component = world_.GetComponent<ScriptComponent>(entity);
    if (component == nullptr || !component->script.IsSet()) {
        return;
    }
    Class* cls = LoadClass(component->script.guid);
    if (cls == nullptr) {
        if (const IdComponent* id = world_.GetComponent<IdComponent>(entity)) {
            waiting_[id->guid] = component->script.guid; // starts if the script is fixed
        }
        return;
    }
    lua_State* L = host_.State();
    lua_createtable(L, 0, 4);
    lua_getref(L, cls->meta_ref);
    lua_setmetatable(L, -2);
    PushEntityValue(L, entity);
    lua_setfield(L, -2, "entity");
    // This entity's overrides of the exposed variables.
    for (const ScriptProperty& property : component->properties) {
        const ExposedVariable* variable = cls->info.Find(property.name);
        const nlohmann::json value = nlohmann::json::parse(property.value, nullptr, /*allow_exceptions=*/false);
        if (variable == nullptr || value.is_discarded() || !MatchesKind(variable->kind, value)) {
            errors_.push_back(cls->name + ": ignoring the value of '" + property.name +
                              "' (the script has no such variable, or it's a different type)");
            continue;
        }
        PushJsonValue(L, value);
        lua_setfield(L, -2, property.name.c_str());
    }
    const EntityGuid guid = world_.GetComponent<IdComponent>(entity)->guid;
    instances_[guid] = Instance{lua_ref(L, -1), component->script.guid, cls->name, next_owner_++};
    lua_pop(L, 1);
}

void ScriptSystem::Invoke(Entity entity, const char* method, const f32* dt) {
    Instance* instance = Find(entity);
    if (instance == nullptr) {
        return;
    }
    lua_State* L = host_.State();
    const int top = lua_gettop(L);
    lua_getref(L, instance->ref);
    lua_getfield(L, -1, method); // from the class, through the metatable
    if (lua_type(L, -1) != LUA_TFUNCTION) {
        lua_settop(L, top);
        return;
    }
    lua_pushvalue(L, -2); // self
    int args = 1;
    if (dt != nullptr) {
        lua_pushnumber(L, *dt);
        ++args;
    }
    std::string error;
    const u64 previous_owner = current_owner_;
    current_owner_ = instance->owner; // what it connects is its own
    if (!host_.ProtectedCall(args, 0, &error)) {
        RecordError(error + " (in " + method + ")");
    }
    current_owner_ = previous_owner;
    lua_settop(L, top);
}

void ScriptSystem::Destroy(Entity entity) {
    Invoke(entity, "OnDestroy");
    const IdComponent* id = world_.IsAlive(entity) ? world_.GetComponent<IdComponent>(entity) : nullptr;
    if (id == nullptr) {
        return;
    }
    waiting_.erase(id->guid);
    if (auto it = instances_.find(id->guid); it != instances_.end()) {
        ReleaseOwner(it->second.owner); // its connections and timers end with it
        lua_unref(host_.State(), it->second.ref);
        instances_.erase(it);
    }
}

ScriptResult ScriptSystem::CallMethod(Entity entity, const std::string& method, const std::vector<ScriptValue>& args) {
    ScriptResult result;
    Instance* instance = Find(entity);
    if (instance == nullptr) {
        result.error = "The entity has no running script";
        return result;
    }
    lua_State* L = host_.State();
    const int top = lua_gettop(L);
    lua_getref(L, instance->ref);
    lua_getfield(L, -1, method.c_str());
    if (lua_type(L, -1) != LUA_TFUNCTION) {
        lua_settop(L, top);
        result.error = instance->name + " has no method '" + method + "'";
        return result;
    }
    lua_insert(L, -2); // function, self
    for (const ScriptValue& arg : args) {
        PushScriptValue(L, arg);
    }
    const u64 previous_owner = current_owner_;
    current_owner_ = instance->owner;
    const bool ok = host_.ProtectedCall(static_cast<int>(args.size()) + 1, LUA_MULTRET, &result.error);
    current_owner_ = previous_owner;
    if (!ok) {
        lua_settop(L, top);
        return result;
    }
    for (int i = top + 1; i <= lua_gettop(L); ++i) {
        result.values.push_back(ReadScriptValue(L, i));
    }
    lua_settop(L, top);
    result.ok = true;
    return result;
}

ScriptValue ScriptSystem::GetField(Entity entity, const std::string& field) {
    Instance* instance = Find(entity);
    if (instance == nullptr) {
        return {};
    }
    lua_State* L = host_.State();
    lua_getref(L, instance->ref);
    lua_getfield(L, -1, field.c_str());
    ScriptValue value = ReadScriptValue(L, -1);
    lua_pop(L, 2);
    return value;
}

void ScriptSystem::Register(Lifecycle& lifecycle) {
    LifecycleCallbacks callbacks;
    callbacks.on_create = [this](Entity e) {
        Create(e);
        Invoke(e, "OnCreate");
    };
    callbacks.on_enable = [this](Entity e) { Invoke(e, "OnEnable"); };
    callbacks.on_start = [this](Entity e) { Invoke(e, "OnStart"); };
    callbacks.on_update = [this](Entity e, f32 dt) { Invoke(e, "OnUpdate", &dt); };
    callbacks.on_fixed_update = [this](Entity e, f32 dt) { Invoke(e, "OnFixedUpdate", &dt); };
    callbacks.on_late_update = [this](Entity e, f32 dt) { Invoke(e, "OnLateUpdate", &dt); };
    callbacks.on_disable = [this](Entity e) { Invoke(e, "OnDisable"); };
    callbacks.on_destroy = [this](Entity e) { Destroy(e); };
    lifecycle.Register<ScriptComponent>(std::move(callbacks));
}

} // namespace aether::script
