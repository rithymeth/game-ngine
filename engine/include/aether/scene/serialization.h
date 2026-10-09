#pragma once

#include "aether/ecs/world.h"

#include <span>
#include <string>
#include <vector>

namespace aether {

struct SceneLoadOptions {
    // Editors should reject a scene they cannot fully understand so a later
    // save cannot silently discard components owned by unavailable plugins.
    bool reject_unknown_components = false;
};

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
// Unknown components are skipped with a warning by default. Set
// SceneLoadOptions::reject_unknown_components to fail instead, as editors
// should do before allowing a scene to be saved. Version 1 files (written
// before the component registry could exceed 64 types) still load, including
// their raw-byte components into types that have since become reflected.
bool SaveScene(const World& world, const std::string& path);

// Loads entities from a file written by SaveScene into `world` (which is not
// cleared first — loading is additive). Returns false if the file can't be
// read or isn't a recognized/supported-version Aether scene file.
bool LoadScene(World& world, const std::string& path);
bool LoadScene(World& world, const std::string& path, const SceneLoadOptions& options);

// The same binary format, in memory: what Play-in-Editor uses to snapshot the
// edited world and restore it on Stop. `source_name` only labels log output.
std::vector<u8> SaveSceneToMemory(const World& world);
bool LoadSceneFromMemory(World& world, std::span<const u8> bytes, const std::string& source_name = "<memory>");
bool LoadSceneFromMemory(World& world, std::span<const u8> bytes, const std::string& source_name,
                         const SceneLoadOptions& options);

// JSON scene files: the human-readable, diff-friendly form (see
// docs/ROADMAP_DETAILS.md §A.3). Only reflected components can be written as
// JSON; any other component is left out with a warning (and so is an entity
// left with no components at all). Loading tolerates unknown components by
// default and unknown fields in all modes.
bool SaveSceneJson(const World& world, const std::string& path);
bool LoadSceneJson(World& world, const std::string& path);
bool LoadSceneJson(World& world, const std::string& path, const SceneLoadOptions& options);
// The same from a JSON scene in memory (e.g. read from an .apak); `source_name` is for messages.
bool LoadSceneJsonFromMemory(World& world, std::span<const u8> bytes, const std::string& source_name = "<memory>");
bool LoadSceneJsonFromMemory(World& world, std::span<const u8> bytes, const std::string& source_name,
                             const SceneLoadOptions& options);

} // namespace aether
