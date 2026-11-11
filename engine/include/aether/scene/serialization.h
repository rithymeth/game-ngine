#pragma once

#include "aether/ecs/world.h"
#include "aether/math/vec.h"

#include <span>
#include <string>
#include <vector>

namespace aether {

struct SceneLoadOptions {
    // Editors should reject a scene they cannot fully understand so a later
    // save cannot silently discard components owned by unavailable plugins.
    bool reject_unknown_components = false;
    // When given, filled with the name of every component the file has that this build does not know (or cannot read as text), each once, whether the load skips them or, with
    // reject_unknown_components, fails because of them: so a tool can say which components are missing instead of "couldn't load".
    std::vector<std::string>* unknown_components = nullptr;
    // Phase 42.1: the scene's absolute origin. Loaded root Transforms are shifted
    // by -origin, so a scene authored far from (0,0,0) loads into small
    // origin-relative coordinates. Zero (the default) uses the origin recorded in
    // a JSON scene, if it has one.
    Vec3d origin;
};

struct SceneSaveOptions {
    // Phase 42.1: the origin a JSON scene records. With it set, Transform
    // positions are written as absolute (local + origin) and the origin is stored
    // at the root, so a loader gets the small coordinates back. Zero writes
    // positions as they are and records nothing.
    Vec3d origin;
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
// Replaces the scene atomically and preserves the previous version at
// "<path>.bak" when one exists, so an interrupted or damaged edit is recoverable.
[[nodiscard]] bool SaveScene(const World& world, const std::string& path);

// Loads entities from a file written by SaveScene into `world` (which is not
// cleared first — loading is additive). Returns false if the file can't be
// read or isn't a recognized/supported-version Aether scene file. The full
// binary record stream is validated before entities are appended, so truncated
// files and strict unknown-component rejection do not partially load a scene.
[[nodiscard]] bool LoadScene(World& world, const std::string& path);
// Phase 42.1: `used_origin` (optional) receives the origin the loader shifted by,
// which is the caller's for the binary format (it records no origin of its own).
[[nodiscard]] bool LoadScene(World& world, const std::string& path, const SceneLoadOptions& options,
                             Vec3d* used_origin = nullptr);

// The same binary format, in memory: what Play-in-Editor uses to snapshot the
// edited world and restore it on Stop. `source_name` only labels log output.
std::vector<u8> SaveSceneToMemory(const World& world);
[[nodiscard]] bool LoadSceneFromMemory(World& world, std::span<const u8> bytes, const std::string& source_name = "<memory>");
[[nodiscard]] bool LoadSceneFromMemory(World& world, std::span<const u8> bytes, const std::string& source_name,
                         const SceneLoadOptions& options, Vec3d* used_origin = nullptr);

// JSON scene files: the human-readable, diff-friendly form (see
// a retired planning doc). Only reflected components can be written as
// JSON; any other component is left out with a warning (and so is an entity
// left with no components at all). Loading tolerates unknown components by
// default and unknown fields in all modes. JSON entities are collected before
// appending to the destination world, so parse failures and strict unknown-
// component rejection do not partially load a scene.
// Writes JSON scenes atomically and keeps the previous version at "<path>.bak".
[[nodiscard]] bool SaveSceneJson(const World& world, const std::string& path);
// Phase 42.1: records an origin and writes absolute positions (see SceneSaveOptions).
[[nodiscard]] bool SaveSceneJson(const World& world, const std::string& path, const SceneSaveOptions& options);
[[nodiscard]] bool LoadSceneJson(World& world, const std::string& path);
// Phase 42.1: `used_origin` (optional) receives the origin the loader shifted by —
// the caller's, else the one the file recorded — so a game can keep the world
// local and remember where it is.
[[nodiscard]] bool LoadSceneJson(World& world, const std::string& path, const SceneLoadOptions& options,
                                 Vec3d* used_origin = nullptr);
// The same from a JSON scene in memory (e.g. read from an .apak); `source_name` is for messages.
[[nodiscard]] bool LoadSceneJsonFromMemory(World& world, std::span<const u8> bytes, const std::string& source_name = "<memory>");
[[nodiscard]] bool LoadSceneJsonFromMemory(World& world, std::span<const u8> bytes, const std::string& source_name,
                             const SceneLoadOptions& options, Vec3d* used_origin = nullptr);

} // namespace aether
