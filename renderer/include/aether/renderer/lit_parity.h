#pragma once

#include "aether/gfx/parity.h"

#include <vector>

namespace aether::mat {

// Lit-material parity scenes (Phase 39.2 and 39.3/39.4): each draws a full-screen sweep of one lighting term from
// AdvancedPbrHlsl() or ShadingModelsHlsl(), with the light angle across the target and a material parameter down it, and
// carries a C++ port of the same HLSL as its CPU reference. A backend that differs from the port (or from the other
// backends) has compiled or evaluated the term differently. Output is c / (1 + c) so every term fits a UNORM target.
//
// Scenes: lit_anisotropy, lit_sheen, lit_clear_coat, lit_hair, lit_cloth, lit_subsurface.
const std::vector<gfx::parity::Scene>& LitParityScenes();

} // namespace aether::mat
