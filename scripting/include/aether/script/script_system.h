#pragma once

#include "aether/assets/hot_reload.h"
#include "aether/input/actions.h"
#include "aether/scene/lifecycle.h"
#include "aether/scene/script_component.h"
#include "aether/script/luau_host.h"

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace aether { class PhysicsScene; }

namespace aether::script {

// A script error split into its parts, for the error overlay and the code
// editor ("Spin.luau:12: attempt to index nil" -> chunk, line, message).
struct ScriptError {
    std::string chunk;   // "Spin.luau" ("" if the text had no location)
    i32 line = 0;        // 0 if unknown
    std::string message;
    std::string text;    // the whole original error

    static ScriptError Parse(const std::string& text);
};

// Runs ScriptComponents (Phase 11 step 3).
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
//   Physics.AddForce / AddImpulse / SetLinearVelocity(entity, vector)
//   Physics.GetLinearVelocity(entity), Physics.Raycast(...), Physics.SphereCast(...)
//   Physics.Grab(object, holder, point, maxForce?, frequency?, damping?, breakForce?) -> grab id (0: none)
//   Physics.SetGrabTarget(grab, point), Physics.Release(grab), Physics.IsGrabActive(grab)
//   Physics.SetJointEnabled / SetJointLocked(entity, bool), Physics.SetJointMotorTarget(entity, number)
//   Physics.GetJointState(entity) -> enabled, locked, broken, angle (nil if none), Physics.RepairJoint(entity)
//   Scene.Load(path [, spawn])     -> true if the scene will load (false: the package has no such file, nothing changes). It does not load at once: the
//                                     game swaps the scenes at the start of its next frame, so the script that asks is not torn down under itself, and a
//                                     scene that then fails to parse leaves the current one running (with a warning). Everything of this scene's runtime
//                                     ends with the swap (scripts, timers, events); what must outlive it goes in SaveGames.
//                                     With `spawn` (the name of an entity in the new scene), the player (the entity with a CharacterMovement) starts
//                                     where that entity is, facing as it faces: a door's far side, instead of the scene's own start.
//   Camera.Shake(amount)           -> adds trauma 0..1 to the camera shake (0.2 a bump, 0.5 a hit, 1 an explosion); the shake grows with its square and
//                                     drains away. It moves the active camera after the scripts have run, so a script that sets the camera is not disturbed.
//   Scene.Current()                -> the path of the scene that is running
// Connections and timers made while one of an entity's callbacks runs belong
// to that entity's script, and end when it's destroyed (§11.2).
class AiDebugStore;

class ScriptSystem {
public:
    // Reads a script asset's source; false if it isn't available.
    using SourceLoader = std::function<bool(const assets::AssetGuid& script, std::string& source, std::string& name)>;

    // Binds `world` to `host`. Both must outlive the system.
    ScriptSystem(LuauHost& host, World& world, GuidIndex& guids, SourceLoader loader);
    ScriptSystem(LuauHost& host, World& world, GuidIndex& guids, SourceLoader loader, bool install_kit_apis);
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
    // Makes the current scene's ray queries and rigid-body controls available to Luau.
    void BindPhysicsScene(::aether::PhysicsScene* scene) { physics_scene_ = scene; }
    ::aether::PhysicsScene* BoundPhysicsScene() const { return physics_scene_; }
    // What the `Scene` table asks of the game that runs the scripts: `request` asks for another scene at the next frame (false: there is no such scene),
    // `current` is the running scene's path. Without them Scene.Load is a script error ("not bound to a running game").
    struct SceneHooks {
        std::function<bool(const std::string&, const std::string&)> request; // path, spawn (an entity's name; empty: none)
        std::function<std::string()> current;
    };
    void BindSceneHooks(SceneHooks hooks) { scene_hooks_ = std::move(hooks); }
    // What Camera.Shake does: the game adds trauma to its camera shake. Unbound, the call is a script error.
    void BindCameraShake(std::function<void(f32)> add_trauma) { add_camera_trauma_ = std::move(add_trauma); }
    // Calls `method` on the entity's script if it defines it (e.g.
    // "OnCollisionBegin" from physics); false if it has no script or method.
    bool SendEvent(Entity entity, const std::string& method, const std::vector<ScriptValue>& args = {});

    // Hot reload (§11.3): recompiles `script`. If it fails, running
    // instances keep the old code and the error is kept (ReloadErrors) until
    // a later reload succeeds. If it succeeds, every live instance keeps its
    // own fields (its state), switches to the new class (so methods and the
    // defaults it doesn't override are the new ones), and gets :OnReload()
    // if the class defines it. Event connections made by the old code stay.
    struct ReloadResult {
        bool ok = false;
        usize instances = 0; // live instances switched to the new code
        std::string error;
    };
    ReloadResult Reload(const assets::AssetGuid& script);
    // Reloads the scripts among a HotReloader's changes (.luau files that
    // changed or came back). Returns how many reloads were attempted.
    usize ReloadChanged(const std::vector<assets::AssetChange>& changes);
    // Scripts whose latest reload failed, with the error (the overlay's list).
    const std::unordered_map<assets::AssetGuid, ScriptError>& ReloadErrors() const { return reload_errors_; }

    // A SourceLoader reading scripts from an asset database (must outlive it).
    static SourceLoader DatabaseLoader(const assets::AssetDatabase& database);

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
    // The AI debugger's data (AI.SetDebugState, Nav.MoveTowards' paths; ai_api.h). Never null.
    const AiDebugStore& AiDebug() const { return *ai_debug_; }
    // What went wrong while play ran, one entry per distinct error: "<script>:<line>: <message> (in OnUpdate, on entity "Door" a0b1c2d3)". An error that happens again (a broken OnUpdate fails
    // every frame) is not added again: its entry gains " (repeated N times)", so the list stays readable and does not grow for as long as the game runs. At most kMaxDistinctErrors entries are kept;
    // further distinct errors are counted in DroppedErrors() (they are still logged).
    static constexpr usize kMaxDistinctErrors = 200;
    const std::vector<std::string>& Errors() const { return errors_; }
    usize DroppedErrors() const { return errors_dropped_; }
    void ClearErrors() { errors_.clear(); error_seen_.clear(); errors_dropped_ = 0; }

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
    void InstallKitApi(const std::string& kit_name);

private:
    void InstallApi(bool install_kit_apis);
    void ReleaseOwner(u64 owner);
    void RecordError(const std::string& error);
    // `entity` as a person would name it: "entity \"Door\" a0b1c2d3" (the name when it has one, and the start of its id).
    std::string DescribeEntity(Entity entity) const;

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
    struct SeenError { usize index = 0; u32 count = 0; std::string text; };
    std::unordered_map<std::string, SeenError> error_seen_; // by the error as first recorded: where it is in errors_ and how often it happened
    usize errors_dropped_ = 0;

    std::unordered_map<u32, EventData> events_;
    std::vector<Timer> timers_;
    std::unordered_map<assets::AssetGuid, ScriptError> reload_errors_;
    // Entities in play whose script failed to load, by script: started when it reloads.
    std::unordered_map<EntityGuid, assets::AssetGuid> waiting_;
    std::unordered_map<std::string, u32> input_events_; // "action\nevent" -> event id
    input::InputSystem* input_ = nullptr;
    ::aether::PhysicsScene* physics_scene_ = nullptr;
    SceneHooks scene_hooks_;
    std::function<void(f32)> add_camera_trauma_;
    std::vector<u32> input_subscriptions_;
    u32 next_id_ = 1;
    u64 next_owner_ = 1;
    u64 current_owner_ = 0; // the instance whose callback is running
    std::shared_ptr<AiDebugStore> ai_debug_;
};

} // namespace aether::script
