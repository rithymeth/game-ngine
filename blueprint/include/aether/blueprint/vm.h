#pragma once

#include "aether/blueprint/bytecode.h"

#include <functional>
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
    // Tick to every instance that has Event Tick, in attach order.
    void Tick(f32 delta_seconds);
    // BeginPlay to every instance.
    void BeginPlay();

    VmValue GetVariable(Entity entity, std::string_view name) const;
    bool SetVariable(Entity entity, std::string_view name, const VmValue& value);

    // Where Print String goes (default: the engine log, category "Blueprint").
    void SetPrintHandler(std::function<void(Entity, const std::string&)> handler) { print_ = std::move(handler); }

    const std::vector<RuntimeError>& Errors() const { return errors_; }
    void ClearErrors() { errors_.clear(); warned_.clear(); }
    u64 InstructionsRun() const { return instructions_; }

private:
    struct Instance {
        Entity entity;
        std::shared_ptr<const CompiledBlueprint> blueprint;
        std::vector<Reg> vars;
        std::vector<std::string> svars;
        bool detached = false; // detached while running: removed when the outermost run ends
    };
    struct Frame {
        std::vector<Reg> r;
        std::vector<std::string> s;
    };

    static u64 Key(Entity e) { return (static_cast<u64>(e.generation) << 32) | e.index; }
    Instance* Find(Entity e);
    const Instance* Find(Entity e) const;

    bool Run(Instance& instance, u32 function, Frame& frame, u32 depth);
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
    u64 budget_left_ = 0;
    u64 instructions_ = 0;
};

} // namespace aether::bp
