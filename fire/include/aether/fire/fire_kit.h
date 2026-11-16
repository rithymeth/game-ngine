#pragma once

#include "aether/scene/scheduler.h"
#include "aether/fire/fire_system.h"
#include "aether/kit/kit.h"

#include <memory>

namespace aether::fire {

// This kit's description for the player's kit registry: its name, the component it registers, the script table it installs, its frame stage ("Player.Fire", with the
// frame's Update, before the particles are advanced and the scripts run) and the scene-scoped runtime that holds the FireSystem (found as "FireSystem").
std::unique_ptr<kit::IKit> MakeFireKit();

// The scheduler system that advances a FireSystem each frame (the player adds it as the kit's stage).
SystemDesc MakeFireSystem(FireSystem* system);

} // namespace aether::fire
