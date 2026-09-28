#include "aether/ai/bt_world.h"

#include "aether/nav/crowd.h"

namespace aether {

namespace {
ai::BehaviorTreeWorld* g_active = nullptr;
} // namespace

ai::Blackboard& BehaviorTreeComponent::Board() {
    if (!blackboard) blackboard = std::make_shared<ai::Blackboard>();
    return *blackboard;
}
void BehaviorTreeComponent::Start() { commands.push_back({BehaviorTreeCommand::Kind::Start}); }
void BehaviorTreeComponent::Stop() { commands.push_back({BehaviorTreeCommand::Kind::Stop}); }
void BehaviorTreeComponent::Restart() { commands.push_back({BehaviorTreeCommand::Kind::Restart}); }
bool BehaviorTreeComponent::SetBool(const std::string& k, bool v) { return Board().Set(k, v); }
bool BehaviorTreeComponent::SetInt(const std::string& k, i32 v) { return Board().Set(k, v); }
bool BehaviorTreeComponent::SetFloat(const std::string& k, f32 v) { return Board().Set(k, v); }
bool BehaviorTreeComponent::SetString(const std::string& k, const std::string& v) { return Board().Set(k, v); }
bool BehaviorTreeComponent::SetVector(const std::string& k, const Vec3& v) { return Board().Set(k, v); }
bool BehaviorTreeComponent::SetEntity(const std::string& k, const Entity& v) { return Board().Set(k, v.IsNull() ? ai::BlackboardValue{} : ai::BlackboardValue{v}); }
bool BehaviorTreeComponent::ClearValue(const std::string& k) { return Board().Clear(k); }
bool BehaviorTreeComponent::IsValueSet(const std::string& k) const { return blackboard && blackboard->IsSet(k); }
bool BehaviorTreeComponent::GetBool(const std::string& k) const { return blackboard ? blackboard->GetAs<bool>(k) : false; }
i32 BehaviorTreeComponent::GetInt(const std::string& k) const { return blackboard ? blackboard->GetAs<i32>(k) : 0; }
f32 BehaviorTreeComponent::GetFloat(const std::string& k) const { return blackboard ? blackboard->GetAs<f32>(k) : 0.0f; }
std::string BehaviorTreeComponent::GetString(const std::string& k) const { return blackboard ? blackboard->GetAs<std::string>(k) : std::string(); }
Vec3 BehaviorTreeComponent::GetVector(const std::string& k) const { return blackboard ? blackboard->GetAs<Vec3>(k) : Vec3(); }
Entity BehaviorTreeComponent::GetEntity(const std::string& k) const { return blackboard ? blackboard->GetAs<Entity>(k, kNullEntity) : kNullEntity; }

bool BehaviorTrees::FinishTask(const Entity& entity, bool success) { return g_active != nullptr && g_active->FinishTask(entity, success); }

void RegisterBehaviorTreeComponents() {
    RegisterNavAgentComponents();
    (void)GetComponentId<BehaviorTreeComponent>();
}

} // namespace aether

namespace aether::ai {

BehaviorTreeWorld::BehaviorTreeWorld(World& world, AssetLookup assets) : world_(world), assets_(std::move(assets)) {
    RegisterBehaviorTreeComponents();
    MakeActive();
}

BehaviorTreeWorld::~BehaviorTreeWorld() {
    if (g_active == this) g_active = nullptr;
}

BehaviorTreeWorld* BehaviorTreeWorld::Active() { return g_active; }
void BehaviorTreeWorld::MakeActive() { g_active = this; }

void BehaviorTreeWorld::Report(const std::string& key, const std::string& message) {
    if (reported_[key]) return;
    reported_[key] = true;
    problems_.push_back(message);
}

void BehaviorTreeWorld::StopLive(Live& live, BehaviorTreeComponent& c) {
    if (live.instance) {
        BtContext ctx{&world_, live.entity, &c.Board(), &hooks};
        live.instance->Abort(ctx);
    }
    live.instance.reset();
    c.running = false;
    c.active.clear();
}

bool BehaviorTreeWorld::FinishTask(Entity e, bool success, const std::string& event) {
    auto it = live_.find(e.index);
    if (it == live_.end() || it->second.entity != e || !it->second.instance) return false;
    return it->second.instance->FinishLatent(success ? BtStatus::Success : BtStatus::Failure, event);
}

const BehaviorTreeInstance* BehaviorTreeWorld::InstanceOf(Entity e) const {
    auto it = live_.find(e.index);
    return it == live_.end() || it->second.entity != e ? nullptr : it->second.instance.get();
}

void BehaviorTreeWorld::Update(f32 dt) {
    // Entities that went: drop their trees (their world is gone, so no aborts reach them).
    for (auto it = live_.begin(); it != live_.end();) {
        if (!world_.IsAlive(it->second.entity) || world_.GetComponent<BehaviorTreeComponent>(it->second.entity) == nullptr) it = live_.erase(it);
        else ++it;
    }
    std::vector<Entity> entities;
    world_.ForEachArchetype([&](Archetype& archetype) {
        if (!archetype.Mask().test(GetComponentId<BehaviorTreeComponent>())) return;
        for (usize c = 0; c < archetype.ChunkCount(); ++c) {
            Entity* e = archetype.EntityArray(c);
            entities.insert(entities.end(), e, e + archetype.ChunkEntityCount(c));
        }
    });
    for (Entity e : entities) {
        BehaviorTreeComponent& c = *world_.GetComponent<BehaviorTreeComponent>(e);
        auto [it, added] = live_.try_emplace(e.index);
        Live& live = it->second;
        if (added || live.entity != e) {
            live = Live{};
            live.entity = e;
            if (c.auto_start) c.commands.insert(c.commands.begin(), {BehaviorTreeCommand::Kind::Start});
        }
        if (live.tree != c.tree) { // a new tree (or the first): what's running stops
            StopLive(live, c);
            live.tree = c.tree;
            live.asset = assets_ ? assets_(c.tree) : nullptr;
            if (live.asset == nullptr) Report("tree:" + c.tree, "behavior tree '" + c.tree + "' couldn't be found");
            else c.Board().Adopt(live.asset->blackboard);
        }
        for (const BehaviorTreeCommand& cmd : c.commands) {
            if (cmd.kind == BehaviorTreeCommand::Kind::Stop || cmd.kind == BehaviorTreeCommand::Kind::Restart) StopLive(live, c);
            if ((cmd.kind == BehaviorTreeCommand::Kind::Start || cmd.kind == BehaviorTreeCommand::Kind::Restart) && !live.instance && live.asset != nullptr) {
                live.instance = std::make_unique<BehaviorTreeInstance>(*live.asset, static_cast<u64>(e.index) + 1);
                live.accumulated = c.tick_interval; // tick at once
                c.running = true;
            }
        }
        c.commands.clear();
        if (!live.instance) continue;
        live.accumulated += dt;
        if (live.accumulated + 1e-6f < c.tick_interval) continue;
        const f32 step = c.tick_interval > 0.0f ? live.accumulated : dt;
        live.accumulated = 0.0f;
        BtContext ctx{&world_, e, &c.Board(), &hooks};
        live.instance->Tick(ctx, step);
        // The world may have changed under the tick (a task spawning): look the component up again.
        if (BehaviorTreeComponent* now = world_.GetComponent<BehaviorTreeComponent>(e)) now->active = live.instance->ActiveLabel();
    }
}

} // namespace aether::ai
