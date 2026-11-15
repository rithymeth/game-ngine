#pragma once

#include "aether/core/base.h"
#include "aether/reflection/reflection.h"

#include <string>

namespace aether::spawn {

// Makes prefabs appear around an entity: every `interval` seconds, `burst` of them, as long as fewer than `max_alive` of its spawns are alive.
// A component, saved with the entity. What the spawns do is up to the prefab; the spawner's script hears OnSpawned (the new entity).
struct Spawner {
    static constexpr i32 kMaxBurst = 100;
    static constexpr i32 kMaxAlive = 1000;
    static constexpr i32 kMaxTotal = 1000000;
    static constexpr f32 kMinInterval = 0.01f;

    bool enabled = true;
    std::string prefab;       // a prefab in the package, by path ("Prefabs/Wolf.aprefab")
    f32 start_delay = 0.0f;   // seconds before the first spawn
    f32 interval = 5.0f;      // seconds between spawns; it counts only while there is room, so a spawn is `interval` after a place opens up
    i32 burst = 1;            // spawned at a time (1 to kMaxBurst)
    i32 max_alive = 5;        // at most this many of its spawns alive at once (0: no limit; at most kMaxAlive)
    i32 total = 0;            // stops after this many in all (0: no limit; at most kMaxTotal)
    f32 radius = 0.0f;        // spawns land anywhere within this distance of the spawner, in the horizontal plane (0: on it)
    // Runtime state (saved, so a spawner does not start over when the scene does).
    f32 timer = -1.0f;        // seconds to the next spawn; -1 until the spawner has started
    i32 spawned = 0;          // how many it has made in all
};

void RegisterSpawnerComponents();

} // namespace aether::spawn

AETHER_REFLECT(aether::spawn::Spawner, 1,
    AETHER_FIELD(enabled, Field_EditAnywhere),
    AETHER_FIELD(prefab, Field_EditAnywhere, {.tooltip = "The prefab to spawn, by path in the package", .asset_extensions = ".aprefab"}),
    AETHER_FIELD(start_delay, Field_EditAnywhere, {.tooltip = "Seconds before the first spawn", .range_min = 0.0, .range_max = 100000.0, .units = "s"}),
    AETHER_FIELD(interval, Field_EditAnywhere, {.tooltip = "Seconds between spawns; it counts only while there is room", .range_min = 0.01, .range_max = 100000.0, .units = "s"}),
    AETHER_FIELD(burst, Field_EditAnywhere, {.tooltip = "How many are spawned at a time", .range_min = 1.0, .range_max = 100.0}),
    AETHER_FIELD(max_alive, Field_EditAnywhere, {.tooltip = "At most this many of its spawns alive at once; 0 for no limit", .range_min = 0.0, .range_max = 1000.0}),
    AETHER_FIELD(total, Field_EditAnywhere, {.tooltip = "Stops after this many in all; 0 for no limit", .range_min = 0.0, .range_max = 1000000.0}),
    AETHER_FIELD(radius, Field_EditAnywhere, {.tooltip = "Spawns land within this distance, in the horizontal plane", .range_min = 0.0, .range_max = 10000.0, .units = "m"}),
    AETHER_FIELD(timer, Field_EditAnywhere, {.tooltip = "Seconds to the next spawn (-1: not started)"}),
    AETHER_FIELD(spawned, Field_EditAnywhere, {.tooltip = "How many it has made in all"}))
