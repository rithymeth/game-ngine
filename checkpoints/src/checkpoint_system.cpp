#include "aether/checkpoints/checkpoint_system.h"

#include "aether/scene/components.h"
#include "aether/scene/gameplay.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace aether::chk {

namespace {

CheckpointSystem* g_active = nullptr;
constexpr const char* kLast = "checkpoint.last";
constexpr const char* kScene = "checkpoint.scene";

std::string ReachedKey(const std::string& name) { return "checkpoint." + name; }

} // namespace

const char* ResultName(Result result) {
    switch (result) {
        case Result::Ok: return "ok";
        case Result::DeadEntity: return "dead_entity";
        case Result::NoCheckpoint: return "no_checkpoint";
        case Result::NotInScene: return "not_in_scene";
        case Result::NoTransform: return "no_transform";
    }
    return "unknown";
}

CheckpointSystem::CheckpointSystem(World& world, save::SaveBag& bag, std::function<std::string()> current_scene)
    : world_(world), bag_(bag), scene_(std::move(current_scene)) {
    RegisterCheckpointComponents();
    MakeActive();
}
CheckpointSystem::~CheckpointSystem() {
    if (g_active == this) g_active = nullptr;
}
CheckpointSystem* CheckpointSystem::Active() { return g_active; }
void CheckpointSystem::MakeActive() { g_active = this; }

std::vector<Entity> CheckpointSystem::Checkpoints() const {
    std::vector<Entity> out;
    const ComponentId id = GetComponentId<Checkpoint>();
    world_.ForEachArchetype([&](const Archetype& a) {
        if (!a.Mask().test(id)) return;
        for (usize c = 0; c < a.ChunkCount(); ++c) {
            const Entity* chunk = a.EntityArray(c);
            out.insert(out.end(), chunk, chunk + a.ChunkEntityCount(c));
        }
    });
    std::sort(out.begin(), out.end(), [](Entity x, Entity y) { return x.index != y.index ? x.index < y.index : x.generation < y.generation; });
    return out;
}

std::string CheckpointSystem::Last() const { return bag_.GetString(kLast); }
std::string CheckpointSystem::LastScene() const { return bag_.GetString(kScene); }
bool CheckpointSystem::IsReached(const std::string& name) const { return !name.empty() && bag_.GetInt(ReachedKey(name)) == 1; }

Entity CheckpointSystem::Find(const std::string& name) const {
    if (name.empty()) return Entity{};
    for (const Entity e : Checkpoints()) {
        const Checkpoint* c = world_.GetComponent<Checkpoint>(e);
        if (c != nullptr && c->name == name) return e;
    }
    return Entity{};
}

void CheckpointSystem::Reach(Entity checkpoint, const Checkpoint& c, Entity activator) {
    CheckpointEvent event{checkpoint, activator, c.name, !IsReached(c.name)};
    bag_.SetString(kLast, c.name);
    bag_.SetString(kScene, scene_ ? scene_() : std::string());
    bag_.SetInt(ReachedKey(c.name), 1);
    events_.push_back(std::move(event));
}

bool CheckpointSystem::Activate(const std::string& name) {
    const Entity e = Find(name);
    if (e.IsNull()) return false;
    const Checkpoint* c = world_.GetComponent<Checkpoint>(e);
    if (c == nullptr) return false;
    Reach(e, *c, Entity{});
    return true;
}

void CheckpointSystem::Update() {
    std::unordered_map<std::string, std::vector<Entity>> by_tag; // each tag looked up once per update
    for (const Entity e : Checkpoints()) {
        if (!world_.IsAlive(e)) continue;
        const Checkpoint* c = world_.GetComponent<Checkpoint>(e);
        const Transform* at = world_.GetComponent<Transform>(e);
        if (c == nullptr || at == nullptr || !c->enabled || c->name.empty() || c->name == Last()) continue;
        auto found = by_tag.find(c->activator_tag);
        if (found == by_tag.end()) found = by_tag.emplace(c->activator_tag, FindEntitiesWithTag(world_, c->activator_tag)).first;
        const f32 radius = std::isfinite(c->radius) ? std::clamp(c->radius, Checkpoint::kMinRadius, Checkpoint::kMaxRadius) : Checkpoint::kMinRadius;
        for (const Entity who : found->second) {
            const Transform* t = world_.IsAlive(who) ? world_.GetComponent<Transform>(who) : nullptr;
            if (t == nullptr || who == e) continue;
            if ((t->position - at->position).Length() <= radius) {
                Reach(e, *c, who);
                break;
            }
        }
    }
}

Result CheckpointSystem::Respawn(Entity who) {
    if (who.IsNull() || !world_.IsAlive(who)) return Result::DeadEntity;
    const std::string last = Last();
    if (last.empty()) return Result::NoCheckpoint;
    const Entity marker = Find(last);
    if (marker.IsNull()) return Result::NotInScene;
    Transform* t = world_.GetComponent<Transform>(who);
    const Transform* at = world_.GetComponent<Transform>(marker);
    const Checkpoint* c = world_.GetComponent<Checkpoint>(marker);
    if (t == nullptr || at == nullptr || c == nullptr) return Result::NoTransform;
    t->position = at->position + c->respawn_offset;
    t->rotation = at->rotation;
    return Result::Ok;
}

} // namespace aether::chk
