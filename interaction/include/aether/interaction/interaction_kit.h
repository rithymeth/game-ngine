#pragma once

#include "aether/kit/kit.h"
#include "aether/interaction/interaction_system.h"
#include "aether/scene/scheduler.h"

#include <memory>

namespace aether::interact {

// Builds the interaction kit's update system. Event delivery stays with the host so
// the kit does not depend on scripting or Blueprint modules.
SystemDesc MakeInteractionSystem(InteractionSystem* system, std::function<void(const InteractionEvent&)> on_event);

// This kit's description for the player's kit registry (Phase 37 step 4): its name, what it needs, the
// components it registers and the scheduler stage it adds.
std::unique_ptr<kit::IKit> MakeInteractionKit();

} // namespace aether::interact
