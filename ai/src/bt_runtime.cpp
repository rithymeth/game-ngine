#include "aether/ai/bt_runtime.h"

#include "aether/nav/crowd.h"
#include "aether/scene/components.h"

#include <algorithm>
#include <cmath>

namespace aether::ai {

namespace {
bool HasAbort(const BtDecorator& d, BtAbort which) {
    return d.type == BtDecoratorType::BlackboardCondition && (d.abort == which || d.abort == BtAbort::Both);
}
bool PositionOf(const BtContext& ctx, const BlackboardValue& v, Vec3& out) {
    if (const Vec3* p = std::get_if<Vec3>(&v)) return out = *p, true;
    if (const Entity* e = std::get_if<Entity>(&v); e != nullptr && ctx.world != nullptr && ctx.world->IsAlive(*e)) {
        if (const Transform* t = ctx.world->GetComponent<Transform>(*e)) return out = t->position, true;
    }
    return false;
}
} // namespace

BehaviorTreeInstance::BehaviorTreeInstance(const BehaviorTreeAsset& tree, u64 seed) : tree_(tree), rng_(seed * 2862933555777941757ULL + 3037000493ULL) {
    Flatten(tree.root, -1, 0, tree.root.Label());
    state_.resize(nodes_.size());
}

void BehaviorTreeInstance::Flatten(const BtNode& n, i32 parent, i32 depth, const std::string& path) {
    const usize me = nodes_.size();
    nodes_.push_back({&n, parent, depth, path});
    children_.emplace_back();
    if (parent >= 0) children_[static_cast<usize>(parent)].push_back(me);
    for (usize c = 0; c < n.children.size(); ++c) Flatten(n.children[c], static_cast<i32>(me), depth + 1, path + "/" + n.children[c].Label() + "[" + std::to_string(c) + "]");
}

f32 BehaviorTreeInstance::Random01() {
    rng_ = rng_ * 6364136223846793005ULL + 1442695040888963407ULL;
    return static_cast<f32>((rng_ >> 40) & 0xFFFFFF) / 16777216.0f;
}

bool BehaviorTreeInstance::Condition(const BtDecorator& d, const Blackboard& bb) const {
    const BlackboardValue* v = bb.Get(d.key);
    const bool set = v != nullptr && !(std::holds_alternative<Entity>(*v) && std::get<Entity>(*v).IsNull());
    switch (d.op) {
    case BtCompare::IsSet: return set;
    case BtCompare::IsNotSet: return !set;
    case BtCompare::Equal: return v != nullptr && ValuesEqual(*v, d.value);
    case BtCompare::NotEqual: return v == nullptr || !ValuesEqual(*v, d.value);
    default: break;
    }
    f64 a = 0.0, b = 0.0;
    if (!bb.GetNumber(d.key, a)) return false;
    if (const i32* i = std::get_if<i32>(&d.value)) b = *i;
    else if (const f32* f = std::get_if<f32>(&d.value)) b = *f;
    else return false;
    switch (d.op) {
    case BtCompare::Less: return a < b;
    case BtCompare::LessEqual: return a <= b;
    case BtCompare::Greater: return a > b;
    case BtCompare::GreaterEqual: return a >= b;
    default: return false;
    }
}

bool BehaviorTreeInstance::Gate(usize i, const BtContext& ctx) const {
    for (const BtDecorator& d : nodes_[i].node->decorators) {
        if (d.type == BtDecoratorType::BlackboardCondition && !Condition(d, *ctx.blackboard)) return false;
        if (d.type == BtDecoratorType::Cooldown && now_ < state_[i].cooldown_until) return false;
    }
    return true;
}

void BehaviorTreeInstance::End(usize i, BtStatus status) {
    State& st = state_[i];
    st.active = false;
    st.last = status;
    for (const BtDecorator& d : nodes_[i].node->decorators)
        if (d.type == BtDecoratorType::Cooldown) st.cooldown_until = now_ + d.seconds;
}

void BehaviorTreeInstance::AbortNode(usize i, BtContext& ctx) {
    State& st = state_[i];
    if (!st.active) return;
    for (usize c : children_[i]) AbortNode(c, ctx);
    const BtNode& n = *nodes_[i].node;
    if (n.type == BtNodeType::MoveTo && ctx.world != nullptr) {
        if (NavAgent* agent = ctx.world->GetComponent<NavAgent>(ctx.entity)) agent->Stop();
    }
    if (n.type == BtNodeType::RunBlueprint && st.latent && ctx.hooks != nullptr && ctx.hooks->abort_blueprint) ctx.hooks->abort_blueprint(ctx.entity, n.event);
    st.latent = false;
    End(i, BtStatus::Failure);
}

void BehaviorTreeInstance::Abort(BtContext& ctx) { AbortNode(0, ctx); }

bool BehaviorTreeInstance::FinishLatent(BtStatus result, const std::string& event) {
    for (usize i = 0; i < nodes_.size(); ++i) {
        State& st = state_[i];
        if (st.active && st.latent && st.latent_result == BtStatus::Running && (event.empty() || nodes_[i].node->event == event)) {
            st.latent_result = result == BtStatus::Running ? BtStatus::Success : result;
            return true;
        }
    }
    return false;
}

void BehaviorTreeInstance::RunServices(usize i, BtContext& ctx, f32 dt) {
    const BtNode& n = *nodes_[i].node;
    State& st = state_[i];
    for (usize k = 0; k < n.services.size(); ++k) {
        if (now_ < st.service_next[k]) continue;
        const BtService& s = n.services[k];
        st.service_next[k] = now_ + std::max(s.interval, 1e-3f);
        switch (s.type) {
        case BtServiceType::Blueprint:
            if (ctx.hooks != nullptr && ctx.hooks->blueprint_service) ctx.hooks->blueprint_service(ctx.entity, s.event);
            break;
        case BtServiceType::Luau:
            if (ctx.hooks != nullptr && ctx.hooks->luau_service) ctx.hooks->luau_service(ctx.entity, s.event, dt);
            break;
        case BtServiceType::DistanceTo: {
            const Transform* me = ctx.world != nullptr ? ctx.world->GetComponent<Transform>(ctx.entity) : nullptr;
            const BlackboardValue* target = ctx.blackboard->Get(s.key);
            Vec3 at;
            if (me != nullptr && target != nullptr && PositionOf(ctx, *target, at)) ctx.blackboard->Set(s.out_key, (at - me->position).Length());
            else ctx.blackboard->Clear(s.out_key);
            break;
        }
        }
    }
}

BtStatus BehaviorTreeInstance::Run(usize i, BtContext& ctx, f32 dt) {
    State& st = state_[i];
    const BtNode& n = *nodes_[i].node;
    bool entering = false;
    if (!st.active) {
        if (!Gate(i, ctx)) {
            st.last = BtStatus::Failure;
            return BtStatus::Failure;
        }
        st.active = true;
        st.started = now_;
        st.loops = 0;
        st.child = 0;
        st.first = true;
        st.latent = false;
        st.service_next.assign(n.services.size(), now_);
        entering = true;
    } else {
        // Conditions that abort their own branch when they turn false.
        for (const BtDecorator& d : n.decorators) {
            if (HasAbort(d, BtAbort::Self) && !Condition(d, *ctx.blackboard)) {
                AbortNode(i, ctx);
                return BtStatus::Failure;
            }
        }
    }
    for (const BtDecorator& d : n.decorators) {
        if (d.type == BtDecoratorType::TimeLimit && now_ - st.started >= d.seconds && !entering) {
            AbortNode(i, ctx);
            return BtStatus::Failure;
        }
    }
    RunServices(i, ctx, dt);
    BtStatus status = Execute(i, ctx, dt, entering);
    if (status == BtStatus::Running) return status;
    for (const BtDecorator& d : n.decorators) {
        if (d.type == BtDecoratorType::Loop && status == BtStatus::Success && (d.count == 0 || ++st.loops < d.count)) {
            // Again, from the next tick.
            for (usize c : children_[i]) state_[c].active = false;
            st.child = 0;
            st.first = true;
            st.latent = false;
            st.started = now_;
            st.par.clear();
            return BtStatus::Running;
        }
    }
    for (const BtDecorator& d : n.decorators) {
        if (d.type == BtDecoratorType::Inverter) status = status == BtStatus::Success ? BtStatus::Failure : BtStatus::Success;
        if (d.type == BtDecoratorType::ForceSuccess) status = BtStatus::Success;
        if (d.type == BtDecoratorType::ForceFailure) status = BtStatus::Failure;
    }
    End(i, status);
    return status;
}

BtStatus BehaviorTreeInstance::Execute(usize i, BtContext& ctx, f32 dt, bool entering) {
    const BtNode& n = *nodes_[i].node;
    State& st = state_[i];
    const std::vector<usize>& kids = children_[i];
    switch (n.type) {
    case BtNodeType::Sequence:
        while (st.child < static_cast<i32>(kids.size())) {
            const BtStatus r = Run(kids[static_cast<usize>(st.child)], ctx, dt);
            if (r != BtStatus::Success) return r;
            ++st.child;
        }
        return BtStatus::Success;
    case BtNodeType::Selector: {
        // A higher-priority child whose watched conditions now pass takes over.
        for (i32 j = 0; j < st.child && j < static_cast<i32>(kids.size()); ++j) {
            const usize k = kids[static_cast<usize>(j)];
            const auto& decs = nodes_[k].node->decorators;
            if (std::any_of(decs.begin(), decs.end(), [](const BtDecorator& d) { return HasAbort(d, BtAbort::LowerPriority); }) && Gate(k, ctx)) {
                AbortNode(kids[static_cast<usize>(st.child)], ctx);
                st.child = j;
                break;
            }
        }
        while (st.child < static_cast<i32>(kids.size())) {
            const BtStatus r = Run(kids[static_cast<usize>(st.child)], ctx, dt);
            if (r != BtStatus::Failure) return r;
            ++st.child;
        }
        return BtStatus::Failure;
    }
    case BtNodeType::Parallel: {
        if (entering || st.par.size() != kids.size()) st.par.assign(kids.size(), BtStatus::Running);
        for (usize c = 0; c < kids.size(); ++c)
            if (st.par[c] == BtStatus::Running) st.par[c] = Run(kids[c], ctx, dt);
        const auto count = [&](BtStatus s) { return static_cast<usize>(std::count(st.par.begin(), st.par.end(), s)); };
        BtStatus result = BtStatus::Running;
        if (n.succeed_on_one) {
            if (count(BtStatus::Success) > 0) result = BtStatus::Success;
            else if (count(BtStatus::Failure) == kids.size()) result = BtStatus::Failure;
        } else {
            if (count(BtStatus::Failure) > 0) result = BtStatus::Failure;
            else if (count(BtStatus::Success) == kids.size()) result = BtStatus::Success;
        }
        if (result != BtStatus::Running)
            for (usize c : kids) AbortNode(c, ctx);
        return result;
    }
    case BtNodeType::Wait:
        if (entering) st.wait_until = now_ + std::max(0.0f, n.seconds + n.deviation * (Random01() * 2.0f - 1.0f));
        return now_ + 1e-9 >= st.wait_until ? BtStatus::Success : BtStatus::Running;
    case BtNodeType::MoveTo: {
        NavAgent* agent = ctx.world != nullptr ? ctx.world->GetComponent<NavAgent>(ctx.entity) : nullptr;
        if (agent == nullptr) return BtStatus::Failure;
        const u64 rev = ctx.blackboard->Revision(n.key);
        const BlackboardValue* target = ctx.blackboard->Get(n.key);
        const bool goal_moved = !entering && rev != st.key_revision && target != nullptr && std::holds_alternative<Vec3>(*target);
        if (entering || goal_moved) {
            // Start, or the goal (a point) moved: (re)issue the move.
            st.key_revision = rev;
            if (target == nullptr) return BtStatus::Failure;
            agent->stopping_distance = n.acceptance;
            if (const Vec3* p = std::get_if<Vec3>(target)) agent->MoveTo(*p);
            else if (const Entity* e = std::get_if<Entity>(target); e != nullptr && !e->IsNull()) agent->MoveToEntity(*e);
            else return BtStatus::Failure;
            return BtStatus::Running;
        }
        if (!agent->commands.empty() || agent->status == NavMoveStatus::Moving) return BtStatus::Running;
        return agent->status == NavMoveStatus::Arrived ? BtStatus::Success : BtStatus::Failure;
    }
    case BtNodeType::SetBlackboard: return ctx.blackboard->Set(n.key, n.value) ? BtStatus::Success : BtStatus::Failure;
    case BtNodeType::ClearBlackboard: return ctx.blackboard->Clear(n.key) ? BtStatus::Success : BtStatus::Failure;
    case BtNodeType::RunBlueprint:
        if (entering) {
            // Waiting before dispatching: the Blueprint may finish the task while it runs.
            st.latent = true;
            st.latent_result = BtStatus::Running;
            if (ctx.hooks == nullptr || !ctx.hooks->run_blueprint || !ctx.hooks->run_blueprint(ctx.entity, n.event)) {
                st.latent = false;
                return BtStatus::Failure;
            }
        }
        if (st.latent_result != BtStatus::Running) {
            st.latent = false;
            return st.latent_result;
        }
        return BtStatus::Running;
    case BtNodeType::RunLuau: {
        if (ctx.hooks == nullptr || !ctx.hooks->run_luau) return BtStatus::Failure;
        const BtStatus r = ctx.hooks->run_luau(ctx.entity, n.event, dt, st.first);
        st.first = false;
        return r;
    }
    case BtNodeType::Log:
        if (ctx.hooks != nullptr && ctx.hooks->log) ctx.hooks->log(ctx.entity, n.event);
        return BtStatus::Success;
    case BtNodeType::Succeed: return BtStatus::Success;
    case BtNodeType::Fail: return BtStatus::Failure;
    }
    return BtStatus::Failure;
}

BtStatus BehaviorTreeInstance::Tick(BtContext& ctx, f32 dt) {
    now_ += dt;
    const BtStatus r = Run(0, ctx, dt);
    if (r != BtStatus::Running) ++root_runs_;
    return r;
}

std::vector<usize> BehaviorTreeInstance::ActivePath() const {
    std::vector<usize> out;
    for (usize i = 0; i < nodes_.size(); ++i)
        if (state_[i].active) out.push_back(i);
    return out;
}

std::string BehaviorTreeInstance::ActiveLabel() const {
    i32 best = -1, depth = -1;
    for (usize i = 0; i < nodes_.size(); ++i)
        if (state_[i].active && nodes_[i].depth > depth) best = static_cast<i32>(i), depth = nodes_[i].depth;
    return best < 0 ? std::string() : nodes_[static_cast<usize>(best)].node->Label();
}

} // namespace aether::ai
