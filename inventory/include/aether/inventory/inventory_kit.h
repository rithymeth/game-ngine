#pragma once

#include "aether/kit/kit.h"
#include "aether/inventory/inventory_system.h"
#include "aether/scene/scheduler.h"

#include <memory>

namespace aether::inv {

// Builds the inventory update system. Event delivery stays with the host so this
// kit does not depend on scripting, Blueprints, or other optional kits.
SystemDesc MakeInventorySystem(InventorySystem* system, std::function<void(const ItemEvent&)> on_event);

// This kit's description for the player's kit registry (Phase 37 step 4): its name, what it needs, the
// components it registers and the scheduler stage it adds.
std::unique_ptr<kit::IKit> MakeInventoryKit();

} // namespace aether::inv
