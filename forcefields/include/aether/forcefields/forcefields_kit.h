#pragma once

#include "aether/kit/kit.h"

#include <memory>

namespace aether::ff {

// This kit's description for the player's kit registry: its name (no kit dependencies; it needs the physics module, which the build checks), the
// component it registers and the script table it installs. It makes no runtime and adds no scheduler stage: the ForceFieldSystem points at the physics scene,
// which does not exist yet when kit runtimes are made, so the host owns it beside that scene and applies the fields inside its physics step, just before the
// bodies move.
std::unique_ptr<kit::IKit> MakeForceFieldsKit();

} // namespace aether::ff
