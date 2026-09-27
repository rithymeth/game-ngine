#pragma once

#include "aether/input/actions.h"
#include "aether/scene/lifecycle.h"
#include "aether/scene/script_component.h"
#include "aether/script/luau_host.h"

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace aether::script {

// Runs ScriptComponents (Phase 11 step 3, docs/design/PHASE_SPECS.md §11.2).
//
// A script is a module that returns a table, its "class":
//
//   local Spin = {}
//   --@range 0 720
//   Spin.speed = 180
//   function Spin:OnUpdate(dt) ... self.entity:Get("Transform") ... end
//   return Spin
//
// Each entity with a ScriptComponent gets an instance table (class as
// metatable, `self.entity` set, the component's overridden variables copied
// in), and the class's OnCreate / OnEnable / OnStart / OnUpdate(dt) /
// OnFixedUpdate(dt) / OnLateUpdate(dt) / OnDisable / OnDestroy are called
// through Lifecycle, when defined. A script error in one callback is
// recorded (Errors()) and play goes on.
//
// Script APIs (Phase 11 step 4):
//   Event.new()                    -> event with :Connect(fn), :Fire(...)
//   event:Connect(fn)              -> connection with :Disconnect(), :IsConnected()
//   Timer.After(seconds, fn)       -> timer with :Cancel()   (runs once)
//   Timer.Every(seconds, fn)       -> timer with :Cancel()   (repeats)
//   Input.IsTriggered(action), Input.GetAxis1D / GetAxis2D / GetAxis3D(action)
//   Input.OnStarted / OnTriggered / OnCompleted / OnCanceled(action) -> event (fired with the action's value)
// Connections and timers made while one of an entity's callbacks runs belong
// to that entity's script, and end when it's destroyed (§11.2).
class ScriptSystem {
public:
    // Reads a script asset's source; false if it isn't available.
    using SourceLoader = std::function<bool(const assets::AssetGuid& script, std::string& source, std::string& name)>;

    // Binds `world` to `host`. Both must outlive the system.
    ScriptSystem(LuauHost& host, World& world, GuidIndex& guids, SourceLoader loader);
    ~ScriptSystem();
    ScriptSystem(const ScriptSystem&) = delete;
    ScriptSystem& operator=(const ScriptSystem&) = delete;

    // Registers the ScriptComponent callbacks.
    void Register(Lifecycle& lifecycle);

    // Advances timers (call once per frame, in game time).
    void Tick(f32 dt);
    // Connects the Input API to an input system (null to disconnect). The
    // system must outlive the binding.
    void BindInput(input::InputSystem* input);
    // Calls `method` on the entity's script if it defines it (e.g.
    // "OnCollisionBegin" from physics); false if it has no script or method.
    bool SendEvent(Entity entity, const std::string& method, const std::vector<ScriptValue>& args = {});

    usize ConnectionCount() const;
    usize TimerCount() const { return timers_.size(); }

    // The script's class (loaded and cached on first use): its exposed
    // variables and callbacks, for the Inspector.
    ScriptClassInfo ClassInfo(const assets::AssetGuid& script);

    // Calls a method on the entity's script instance (e.g. from an event).
    ScriptResult CallMethod(Entity entity, const std::string& method, const std::vector<ScriptValue>& args = {});
    // A field of the entity's script instance (e.g. an exposed variable).
    ScriptValue GetField(Entity entity, const std::string& field);

    usize InstanceCount() const { return instances_.size(); }
    const std::vector<std::string>& Errors() const { return errors_; }
    void ClearErrors() { errors_.clear(); }

    // Loads a class from source into `host` and describes it, without
    // creating any instances. `ref` receives the class table's registry
    // reference (or LUA_NOREF on failure) when given.
    static ScriptClassInfo DescribeSource(LuauHost& host, const std::string& source, const std::string& name,
                                          int* ref = nullptr);

private:
    struct Class {
        int ref = -1;      // the class table
        int meta_ref = -1; // {__index = class} for instances
        std::string name;
        ScriptClassInfo info;
    };
    struct Instance {
        int ref = -1;
        assets::AssetGuid script;
        std::string name;
        u64 owner = 0; // identifies what this instance's code connects
    };
    struct Connection {
        u32 id = 0;
        int function_ref = -1;
        u64 owner = 0; // 0: made outside any instance (never auto-disconnected)
    };
    struct EventData {
        std::vector<Connection> connections;
    };
    struct Timer {
        u32 id = 0;
        int function_ref = -1;
        f64 interval = 0.0;
        f64 remaining = 0.0;
        bool repeat = false;
        u64 owner = 0;
    };

public:
    // For the Luau C functions (script_api.cpp).
    u32 NewEvent();
    u32 Connect(u32 event, int function_ref);
    bool Disconnect(u32 event, u32 connection);
    bool IsConnected(u32 event, u32 connection) const;
    void Fire(u32 event, int args); // the args are on the Luau stack top
    u32 StartTimer(f64 seconds, bool repeat, int function_ref);
    bool CancelTimer(u32 timer);
    input::InputSystem* BoundInput() { return input_; }
    u32 InputEvent(const std::string& action, input::ActionEvent event);

private:
    void InstallApi();
    void ReleaseOwner(u64 owner);
    void RecordError(const std::string& error);

    Class* LoadClass(const assets::AssetGuid& script);
    void Create(Entity entity);
    void Destroy(Entity entity);
    void Invoke(Entity entity, const char* method, const f32* dt = nullptr);
    Instance* Find(Entity entity);

    LuauHost& host_;
    World& world_;
    GuidIndex& guids_;
    SourceLoader loader_;
    std::unordered_map<assets::AssetGuid, Class> classes_;
    std::unordered_map<EntityGuid, Instance> instances_;
    std::vector<std::string> errors_;

    std::unordered_map<u32, EventData> events_;
    std::vector<Timer> timers_;
    std::unordered_map<std::string, u32> input_events_; // "action\nevent" -> event id
    input::InputSystem* input_ = nullptr;
    std::vector<u32> input_subscriptions_;
    u32 next_id_ = 1;
    u64 next_owner_ = 1;
    u64 current_owner_ = 0; // the instance whose callback is running
};

} // namespace aether::script
