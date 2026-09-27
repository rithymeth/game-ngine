#pragma once

#include "aether/blueprint/bytecode.h"

#include <functional>
#include <random>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace aether {
class World;
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
    // defaults except `overrides` ({"name": value}, for instance-editable
    // variables; others are ignored). Replaces an existing instance.
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

    // Where Print String goes (default: the engine log, category "Blueprint").
    void SetPrintHandler(std::function<void(Entity, const std::string&)> handler) { print_ = std::move(handler); }

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
    std::vector<LatentAction> latent_;
    f64 time_ = 0.0;
    u64 frame_ = 0;
    u64 latent_order_ = 0;
    std::mt19937 rng_;
    u64 budget_left_ = 0;
    u64 instructions_ = 0;
};

} // namespace aether::bp
