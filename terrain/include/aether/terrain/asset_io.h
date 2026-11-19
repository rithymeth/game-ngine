#pragma once

#include "aether/terrain/foliage.h"
#include "aether/terrain/splat.h"
#include "aether/terrain/terrain.h"

#include <nlohmann/json.hpp>

#include <string>

namespace aether {
namespace terrain {

// The terrain asset (.aterrain) and the foliage asset (.afoliage) as JSON: what the editor's Terrain and Foliage tools save and open
// (docs/design/format_versions.txt). A terrain is its heightmap, layers, settings and painted splatmap; a foliage asset is the layer
// (types, settings, instances) and the density map the paint tool thinned it with. Both carry a format name and a version, and a file
// of another version is refused rather than guessed at.
nlohmann::json TerrainToJson(const TerrainData& data, const TerrainSettings& settings, const SplatmapData& splatmap);
// Fills all three from a terrain's JSON. False, with the reason in `error`, leaves them unmodified.
bool TerrainFromJson(const nlohmann::json& json, TerrainData& data, TerrainSettings& settings, SplatmapData& splatmap,
                     std::string* error = nullptr);

nlohmann::json FoliageToJson(const FoliageLayer& layer, const DensityMap& density);
// False, with the reason in `error`, leaves both unmodified.
bool FoliageFromJson(const nlohmann::json& json, FoliageLayer& layer, DensityMap& density, std::string* error = nullptr);

} // namespace terrain
} // namespace aether
