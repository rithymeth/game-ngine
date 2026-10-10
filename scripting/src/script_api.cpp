// Script APIs installed by ScriptSystem (Phase 11 step 4): events with
// :Connect, timers, and input.

#include "aether/script/script_system.h"
#include "aether/script/gameplay_api.h"
#include "aether/script/interaction_api.h"
#include "aether/script/quests_api.h"
#include "aether/script/inventory_api.h"
#include "aether/script/loc_api.h"
#include "aether/script/save_api.h"
#if AETHER_SCRIPTING_PHYSICS
#include "aether/physics/physics_scene.h"
#include "aether/physics/physics_world.h"
#endif

#include "aether/core/log.h"

#include "script_values.h"

#include <lua.h>
#include <lualib.h>

#include <algorithm>
#include <cmath>

namespace aether::script {

namespace {

constexpr const char* kSystemKey = "Aether.ScriptSystem";
constexpr const char* kEventMeta = "Aether.Event";
constexpr const char* kConnectionMeta = "Aether.Connection";
constexpr const char* kTimerMeta = "Aether.Timer";

struct EventHandle {
    u32 event;
};
struct ConnectionHandle {
    u32 event;
    u32 connection;
};
struct TimerHandle {
    u32 timer;
};

ScriptSystem& System(lua_State* L) {
    lua_getfield(L, LUA_REGISTRYINDEX, kSystemKey);
    auto* system = static_cast<ScriptSystem*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    if (system == nullptr) {
        luaL_error(L, "no script system is running");
    }
    return *system;
}

template <typename T>
T* PushHandle(lua_State* L, const char* meta, T value) {
    T* handle = static_cast<T*>(lua_newuserdata(L, sizeof(T)));
    *handle = value;
    luaL_getmetatable(L, meta);
    lua_setmetatable(L, -2);
    return handle;
}

void PushEvent(lua_State* L, u32 event) { PushHandle(L, kEventMeta, EventHandle{event}); }

int EventNew(lua_State* L) {
    PushEvent(L, System(L).NewEvent());
    return 1;
}

int EventConnect(lua_State* L) {
    auto* event = static_cast<EventHandle*>(luaL_checkudata(L, 1, kEventMeta));
    luaL_checktype(L, 2, LUA_TFUNCTION);
    lua_pushvalue(L, 2);
    const int ref = lua_ref(L, -1);
    lua_pop(L, 1);
    const u32 connection = System(L).Connect(event->event, ref);
    PushHandle(L, kConnectionMeta, ConnectionHandle{event->event, connection});
    return 1;
}

int EventFire(lua_State* L) {
    auto* event = static_cast<EventHandle*>(luaL_checkudata(L, 1, kEventMeta));
    System(L).Fire(event->event, lua_gettop(L) - 1);
    return 0;
}

int ConnectionDisconnect(lua_State* L) {
    auto* c = static_cast<ConnectionHandle*>(luaL_checkudata(L, 1, kConnectionMeta));
    lua_pushboolean(L, System(L).Disconnect(c->event, c->connection) ? 1 : 0);
    return 1;
}

int ConnectionIsConnected(lua_State* L) {
    auto* c = static_cast<ConnectionHandle*>(luaL_checkudata(L, 1, kConnectionMeta));
    lua_pushboolean(L, System(L).IsConnected(c->event, c->connection) ? 1 : 0);
    return 1;
}

int StartTimer(lua_State* L, bool repeat) {
    const f64 seconds = luaL_checknumber(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    if (repeat && seconds <= 0.0) {
        luaL_error(L, "Timer.Every needs a positive interval");
    }
    lua_pushvalue(L, 2);
    const int ref = lua_ref(L, -1);
    lua_pop(L, 1);
    PushHandle(L, kTimerMeta, TimerHandle{System(L).StartTimer(seconds, repeat, ref)});
    return 1;
}

int TimerAfter(lua_State* L) { return StartTimer(L, false); }
int TimerEvery(lua_State* L) { return StartTimer(L, true); }

int TimerCancel(lua_State* L) {
    auto* t = static_cast<TimerHandle*>(luaL_checkudata(L, 1, kTimerMeta));
    lua_pushboolean(L, System(L).CancelTimer(t->timer) ? 1 : 0);
    return 1;
}

// Input.* without a bound input system reads as nothing pressed.
const input::ActionState& ActionOf(lua_State* L) {
    static const input::ActionState kNone;
    const char* name = luaL_checkstring(L, 1);
    input::InputSystem* input = System(L).BoundInput();
    return input != nullptr ? input->GetAction(name) : kNone;
}

int InputIsTriggered(lua_State* L) {
    lua_pushboolean(L, ActionOf(L).phase == input::ActionPhase::Triggered ? 1 : 0);
    return 1;
}
int InputGetAxis1D(lua_State* L) {
    lua_pushnumber(L, ActionOf(L).value.x);
    return 1;
}
int InputGetAxis2D(lua_State* L) {
    const Vec3& v = ActionOf(L).value;
    lua_pushvector(L, v.x, v.y, 0.0f);
    return 1;
}
int InputGetAxis3D(lua_State* L) {
    const Vec3& v = ActionOf(L).value;
    lua_pushvector(L, v.x, v.y, v.z);
    return 1;
}

int InputEventOf(lua_State* L, input::ActionEvent kind) {
    const char* name = luaL_checkstring(L, 1);
    PushEvent(L, System(L).InputEvent(name, kind));
    return 1;
}
int InputOnStarted(lua_State* L) { return InputEventOf(L, input::ActionEvent::Started); }
int InputOnTriggered(lua_State* L) { return InputEventOf(L, input::ActionEvent::Triggered); }
int InputOnCompleted(lua_State* L) { return InputEventOf(L, input::ActionEvent::Completed); }
int InputOnCanceled(lua_State* L) { return InputEventOf(L, input::ActionEvent::Canceled); }

void SetFunction(lua_State* L, const char* name, lua_CFunction fn) {
    lua_pushcfunction(L, fn, name);
    lua_setfield(L, -2, name);
}

void MethodsMeta(lua_State* L, const char* meta, std::initializer_list<std::pair<const char*, lua_CFunction>> methods) {
    luaL_newmetatable(L, meta);
    lua_createtable(L, 0, static_cast<int>(methods.size()));
    for (const auto& [name, fn] : methods) {
        SetFunction(L, name, fn);
    }
    lua_setfield(L, -2, "__index");
    lua_pop(L, 1);
}

const char* EventName(input::ActionEvent event) {
    switch (event) {
    case input::ActionEvent::Started: return "Started";
    case input::ActionEvent::Ongoing: return "Ongoing";
    case input::ActionEvent::Triggered: return "Triggered";
    case input::ActionEvent::Completed: return "Completed";
    case input::ActionEvent::Canceled: return "Canceled";
    }
    return "?";
}

} // namespace

void ScriptSystem::InstallApi(bool install_kit_apis) {
    lua_State* L = host_.State();
    lua_pushlightuserdata(L, this);
    lua_setfield(L, LUA_REGISTRYINDEX, kSystemKey);

    MethodsMeta(L, kEventMeta, {{"Connect", &EventConnect}, {"Fire", &EventFire}});
    MethodsMeta(L, kConnectionMeta, {{"Disconnect", &ConnectionDisconnect}, {"IsConnected", &ConnectionIsConnected}});
    MethodsMeta(L, kTimerMeta, {{"Cancel", &TimerCancel}});

    lua_createtable(L, 0, 1);
    SetFunction(L, "new", &EventNew);
    lua_setglobal(L, "Event");

    lua_createtable(L, 0, 2);
    SetFunction(L, "After", &TimerAfter);
    SetFunction(L, "Every", &TimerEvery);
    lua_setglobal(L, "Timer");

    lua_createtable(L, 0, 8);
    SetFunction(L, "IsTriggered", &InputIsTriggered);
    SetFunction(L, "GetAxis1D", &InputGetAxis1D);
    SetFunction(L, "GetAxis2D", &InputGetAxis2D);
    SetFunction(L, "GetAxis3D", &InputGetAxis3D);
    SetFunction(L, "OnStarted", &InputOnStarted);
    SetFunction(L, "OnTriggered", &InputOnTriggered);
    SetFunction(L, "OnCompleted", &InputOnCompleted);
    SetFunction(L, "OnCanceled", &InputOnCanceled);
    lua_setglobal(L, "Input");

#if AETHER_SCRIPTING_PHYSICS
    host_.RegisterNative("Physics", "AddForce", [this](NativeCall& call) {
        if (!physics_scene_) return call.Fail("Physics isn't bound to a running scene");
        if (!call.IsEntity(0) || !call.IsVector(1)) return call.Fail("Physics.AddForce(entity, forceVector) expected");
        const JPH::BodyID body = physics_scene_->BodyOf(call.EntityArg(0));
        if (body.IsInvalid()) return call.Return(false);
        auto& interface = physics_scene_->Physics().BodyInterface();
        if (interface.GetMotionType(body) != JPH::EMotionType::Dynamic) return call.Return(false);
        const Vec3 force = call.Vector(1);
        interface.AddForce(body, JPH::Vec3(force.x, force.y, force.z));
        call.Return(true);
    });
    host_.RegisterNative("Physics", "AddImpulse", [this](NativeCall& call) {
        if (!physics_scene_) return call.Fail("Physics isn't bound to a running scene");
        if (!call.IsEntity(0) || !call.IsVector(1)) return call.Fail("Physics.AddImpulse(entity, impulseVector) expected");
        const JPH::BodyID body = physics_scene_->BodyOf(call.EntityArg(0));
        if (body.IsInvalid()) return call.Return(false);
        auto& interface = physics_scene_->Physics().BodyInterface();
        if (interface.GetMotionType(body) != JPH::EMotionType::Dynamic) return call.Return(false);
        const Vec3 impulse = call.Vector(1);
        interface.AddImpulse(body, JPH::Vec3(impulse.x, impulse.y, impulse.z));
        call.Return(true);
    });
    host_.RegisterNative("Physics", "SetLinearVelocity", [this](NativeCall& call) {
        if (!physics_scene_) return call.Fail("Physics isn't bound to a running scene");
        if (!call.IsEntity(0) || !call.IsVector(1)) return call.Fail("Physics.SetLinearVelocity(entity, velocityVector) expected");
        const JPH::BodyID body = physics_scene_->BodyOf(call.EntityArg(0));
        if (body.IsInvalid()) return call.Return(false);
        auto& interface = physics_scene_->Physics().BodyInterface();
        if (interface.GetMotionType(body) != JPH::EMotionType::Dynamic) return call.Return(false);
        const Vec3 velocity = call.Vector(1);
        interface.SetLinearVelocity(body, JPH::Vec3(velocity.x, velocity.y, velocity.z));
        call.Return(true);
    });
    host_.RegisterNative("Physics", "GetLinearVelocity", [this](NativeCall& call) {
        if (!physics_scene_) return call.Fail("Physics isn't bound to a running scene");
        if (!call.IsEntity(0)) return call.Fail("Physics.GetLinearVelocity(entity) expected");
        const JPH::BodyID body = physics_scene_->BodyOf(call.EntityArg(0));
        if (body.IsInvalid()) return call.Return(ScriptValue{});
        const JPH::Vec3 velocity = physics_scene_->Physics().BodyInterface().GetLinearVelocity(body);
        call.ReturnVector(Vec3{velocity.GetX(), velocity.GetY(), velocity.GetZ()});
    });
    host_.RegisterNative("Physics", "Raycast", [this](NativeCall& call) {
        if (!physics_scene_) return call.Fail("Physics isn't bound to a running scene");
        if (!call.IsVector(0) || !call.IsVector(1) || !call.IsNumber(2))
            return call.Fail("Physics.Raycast(origin, direction, maxDistance, ignoreEntity?, layerMask?, includeTriggers?) expected");
        const Vec3 origin = call.Vector(0);
        Vec3 direction = call.Vector(1);
        const f32 distance = static_cast<f32>(call.Number(2));
        const f32 length = std::sqrt(direction.x * direction.x + direction.y * direction.y + direction.z * direction.z);
        if (!(distance > 0.0f) || !(length > 1e-6f)) {
            call.Return(false); call.Return(ScriptValue{}); call.Return(ScriptValue{}); call.Return(ScriptValue{}); call.Return(0.0);
            return;
        }
        direction = direction * (1.0f / length);
        const Entity ignore = call.IsEntity(3) ? call.EntityArg(3) : kNullEntity;
        const LayerMask layers = static_cast<LayerMask>(call.Number(4, kAllLayers));
        const SceneHit hit = physics_scene_->RayCast(origin, origin + direction * distance, layers, ignore, call.Bool(5));
        call.Return(hit.hit);
        call.Return(hit.hit && !hit.entity.IsNull() ? ScriptValue(EntityRef{hit.entity}) : ScriptValue{});
        if (hit.hit) {
            call.ReturnVector(hit.point);
            call.ReturnVector(hit.normal);
            call.Return(static_cast<f64>(hit.distance));
        } else {
            call.Return(ScriptValue{}); call.Return(ScriptValue{}); call.Return(0.0);
        }
    });
    host_.RegisterNative("Physics", "SphereCast", [this](NativeCall& call) {
        if (!physics_scene_) return call.Fail("Physics isn't bound to a running scene");
        if (!call.IsVector(0) || !call.IsVector(1) || !call.IsNumber(2) || !call.IsNumber(3))
            return call.Fail("Physics.SphereCast(origin, direction, radius, maxDistance, ignoreEntity?, layerMask?, includeTriggers?) expected");
        const Vec3 origin = call.Vector(0);
        Vec3 direction = call.Vector(1);
        const f32 radius = static_cast<f32>(call.Number(2));
        const f32 distance = static_cast<f32>(call.Number(3));
        const f32 length = std::sqrt(direction.x * direction.x + direction.y * direction.y + direction.z * direction.z);
        if (!(radius > 0.0f) || !(distance > 0.0f) || !(length > 1e-6f)) {
            call.Return(false); call.Return(ScriptValue{}); call.Return(ScriptValue{}); call.Return(ScriptValue{}); call.Return(0.0);
            return;
        }
        direction = direction * (1.0f / length);
        const Entity ignore = call.IsEntity(4) ? call.EntityArg(4) : kNullEntity;
        const LayerMask layers = static_cast<LayerMask>(call.Number(5, kAllLayers));
        const SceneHit hit = physics_scene_->SphereCast(radius, origin, origin + direction * distance,
                                                        layers, ignore, call.Bool(6));
        call.Return(hit.hit);
        call.Return(hit.hit && !hit.entity.IsNull() ? ScriptValue(EntityRef{hit.entity}) : ScriptValue{});
        if (hit.hit) {
            call.ReturnVector(hit.point);
            call.ReturnVector(hit.normal);
            call.Return(static_cast<f64>(hit.distance));
        } else {
            call.Return(ScriptValue{}); call.Return(ScriptValue{}); call.Return(0.0);
        }
    });
#endif

    InstallSaveApi(host_); // SaveGames (Phase 28 step 4)
    InstallLocApi(host_);  // Localization (Phase 29 step 2)
    if (install_kit_apis) {
        InstallKitApi("Quests");
        InstallKitApi("Interaction");
        InstallKitApi("Inventory");
        InstallKitApi("Gameplay");
    }
}

void ScriptSystem::InstallKitApi(const std::string& kit_name) {
    if (kit_name == "Gameplay") InstallGameplayApi(host_);
    else if (kit_name == "Inventory") InstallInventoryApi(host_);
    else if (kit_name == "Interaction") InstallInteractionApi(host_);
    else if (kit_name == "Quests") InstallQuestsApi(host_);
}

void ScriptSystem::RecordError(const std::string& error) {
    errors_.push_back(error);
    AETHER_LOG_ERROR("Script", "%s", error.c_str());
}

u32 ScriptSystem::NewEvent() {
    const u32 id = next_id_++;
    events_[id];
    return id;
}

u32 ScriptSystem::Connect(u32 event, int function_ref) {
    const u32 id = next_id_++;
    events_[event].connections.push_back({id, function_ref, current_owner_});
    return id;
}

bool ScriptSystem::Disconnect(u32 event, u32 connection) {
    auto it = events_.find(event);
    if (it == events_.end()) {
        return false;
    }
    auto& list = it->second.connections;
    auto c = std::find_if(list.begin(), list.end(), [&](const Connection& x) { return x.id == connection; });
    if (c == list.end()) {
        return false;
    }
    lua_unref(host_.State(), c->function_ref);
    list.erase(c);
    return true;
}

bool ScriptSystem::IsConnected(u32 event, u32 connection) const {
    auto it = events_.find(event);
    return it != events_.end() && std::any_of(it->second.connections.begin(), it->second.connections.end(),
                                              [&](const Connection& x) { return x.id == connection; });
}

usize ScriptSystem::ConnectionCount() const {
    usize count = 0;
    for (const auto& [id, event] : events_) {
        count += event.connections.size();
    }
    return count;
}

void ScriptSystem::Fire(u32 event, int args) {
    lua_State* L = host_.State();
    const int first_arg = lua_gettop(L) - args + 1;
    auto it = events_.find(event);
    if (it != events_.end()) {
        // A copy: handlers may connect or disconnect while this runs.
        const std::vector<Connection> connections = it->second.connections;
        for (const Connection& connection : connections) {
            if (!IsConnected(event, connection.id)) {
                continue; // disconnected by an earlier handler
            }
            lua_getref(L, connection.function_ref);
            for (int i = 0; i < args; ++i) {
                lua_pushvalue(L, first_arg + i);
            }
            const u64 previous_owner = current_owner_;
            current_owner_ = connection.owner; // what a handler connects belongs to the same script
            std::string error;
            if (!host_.ProtectedCall(args, 0, &error)) {
                RecordError(error + " (in an event handler)");
            }
            current_owner_ = previous_owner;
        }
    }
    lua_pop(L, args);
}

u32 ScriptSystem::StartTimer(f64 seconds, bool repeat, int function_ref) {
    const u32 id = next_id_++;
    const f64 interval = std::max(seconds, 0.0);
    timers_.push_back({id, function_ref, interval, interval, repeat, current_owner_});
    return id;
}

bool ScriptSystem::CancelTimer(u32 timer) {
    auto it = std::find_if(timers_.begin(), timers_.end(), [&](const Timer& t) { return t.id == timer; });
    if (it == timers_.end()) {
        return false;
    }
    lua_unref(host_.State(), it->function_ref);
    timers_.erase(it);
    return true;
}

void ScriptSystem::Tick(f32 dt) {
    lua_State* L = host_.State();
    std::vector<u32> ids;
    for (const Timer& timer : timers_) {
        ids.push_back(timer.id);
    }
    for (u32 id : ids) {
        auto find = [&]() { return std::find_if(timers_.begin(), timers_.end(), [&](const Timer& t) { return t.id == id; }); };
        auto it = find();
        if (it == timers_.end()) {
            continue; // cancelled by an earlier timer
        }
        it->remaining -= dt;
        // A long frame can make a repeating timer due several times; run
        // each (up to a limit), so Every(1) counts real seconds.
        for (int fired = 0; fired < 100; ++fired) {
            it = find();
            if (it == timers_.end() || it->remaining > 0.0) {
                break;
            }
            const Timer timer = *it;
            if (timer.repeat) {
                it->remaining += timer.interval;
            } else {
                timers_.erase(it); // unref after the call
            }
            lua_getref(L, timer.function_ref);
            const u64 previous_owner = current_owner_;
            current_owner_ = timer.owner;
            std::string error;
            if (!host_.ProtectedCall(0, 0, &error)) {
                RecordError(error + " (in a timer)");
            }
            current_owner_ = previous_owner;
            if (!timer.repeat) {
                lua_unref(L, timer.function_ref);
                break;
            }
        }
    }
}

void ScriptSystem::ReleaseOwner(u64 owner) {
    lua_State* L = host_.State();
    for (auto& [id, event] : events_) {
        auto& list = event.connections;
        for (auto it = list.begin(); it != list.end();) {
            if (it->owner == owner) {
                lua_unref(L, it->function_ref);
                it = list.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (auto it = timers_.begin(); it != timers_.end();) {
        if (it->owner == owner) {
            lua_unref(L, it->function_ref);
            it = timers_.erase(it);
        } else {
            ++it;
        }
    }
}

u32 ScriptSystem::InputEvent(const std::string& action, input::ActionEvent event) {
    const std::string key = action + "\n" + EventName(event);
    auto it = input_events_.find(key);
    if (it != input_events_.end()) {
        return it->second;
    }
    const u32 id = NewEvent();
    input_events_[key] = id;
    return id;
}

void ScriptSystem::BindInput(input::InputSystem* input) {
    if (input_ != nullptr) {
        for (u32 subscription : input_subscriptions_) {
            input_->Unsubscribe(subscription);
        }
    }
    input_subscriptions_.clear();
    input_ = input;
    if (input_ == nullptr) {
        return;
    }
    for (input::ActionEvent kind : {input::ActionEvent::Started, input::ActionEvent::Triggered,
                                    input::ActionEvent::Completed, input::ActionEvent::Canceled}) {
        input_subscriptions_.push_back(input_->Subscribe(
            "", kind, [this, kind](const std::string& action, input::ActionEvent, const input::ActionState& state) {
                auto it = input_events_.find(action + "\n" + EventName(kind));
                if (it == input_events_.end()) {
                    return; // no script listens
                }
                lua_pushvector(host_.State(), state.value.x, state.value.y, state.value.z);
                Fire(it->second, 1);
            }));
    }
}

bool ScriptSystem::SendEvent(Entity entity, const std::string& method, const std::vector<ScriptValue>& args) {
    Instance* instance = Find(entity);
    if (instance == nullptr) {
        return false;
    }
    lua_State* L = host_.State();
    lua_getref(L, instance->ref);
    lua_getfield(L, -1, method.c_str());
    const bool defined = lua_type(L, -1) == LUA_TFUNCTION;
    lua_pop(L, 2);
    if (!defined) {
        return false;
    }
    ScriptResult result = CallMethod(entity, method, args);
    if (!result.ok) {
        RecordError(result.error + " (in " + method + ")");
    }
    return true;
}

} // namespace aether::script
