#pragma once

#include "aether/ecs/world.h"

#include <string>

namespace aether {

// Serializes every entity in `world` to a simple binary format: for each
// entity, its component mask followed by each component's data, identified
// by NAME rather than the process-local numeric ComponentId (which is only
// stable within the process that assigned it) — so a scene file survives
// components being registered in a different order, or by a different
// binary, the next time it's loaded. A component with no registered
// serializer (shouldn't happen — GetComponentId<T>() always installs a
// default raw-byte one) or unknown at load time is skipped with a warning,
// not a hard failure.
bool SaveScene(const World& world, const std::string& path);

// Loads entities from a file written by SaveScene into `world` (which is not
// cleared first — loading is additive). Returns false if the file can't be
// read or isn't a recognized/supported-version Aether scene file.
bool LoadScene(World& world, const std::string& path);

} // namespace aether
