#pragma once

#include "aether/kit/kit.h"

#include <memory>

namespace aether::vehicles {

// This kit's description for the player's kit registry: its name (no kit dependencies; it needs the physics module, which the build checks), the components
// it registers and the script table it installs. It makes no runtime and adds no scheduler stage: the VehicleSystem points at the physics scene, which does
// not exist yet when kit runtimes are made, so the host owns it beside that scene and applies it inside its physics step.
std::unique_ptr<kit::IKit> MakeVehiclesKit();

} // namespace aether::vehicles
