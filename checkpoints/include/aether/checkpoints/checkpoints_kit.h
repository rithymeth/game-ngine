#pragma once

#include "aether/checkpoints/checkpoint_system.h"
#include "aether/kit/kit.h"
#include "aether/scene/scheduler.h"

#include <functional>
#include <memory>

namespace aether::chk {

// The checkpoint kit's update system ("Player.Checkpoints"). Event delivery stays with the host so the kit does not depend on scripting or Blueprints.
SystemDesc MakeCheckpointsSystem(CheckpointSystem* system, std::function<void(const CheckpointEvent&)> on_event);

// This kit's description for the player's kit registry: its name (no dependencies), the component it registers, the stage it adds and the system it
// creates over the game's save bag (the host's "save.bag" service; "checkpoints.scene", a std::function<std::string()>, is optional).
std::unique_ptr<kit::IKit> MakeCheckpointsKit();

} // namespace aether::chk
