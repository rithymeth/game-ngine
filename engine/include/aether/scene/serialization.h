#pragma once

#include "aether/ecs/world.h"

#include <string>

namespace aether {

// Binary scene files (.aesc). For each entity, every component is stored as
// {name, encoding, bytes}: identified by NAME rather than the process-local
// numeric ComponentId (which is only stable within the process that assigned
// it), so a scene file survives components being registered in a different
// order, or by a different binary. Reflected components are named by their
// declared name and stored field by field (see ComponentInfo), so a scene
// also survives fields being added or removed. Entities are written in
// creation (entity index) order, so saving the same world twice produces an
// identical file.
//
// A component unknown at load time is skipped with a warning, not a hard
// failure. Version 1 files (written before the component registry could
// exceed 64 types) still load, including their raw-byte components into types
// that have since become reflected.
bool SaveScene(const World& world, const std::string& path);

// Loads entities from a file written by SaveScene into `world` (which is not
// cleared first — loading is additive). Returns false if the file can't be
// read or isn't a recognized/supported-version Aether scene file.
bool LoadScene(World& world, const std::string& path);

// JSON scene files: the human-readable, diff-friendly form (see
// docs/ROADMAP_DETAILS.md §A.3). Only reflected components can be written as
// JSON; any other component is left out with a warning (and so is an entity
// left with no components at all). Loading tolerates unknown components and
// fields the same way the binary form does.
bool SaveSceneJson(const World& world, const std::string& path);
bool LoadSceneJson(World& world, const std::string& path);

} // namespace aether
