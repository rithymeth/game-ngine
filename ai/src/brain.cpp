#include "aether/ai/brain.h"

namespace aether::ai {

BrainRuntime::BrainRuntime(const BehaviorTreeAsset& tree, u64 seed)
    : kind_(Kind::BehaviorTree), tree_(std::make_unique<BehaviorTreeInstance>(tree, seed)) {}
BrainRuntime::BrainRuntime(UtilityBrain brain)
    : kind_(Kind::Utility), utility_(std::make_unique<UtilityBrain>(std::move(brain))) {}
BrainRuntime::BrainRuntime(StateTree states)
    : kind_(Kind::StateTree), states_(std::make_unique<StateTree>(std::move(states))) {}

BtStatus BrainRuntime::Tick(BtContext& ctx, f32 dt) {
    switch (kind_) {
    case Kind::BehaviorTree:
        return tree_->Tick(ctx, dt);
    case Kind::Utility: {
        if (ctx.blackboard == nullptr) return BtStatus::Failure;
        const UtilityBrain::Choice choice = utility_->Choose(*ctx.blackboard);
        chosen_index_ = choice.valid ? static_cast<i32>(choice.index) : -1;
        chosen_action_ =
            (choice.valid && choice.index < utility_->Actions().size()) ? utility_->Actions()[choice.index].name : "-";
        return BtStatus::Running;
    }
    case Kind::StateTree: {
        if (ctx.blackboard == nullptr) return BtStatus::Failure;
        if (!states_started_) {
            if (!states_->Start(*ctx.blackboard)) return BtStatus::Failure;
            states_started_ = true;
            return BtStatus::Running;
        }
        states_->Tick(*ctx.blackboard);
        return states_->Running() ? BtStatus::Running : BtStatus::Failure;
    }
    }
    return BtStatus::Failure;
}

void BrainRuntime::Abort(BtContext& ctx) {
    switch (kind_) {
    case Kind::BehaviorTree:
        tree_->Abort(ctx);
        break;
    case Kind::Utility:
        utility_->Reset();
        chosen_index_ = -1;
        chosen_action_ = "-";
        break;
    case Kind::StateTree:
        if (states_started_ && ctx.blackboard != nullptr) states_->Stop(*ctx.blackboard);
        states_started_ = false;
        break;
    }
}

} // namespace aether::ai
