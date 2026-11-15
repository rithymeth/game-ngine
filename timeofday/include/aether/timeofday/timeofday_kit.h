#pragma once

#include "aether/kit/kit.h"
#include "aether/scene/scheduler.h"
#include "aether/timeofday/world_clock.h"

#include <memory>

namespace aether::tod {

// The scheduler system that advances the clock with each frame's dt ("Player.TimeOfDay").
SystemDesc MakeTimeOfDaySystem(WorldClock* clock);

// This kit's description for the player's kit registry: its name (no dependencies), the script table it installs and the stage it adds. The
// clock itself belongs to the game, so it outlives a scene.
std::unique_ptr<kit::IKit> MakeTimeOfDayKit();

} // namespace aether::tod
