# Spawner

The spawner kit makes prefabs appear: wolves around a den, crates in a depot, a wave of enemies. You put a **Spawner** component on an entity,
name a prefab, and the kit does the rest. It is an optional part of the build (`AETHER_KIT_SPAWNER`, on by default) and needs no other kit.
What the spawns *do* is up to the prefab and your scripts.

## The component

| Field | Meaning |
| --- | --- |
| `prefab` | the prefab to spawn, by path in the package (`Prefabs/Wolf.aprefab`) |
| `enabled` | switched off, the spawner does nothing on its own |
| `start_delay` | seconds before the first spawn |
| `interval` | seconds between spawns (at least 0.01). It counts only while there is room, so after a place opens up the next spawn comes an `interval` later |
| `burst` | how many appear at a time (1 to 100) |
| `max_alive` | at most this many of its spawns alive at once (0: no limit; at most 1000) |
| `total` | stops after this many in all (0: no limit; at most 1,000,000) |
| `radius` | spawns land anywhere within this distance of the spawner, in the horizontal plane, spread evenly over the disc (0: on it) |
| `timer`, `spawned` | the spawner's state, saved with the entity: seconds to the next spawn (-1 before it starts) and how many it has made |

A spawn is placed at the spawner's position plus a scatter within `radius`, with the spawner's rotation. The scatter is a fixed function of the
spawner and how many it has made, so a run is the same every time. The count of what is alive drops a spawn as soon as it is destroyed.

A spawner that names a prefab the package does not have is a warning in the player that names the path, once; it keeps trying at its interval,
so putting the prefab in the package fixes it.

## From a script

The spawner's own script hears `OnSpawned` for each one, with the new entity:

```lua
function Den:OnSpawned(wolf)
    wolf:Get("Tags")   -- set it up: give it a target, a name, a colour
end

function Den:OnUpdate(dt)
    if Input.IsTriggered("Alarm") then
        Spawner.SpawnNow(self.entity, 4)   -- a wave now, whatever the timer says
    end
end
```

| Call | Result |
| --- | --- |
| `Spawner.SpawnNow(spawner [, count])` | how many appeared: up to `count` (1 to 100; default 1) right now, never past `max_alive` or `total`. It works on a switched-off spawner too; it gives 0 for an entity that is no spawner, has no prefab, or is full |
| `Spawner.AliveCount(spawner)` | how many of its spawns are alive now |
| `Spawner.TotalSpawned(spawner)` | how many it has made in all |

The component's fields are ordinary reflected fields, so a script changes them like any component's (`entity:Get("Spawner")`): switch `enabled`,
change the `interval` as a fight goes on, or set `total` to cap a wave.
