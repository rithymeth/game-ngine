#pragma once

#include "aether/kit/kit.h"

#include <memory>

namespace aether::gas {

// The gameplay module as a kit (Phase 37 step 4): tags, attributes, effects and abilities. The other gameplay
// kits depend on it. Its scheduler stages (attributes, effects, abilities) are still added by the player.
std::unique_ptr<kit::IKit> MakeGameplayKit();

} // namespace aether::gas
