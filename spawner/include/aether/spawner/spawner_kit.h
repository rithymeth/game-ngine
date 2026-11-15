#pragma once

#include "aether/kit/kit.h"
#include "aether/scene/scheduler.h"
#include "aether/spawner/spawner_system.h"

#include <functional>
#include <memory>

namespace aether::spawn {

// The spawner kit's update system ("Player.Spawner"). Event delivery stays with the host so the kit does not depend on scripting or Blueprints.
SystemDesc MakeSpawnerSystem(SpawnerSystem* system, std::function<void(const SpawnEvent&)> on_event);

// This kit's description for the player's kit registry: its name (no dependencies), the component it registers, the stage it adds and the system it
// creates, which makes prefabs through the host's "spawner.spawn" service (a SpawnFunction).
std::unique_ptr<kit::IKit> MakeSpawnerKit();

} // namespace aether::spawn
