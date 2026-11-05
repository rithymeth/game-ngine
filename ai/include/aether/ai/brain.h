#pragma once

#include "aether/ai/bt_runtime.h"
#include "aether/ai/state_tree.h"
#include "aether/ai/utility.h"

#include <memory>
#include <string>

namespace aether::ai {

// Phase 40.4: one per-entity AI brain ticked the same way whether it is a
// behavior tree, a utility brain or a state tree, so the runtime loop and the
// debugger treat them uniformly. A behavior tree needs its asset to outlive the
// runtime (BehaviorTreeInstance's existing contract); the other two are owned by
// value. GOAP stays a one-shot PlanGoap call, not a ticked brain.
class BrainRuntime {
public:
    enum class Kind : u8 { BehaviorTree, Utility, StateTree };

    explicit BrainRuntime(const BehaviorTreeAsset& tree, u64 seed = 1);
    explicit BrainRuntime(UtilityBrain brain);
    explicit BrainRuntime(StateTree states);

    BrainRuntime(const BrainRuntime&) = delete;
    BrainRuntime& operator=(const BrainRuntime&) = delete;
    BrainRuntime(BrainRuntime&&) = default;
    BrainRuntime& operator=(BrainRuntime&&) = default;

    Kind GetKind() const { return kind_; }

    // Advances the brain one frame. A behavior tree reports its own BtStatus; a
    // utility brain picks (and remembers) an action and reports Running; a state
    // tree starts on the first tick and then transitions, reporting Running while
    // it has an active state and Failure once it stops (or when it can't start).
    BtStatus Tick(BtContext& ctx, f32 dt);

    // Stops the brain: a behavior tree aborts its running nodes, a state tree
    // exits its states, a utility brain forgets its choice.
    void Abort(BtContext& ctx);

    // The utility action chosen by the last Tick ("-" when none).
    const std::string& ChosenAction() const { return chosen_action_; }
    i32 ChosenIndex() const { return chosen_index_; }

    // What this runtime is, for the debugger; null for the other kinds.
    const BehaviorTreeInstance* Tree() const { return tree_.get(); }
    const UtilityBrain* Utility() const { return utility_.get(); }
    const StateTree* States() const { return states_.get(); }

private:
    Kind kind_;
    std::unique_ptr<BehaviorTreeInstance> tree_;
    std::unique_ptr<UtilityBrain> utility_;
    std::unique_ptr<StateTree> states_;
    bool states_started_ = false;
    std::string chosen_action_ = "-";
    i32 chosen_index_ = -1;
};

} // namespace aether::ai
