#pragma once

#include "aether/kit/kit.h"

#include <memory>

namespace aether::destruction {

// This kit's description for the player's kit registry: its name (no kit dependencies; it needs the physics module, which the build checks), the components
// it registers and the script table it installs. It makes no runtime and adds no scheduler stage: the DestructionSystem points at the physics scene, which
// does not exist yet when kit runtimes are made, so the host owns it beside that scene, feeds it the physics events and updates it each frame.
std::unique_ptr<kit::IKit> MakeDestructionKit();

} // namespace aether::destruction
