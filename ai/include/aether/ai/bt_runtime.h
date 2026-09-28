#pragma once

#include "aether/ai/behavior_tree.h"
#include "aether/ecs/world.h"

#include <functional>

namespace aether::ai {

// Where Blueprint and Luau tasks and services go (BehaviorTreeWorld fills them).
struct BtHooks {
    std::function<void(Entity, const std::string& text)> log;
    // Starts a Blueprint task: dispatch Event.Custom:<event>; false when it can't.
    std::function<bool(Entity, const std::string& event)> run_blueprint;
    // A running Blueprint task was aborted (dispatch Event.Custom:<event>Aborted, say).
    std::function<void(Entity, const std::string& event)> abort_blueprint;
    // A Luau task's tick: `first` on the tick it starts.
    std::function<BtStatus(Entity, const std::string& function, f32 dt, bool first)> run_luau;
    std::function<void(Entity, const std::string& event)> blueprint_service;
    std::function<void(Entity, const std::string& function, f32 dt)> luau_service;
};

struct BtContext {
    World* world = nullptr; // for MoveTo (the entity's NavAgent) and DistanceTo (Transforms)
    Entity entity = kNullEntity;
    Blackboard* blackboard = nullptr;
    const BtHooks* hooks = nullptr;
};

// One running tree (Phase 20 step 4, docs/design/PHASE_SPECS.md §20.4). The
// asset must outlive it. Each Tick walks from the root down the running
// branch, re-checking conditions that abort; when the root ends it starts
// over on the next tick.
class BehaviorTreeInstance {
public:
    explicit BehaviorTreeInstance(const BehaviorTreeAsset& tree, u64 seed = 1);

    BtStatus Tick(BtContext& ctx, f32 dt);
    // Stops every running node (MoveTo stops its agent, Blueprint tasks hear it).
    void Abort(BtContext& ctx);
    // Ends a waiting Blueprint task (the one running `event`, or any when empty). False when none waits.
    bool FinishLatent(BtStatus result, const std::string& event = {});

    struct NodeInfo {
        const BtNode* node = nullptr;
        i32 parent = -1;
        i32 depth = 0;
        std::string path;
    };
    // Every node, depth-first from the root (index 0), for the debugger.
    const std::vector<NodeInfo>& Nodes() const { return nodes_; }
    bool IsActive(usize node) const { return state_[node].active; }
    // What the node last ended with (Running when it hasn't ended yet).
    BtStatus LastStatus(usize node) const { return state_[node].last; }
    std::vector<usize> ActivePath() const;
    std::string ActiveLabel() const; // the deepest active node's label
    bool Running() const { return state_[0].active; }
    f64 Time() const { return now_; }
    u64 RootRuns() const { return root_runs_; } // times the root ended
    const BehaviorTreeAsset& Tree() const { return tree_; }

private:
    struct State {
        bool active = false;
        i32 child = 0;
        f64 started = 0.0, wait_until = 0.0, cooldown_until = -1e300;
        i32 loops = 0;
        bool latent = false, first = true;
        BtStatus latent_result = BtStatus::Running;
        BtStatus last = BtStatus::Running;
        u64 key_revision = 0;
        std::vector<BtStatus> par;
        std::vector<f64> service_next;
    };
    void Flatten(const BtNode& n, i32 parent, i32 depth, const std::string& path);
    bool Condition(const BtDecorator& d, const Blackboard& bb) const;
    bool Gate(usize i, const BtContext& ctx) const; // conditions and cooldown allow entering
    BtStatus Run(usize i, BtContext& ctx, f32 dt);
    BtStatus Execute(usize i, BtContext& ctx, f32 dt, bool entering);
    void AbortNode(usize i, BtContext& ctx);
    void End(usize i, BtStatus status);
    void RunServices(usize i, BtContext& ctx, f32 dt);
    f32 Random01();

    const BehaviorTreeAsset& tree_;
    std::vector<NodeInfo> nodes_;
    std::vector<std::vector<usize>> children_;
    std::vector<State> state_;
    f64 now_ = 0.0;
    u64 rng_;
    u64 root_runs_ = 0;
};

} // namespace aether::ai
