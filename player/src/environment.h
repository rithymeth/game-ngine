#pragma once

// Image-based lighting from the sky (render roadmap step 3). The environment images, their prefiltering and the cooked form moved to the
// engine (aether/assets/environment.h) so the cooker can build them; the player keeps its names for them.
#include "aether/assets/environment.h"

namespace aether::player {

using assets::DirectionToEquirect;
using assets::EnvironmentImage;
using assets::EquirectToDirection;
using assets::EvaluateIrradiance;
using assets::FloatToHalf;
using assets::HalfToFloat;
using assets::MakeEnvironment;
using assets::MakeEnvironmentFromSrgb;
using assets::MakeProceduralSky;
using assets::PrefilteredEnvironment;
using assets::PrefilterEnvironment;
using assets::PrefilterSettings;

} // namespace aether::player
