#pragma once

#include "aether/kit/kit.h"
#include "aether/scene/scheduler.h"
#include "aether/weather/weather_system.h"

#include <memory>

namespace aether::weather {

// The scheduler system that advances the weather with each frame's dt ("Player.Weather").
SystemDesc MakeWeatherSystem(WeatherSystem* weather);

// This kit's description for the player's kit registry: its name (no dependencies), the script table it installs and the stage it adds. The weather itself
// belongs to the game, so it outlives a scene.
std::unique_ptr<kit::IKit> MakeWeatherKit();

} // namespace aether::weather
