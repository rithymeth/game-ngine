#pragma once

#include "aether/ecs/entity.h"
#include "aether/ecs/world.h"
#include "aether/scene/components.h"
#include "aether/spawner/spawner.h"

#include <functional>
#include <unordered_map>
#include <vector>

namespace aether::spawn {

// Makes a prefab appear at a place: the host's job (it knows the package's prefabs). Returns the new entity, or a null one if it could not.
using SpawnFunction = std::function<Entity(const std::string& prefab, const Transform& place)>;

// One thing the system did: `spawned` appeared because of `spawner`.
struct SpawnEvent {
    Entity spawner;
    Entity spawned;
};

// A point within `radius` of the origin in the horizontal plane (y is 0), chosen by `seed` alone: the same seed gives the same point, nearby seeds
// give unrelated ones, and the points are spread evenly over the disc. The system's scatter, exposed so a test can look at it.
Vec3 ScatterOffset(u32 seed, f32 radius);

// Runs the Spawner components of a world. Deterministic: it moves only by the dt it is given, and the scatter has no random state of its own.
// It remembers which entities each spawner made, so it knows how many are alive (a destroyed one is dropped from the count).
class SpawnerSystem {
public:
    SpawnerSystem(World& world, SpawnFunction spawn);
    ~SpawnerSystem();
    SpawnerSystem(const SpawnerSystem&) = delete;
    SpawnerSystem& operator=(const SpawnerSystem&) = delete;

    static SpawnerSystem* Active();
    void MakeActive();

    // Counts every enabled spawner's timer down (while it has room) and spawns when one reaches 0.
    void Update(f32 dt);
    // Spawns up to `count` now (1 to Spawner::kMaxBurst), whatever the timer says and whether or not the spawner is enabled, but never past max_alive
    // or total. Returns how many appeared (0 for an entity that is no spawner, has no prefab, or is full).
    i32 SpawnNow(Entity spawner, i32 count = 1);
    // How many of this spawner's spawns are alive now.
    i32 AliveCount(Entity spawner);
    // How many it has made in all (the component's `spawned`); 0 for an entity that is no spawner.
    i32 TotalSpawned(Entity spawner) const;

    const std::vector<SpawnEvent>& Events() const { return events_; }
    void ClearEvents() { events_.clear(); }

private:
    std::vector<Entity> Spawners() const;
    void Prune(Entity spawner);
    i32 Room(Entity spawner, const Spawner& s);
    i32 SpawnUpTo(Entity spawner, i32 wanted);

    World& world_;
    SpawnFunction spawn_;
    std::unordered_map<Entity, std::vector<Entity>> alive_;
    std::vector<SpawnEvent> events_;
};

} // namespace aether::spawn
