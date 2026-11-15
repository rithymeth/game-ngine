#include "aether/spawner/spawner_system.h"

#include <algorithm>
#include <cmath>

namespace aether::spawn {

namespace {

SpawnerSystem* g_active = nullptr;
constexpr f32 kPi = 3.14159265358979323846f;

u32 Mix(u32 x) { // a 32-bit finalizer: nearby inputs give unrelated outputs
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

i32 Clamp(i32 value, i32 lo, i32 hi) { return std::clamp(value, lo, hi); }

} // namespace

Vec3 ScatterOffset(u32 seed, f32 radius) {
    if (!(radius > 0.0f) || !std::isfinite(radius)) return Vec3(0, 0, 0);
    const f32 u = static_cast<f32>(Mix(seed) & 0xffffffu) / 16777216.0f;
    const f32 v = static_cast<f32>(Mix(seed ^ 0x9e3779b9u) & 0xffffffu) / 16777216.0f;
    const f32 angle = u * 2.0f * kPi;
    const f32 distance = std::sqrt(v) * radius; // sqrt: even over the area, not bunched at the centre
    return Vec3(std::cos(angle) * distance, 0.0f, std::sin(angle) * distance);
}

SpawnerSystem::SpawnerSystem(World& world, SpawnFunction spawn) : world_(world), spawn_(std::move(spawn)) {
    RegisterSpawnerComponents();
    MakeActive();
}
SpawnerSystem::~SpawnerSystem() {
    if (g_active == this) g_active = nullptr;
}
SpawnerSystem* SpawnerSystem::Active() { return g_active; }
void SpawnerSystem::MakeActive() { g_active = this; }

std::vector<Entity> SpawnerSystem::Spawners() const {
    std::vector<Entity> out;
    const ComponentId id = GetComponentId<Spawner>();
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

void SpawnerSystem::Prune(Entity spawner) {
    const auto it = alive_.find(spawner);
    if (it == alive_.end()) return;
    auto& list = it->second;
    list.erase(std::remove_if(list.begin(), list.end(), [&](Entity e) { return !world_.IsAlive(e); }), list.end());
    if (list.empty()) alive_.erase(it);
}

i32 SpawnerSystem::AliveCount(Entity spawner) {
    Prune(spawner);
    const auto it = alive_.find(spawner);
    return it == alive_.end() ? 0 : static_cast<i32>(it->second.size());
}

i32 SpawnerSystem::TotalSpawned(Entity spawner) const {
    if (spawner.IsNull() || !world_.IsAlive(spawner)) return 0;
    const Spawner* s = world_.GetComponent<Spawner>(spawner);
    return s ? std::max(s->spawned, 0) : 0;
}

// How many more this spawner may make right now: what max_alive and total leave, never negative.
i32 SpawnerSystem::Room(Entity spawner, const Spawner& s) {
    i32 room = Spawner::kMaxBurst;
    const i32 max_alive = Clamp(s.max_alive, 0, Spawner::kMaxAlive);
    if (max_alive > 0) room = std::min(room, max_alive - AliveCount(spawner));
    const i32 total = Clamp(s.total, 0, Spawner::kMaxTotal);
    if (total > 0) room = std::min(room, total - std::max(s.spawned, 0));
    return std::max(room, 0);
}

i32 SpawnerSystem::SpawnUpTo(Entity spawner, i32 wanted) {
    if (!spawn_ || wanted <= 0) return 0;
    i32 made = 0;
    for (i32 i = 0; i < wanted; ++i) {
        // Fetch the component and the transform afresh: a spawn creates entities, which can move them.
        Spawner* s = world_.GetComponent<Spawner>(spawner);
        if (s == nullptr || s->prefab.empty()) break;
        Transform place;
        if (const Transform* t = world_.GetComponent<Transform>(spawner)) place = *t;
        const u32 seed = Mix(spawner.index * 2654435761u + static_cast<u32>(std::max(s->spawned, 0)));
        place.position = place.position + ScatterOffset(seed, s->radius);
        const std::string prefab = s->prefab;
        const Entity created = spawn_(prefab, place);
        s = world_.GetComponent<Spawner>(spawner);
        if (created.IsNull() || !world_.IsAlive(created)) break; // the host could not (a prefab that is not there): do not try the rest this time
        if (s != nullptr) ++s->spawned;
        alive_[spawner].push_back(created);
        events_.push_back({spawner, created});
        ++made;
    }
    return made;
}

i32 SpawnerSystem::SpawnNow(Entity spawner, i32 count) {
    if (spawner.IsNull() || !world_.IsAlive(spawner) || count < 1 || count > Spawner::kMaxBurst) return 0;
    const Spawner* s = world_.GetComponent<Spawner>(spawner);
    if (s == nullptr || s->prefab.empty()) return 0;
    return SpawnUpTo(spawner, std::min(count, Room(spawner, *s)));
}

void SpawnerSystem::Update(f32 dt) {
    if (!(dt > 0.0f) || !std::isfinite(dt)) return;
    for (const Entity e : Spawners()) {
        if (!world_.IsAlive(e)) continue;
        Spawner* s = world_.GetComponent<Spawner>(e);
        if (s == nullptr || !s->enabled || s->prefab.empty()) continue;
        if (s->timer < 0.0f) s->timer = std::max(s->start_delay, 0.0f); // the first frame: start counting
        if (Room(e, *s) <= 0) continue;                                 // full, or done: the timer waits for room
        s->timer -= dt;
        if (s->timer > 0.0f) continue;
        const i32 burst = Clamp(s->burst, 1, Spawner::kMaxBurst);
        const f32 interval = std::isfinite(s->interval) ? std::max(s->interval, Spawner::kMinInterval) : Spawner::kMinInterval;
        const i32 wanted = std::min(burst, Room(e, *s));
        SpawnUpTo(e, wanted);
        s = world_.GetComponent<Spawner>(e);
        if (s != nullptr) s->timer = interval;
    }
}

} // namespace aether::spawn
