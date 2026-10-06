#pragma once

#include "aether/kit/kit.h"

#include <memory>

namespace aether::quest {

// This kit's description for the player's kit registry (Phase 37 step 4): its name, what it needs, the
// components it registers and the scheduler stage it adds.
std::unique_ptr<kit::IKit> MakeQuestsKit();

} // namespace aether::quest
