#pragma once

#include "aether/script/luau_host.h"

// Luau's `Spawner` table: the spawner kit's Spawner component, acted on by the active SpawnerSystem. Present only when the kit is built. The component's
// own fields (enabled, prefab, interval, burst, max_alive, total, radius) are read and written like any component's: entity:Get("Spawner").
//
//   Spawner.SpawnNow (spawner [, count])  -> number spawned: up to count (1 to 100; default 1) right now, whatever the timer says, but never past
//                                            max_alive or total; 0 for an entity that is no spawner, has no prefab or is full
//   Spawner.AliveCount (spawner)          -> number of its spawns alive now
//   Spawner.TotalSpawned (spawner)        -> number it has made in all (0 for an entity that is no spawner)
//
// The spawner's own script hears OnSpawned (the new entity) for each spawn. A call with a missing or wrong-typed argument raises a script error; without an
// active system the functions return 0.

namespace aether::script {

void InstallSpawnerApi(LuauHost& host); // does nothing when the kit isn't built

} // namespace aether::script
