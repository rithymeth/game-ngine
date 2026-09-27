#pragma once

#include "aether/blueprint/bytecode.h"

#include <functional>
#include <map>
#include <tuple>
#include <random>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace aether {
class World;
class GuidIndex;
struct Transform;
} // namespace aether
namespace aether::assets {
struct AssetGuid;
}

namespace aether::bp {

// A value passed into or read out of a running Blueprint.
using VmValue = std::variant<std::monostate, bool, i32, f32, std::string, Vec3, Quaternion, Entity>;

// A problem while running (BLUEPRINT_NODES.md §13, runtime codes).
struct RuntimeError {
    std::string code; // BP201 invalid entity, BP202 instruction budget, BP203 call depth
    Entity entity;
    std::string function; // the event or function running
    NodeId node = 0;
    std::string message;
};

// The Blueprint debugger (Phase 12 step 5, ROADMAP.md §12.5).
enum class BpStopReason : u8 { Breakpoint, Step, Pause };
enum class BpAction : u8 { Continue, StepInto, StepOver, StepOut };

struct BpPinValue {
    NodeId node = 0;
    std::string pin;
    std::string type;  // "float"
    std::string value; // "2.5", "true", "(1, 0, 0)", "Entity(3v1)", "[4 items]"
};

struct BpFrame {
    Entity entity;
    std::string function; // "Event.BeginPlay", a function graph's name
    std::string graph;
    NodeId node = 0; // the node running in this frame
    std::vector<BpPinValue> values; // this function's pins, as they are now
};

struct BpStop {
    BpStopReason reason = BpStopReason::Breakpoint;
    Entity entity;
    std::string graph;
    NodeId node = 0;
    std::vector<BpFrame> frames; // innermost first
};

// One node starting to run, for the editor's wire animation.
struct BpTraceEvent {
    Entity entity;
    std::string graph;
    NodeId node = 0;
    u64 frame = 0; // BlueprintVM::Tick count
};

// Runs compiled Blueprints on a world's entities (Phase 12 step 2,
// docs/ROADMAP_DETAILS.md §C). Each attached entity is an instance with its
// own variables; events run to completion on the calling thread.
class BlueprintVM {
public:
    struct Options {
        u64 instruction_budget = 1'000'000; // per dispatch (BP202); 0 = none
        u32 max_call_depth = 64;            // nested function and custom event calls (BP203)
        u32 random_seed = 0;                // Random nodes; 0 = a different sequence each run
    };

    explicit BlueprintVM(World& world) : BlueprintVM(world, Options{}) {}
    BlueprintVM(World& world, Options options);

    // Makes `entity` an instance of `blueprint`, with variables at their
    // defaults except `overrides` ({"name": value}, for instance-editable and
    // expose-on-spawn variables; others are ignored). Replaces an existing instance.
    // Doesn't run BeginPlay; dispatch it when play starts.
    bool Attach(Entity entity, std::shared_ptr<const CompiledBlueprint> blueprint,
                const nlohmann::json& overrides = nlohmann::json::object());
    void Detach(Entity entity);
    bool IsAttached(Entity entity) const;
    usize InstanceCount() const { return instances_.size(); }

    // Runs an event ("Event.BeginPlay", "Event.Custom:Hit") on one instance.
    // False if it isn't attached, has no such event, or the event failed
    // (a runtime error is recorded). Arguments fill the event's outputs
    // (Tick's delta, a custom event's parameters) in order.
    bool Dispatch(Entity entity, std::string_view event, std::span<const VmValue> args = {});
    // One frame (§C.4): advances game time, resumes the latent actions that
    // are due (Delay's "completed", in wake-time order), then runs Event
    // Tick on every enabled instance that has it, in attach order.
    void Tick(f32 delta_seconds);
    // Disabled instances don't tick, and their latent actions wait (paused).
    void SetEnabled(Entity entity, bool enabled);
    f64 GameTime() const { return time_; }
    usize PendingLatentActions() const { return latent_.size(); }
    // BeginPlay to every instance.
    void BeginPlay();

    VmValue GetVariable(Entity entity, std::string_view name) const;
    bool SetVariable(Entity entity, std::string_view name, const VmValue& value);
    // Array variables, element by element (empty if it isn't one).
    std::vector<VmValue> GetArray(Entity entity, std::string_view name) const;
    bool SetArray(Entity entity, std::string_view name, const std::vector<VmValue>& items);

    // Needed for the hierarchy nodes (Get Parent, Attach To, world locations).
    void SetGuidIndex(GuidIndex* guids) { guids_ = guids; }
    // Spawn Blueprint: creates an entity running `blueprint` at `transform`
    // and returns it (BlueprintSystem sets this; without one, Spawn gives
    // no entity and BP206).
    // `exposed` holds the Spawn node's Expose on Spawn values ({"name": value}),
    // to apply when attaching the new instance (before its BeginPlay).
    using SpawnHandler = std::function<Entity(const assets::AssetGuid& blueprint, const Transform& transform,
                                              const nlohmann::json& exposed)>;
    void SetSpawnHandler(SpawnHandler handler) { spawn_ = std::move(handler); }
    // Destroy Entity is deferred until the running event finishes. Then this
    // runs, if set (BlueprintSystem: the lifecycle's Destroy). Otherwise the
    // VM runs EndPlay, detaches the instance, and destroys the entity.
    void SetDestroyHandler(std::function<void(Entity)> handler) { destroy_ = std::move(handler); }

    // Physics queries for the Line Trace, Sphere Trace and Overlap Sphere
    // nodes (Phase 13 step 4). The game connects them to its physics scene;
    // without them the nodes find nothing and warn (BP207). `radius` is 0
    // for a line; `ignore` is the running instance when "ignore self" is set.
    struct PhysicsHit {
        bool hit = false;
        Entity entity;
        Vec3 location{0, 0, 0}, normal{0, 0, 0};
        f32 distance = 0.0f;
    };
    struct PhysicsQueries {
        std::function<PhysicsHit(const Vec3& from, const Vec3& to, f32 radius, u32 layers, Entity ignore)> trace;
        std::function<std::vector<Entity>(const Vec3& center, f32 radius, u32 layers, Entity ignore)> overlap_sphere;
    };
    void SetPhysicsQueries(PhysicsQueries queries) { queries_ = std::move(queries); }

    // Where Print String goes (default: the engine log, category "Blueprint").
    void SetPrintHandler(std::function<void(Entity, const std::string&)> handler) { print_ = std::move(handler); }

    // --- Debugging -----------------------------------------------------
    // The handler runs at each stop, synchronously (PIE is paused inside it;
    // it must not run Blueprints on this VM). Null detaches the debugger.
    // Checks cost one test per instruction while a handler or the trace is on.
    void SetDebugHandler(std::function<BpAction(const BpStop&)> handler);
    // A breakpoint on a node of `blueprint` (F9). False if there's no such node.
    bool SetBreakpoint(const CompiledBlueprint& blueprint, const std::string& graph, NodeId node, bool enabled = true);
    void ClearBreakpoints() { breakpoints_.clear(); }
    // Stops at the next node any Blueprint runs (the Pause button).
    void RequestPause() { step_ = StepMode::Pause; }
    // Only stop for this instance ("Debug: BP_Door_2"); kNullEntity = any.
    void SetDebugFilter(Entity entity) { debug_filter_ = entity; }
    // Records every node that starts running (up to `capacity`, oldest dropped).
    void SetTraceEnabled(bool enabled, usize capacity = 4096);
    std::vector<BpTraceEvent> TakeTrace();
    usize DebugStops() const { return debug_stops_; }

    const std::vector<RuntimeError>& Errors() const { return errors_; }
    void ClearErrors() { errors_.clear(); warned_.clear(); }
    u64 InstructionsRun() const { return instructions_; }

private:
    struct NodeState {
        bool init = false;
        bool flag = false; // Do Once: done; Gate: closed; Flip Flop: took A last
        bool pending = false; // a latent action is waiting
        i32 counter = 0;   // Do N
    };
    // A suspended event (§C.4): its frame, and where to continue.
    struct LatentAction {
        u64 owner = 0;
        u32 function = 0;
        u32 resume_pc = 0;
        u16 slot = 0;
        NodeId node = 0;
        f64 wake_time = 0.0;
        u64 wake_frame = 0; // Delay Until Next Tick
        u64 order = 0;      // ties resume in start order
        std::vector<Reg> r;
        std::vector<std::string> s;
        std::vector<ArrayValue> a;
    };
    struct Instance {
        Entity entity;
        std::shared_ptr<const CompiledBlueprint> blueprint;
        std::vector<Reg> vars;
        std::vector<std::string> svars;
        std::vector<ArrayValue> avars;
        bool detached = false; // detached while running: removed when the outermost run ends
        bool enabled = true;
        std::vector<NodeState> states; // CompiledBlueprint::state_slots
    };
    struct Frame {
        std::vector<Reg> r;
        std::vector<std::string> s;
        std::vector<ArrayValue> a;
    };

    static u64 Key(Entity e) { return (static_cast<u64>(e.generation) << 32) | e.index; }
    Instance* Find(Entity e);
    const Instance* Find(Entity e) const;

    bool Run(Instance& instance, u32 function, Frame& frame, u32 depth, usize start_pc = 0);
    // Calls a Blueprint function with array items as its arguments (Sort's
    // comparator, Filter's predicate); its bool result goes to `result`.
    bool CallPredicate(Instance& instance, u32 function, const ArrayValue& array, usize a, const usize* b, u32 depth,
                       bool& result);
    void StartLatent(Instance& instance, u32 function, const Instr& in, Frame& frame);
    void ResumeDue();
    bool Call(Instance& instance, const FunctionCall& call, Frame& caller, u32 depth);
    void NativeCallOp(Instance& instance, const CompiledFunction& fn, const NativeCall& call, Frame& frame, NodeId node);
    void FieldOp(Instance& instance, const CompiledFunction& fn, const FieldAccess& access, Frame& frame, NodeId node,
                 bool write);
    void* ComponentOf(Entity target, const reflect::TypeInfo* owner);
    void Warn(const char* code, const Instance& instance, const CompiledFunction& fn, NodeId node, std::string message);
    Frame& AcquireFrame(const CompiledFunction& fn, u32 depth);

    void RemoveDetached();
    // Runs one of an attached instance's functions as an event (Dispatch
    // after the lookup).
    bool Invoke(Instance& instance, u32 function, std::span<const VmValue> args);
    std::vector<u64> detached_keys_; // instances detached while running, waiting for RemoveDetached
    void DebugHook(Instance& instance, u32 function, usize pc);
    BpStop DescribeStop(BpStopReason reason, const Instance& instance) const;
    void ProcessDestroys();
    void Finish(); // after an outermost run: detached instances, deferred destroys
    Transform* TransformOf(Instance& instance, const CompiledFunction& fn, NodeId node, Entity target);

    World& world_;
    Options options_;
    // Instances stay put while scripts run (attach/detach from a native call is safe).
    std::unordered_map<u64, std::unique_ptr<Instance>> instances_;
    std::vector<u64> order_; // attach order, for Tick and BeginPlay
    std::vector<Frame> frames_; // one per call depth, reused: no allocation per dispatch
    u32 depth_ = 0;             // frames in use
    std::function<void(Entity, const std::string&)> print_;
    std::vector<RuntimeError> errors_;
    std::unordered_map<u64, bool> warned_; // BP201 once per (function, node)
    GuidIndex* guids_ = nullptr;
    SpawnHandler spawn_;
    PhysicsQueries queries_;
    std::function<void(Entity)> destroy_;
    std::vector<Entity> pending_destroy_;
    bool destroying_ = false;
    f32 last_delta_ = 0.0f;
    // Event dispatcher bindings: (target, dispatcher) -> (listener, Custom Event).
    std::map<std::pair<u64, std::string>, std::vector<std::pair<u64, std::string>>> bindings_;
    std::vector<VmValue> ArgsOf(const std::vector<TypedReg>& args, const Frame& frame) const;
    enum class StepMode : u8 { None, Into, Over, Out, Pause };
    struct DebugFrame {
        Instance* instance = nullptr;
        u32 function = 0;
        const Frame* frame = nullptr;
        NodeId node = 0;
    };
    std::function<BpAction(const BpStop&)> debug_handler_;
    bool debugging_ = false; // a handler or the trace is on
    bool in_handler_ = false;
    std::vector<DebugFrame> debug_stack_;
    std::vector<std::tuple<const CompiledBlueprint*, std::string, NodeId>> breakpoints_;
    StepMode step_ = StepMode::None;
    usize step_depth_ = 0;
    Entity debug_filter_ = kNullEntity;
    bool tracing_ = false;
    usize trace_capacity_ = 4096;
    std::vector<BpTraceEvent> trace_;
    usize debug_stops_ = 0;
    std::vector<LatentAction> latent_;
    f64 time_ = 0.0;
    u64 frame_ = 0;
    u64 latent_order_ = 0;
    std::mt19937 rng_;
    u64 budget_left_ = 0;
    u64 instructions_ = 0;
};

} // namespace aether::bp
