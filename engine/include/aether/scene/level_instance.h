#pragma once

// Phase 42.6: level instances -- placing one scene inside another.
//
// A level instance is the prefab rule applied to a whole scene: the scene becomes prefab data and is
// instantiated at the placement, so it follows later edits to the scene the way a prefab follows its
// asset, with per-instance overrides resolved by the same path.
//
// Turning a scene into prefab data needs a file system, so it is the caller's job (`SceneToPrefab`
// below is the shape the editor supplies). That keeps the part worth testing -- finding unexpanded
// instances, expanding each once, and reporting what failed -- in the engine with a fake loader.

#include "aether/reflection/reflect_macros.h"
#include "aether/scene/entity_guid.h"
#include "aether/scene/prefab.h"

#include <functional>
#include <string>
#include <vector>

namespace aether {

struct LevelInstance {
    std::string scene;      // the scene to place, as the loader understands it
    bool expanded = false;  // set once the instance has been created, so a second expand does nothing
};

// Turns a scene into prefab data. The editor's implementation loads the scene into a temporary world,
// makes a synthetic root (a prefab has one root; a scene has as many as it has), parents the scene's
// roots under it, and calls MakePrefab. Returns false with a reason when the scene cannot be read.
using SceneToPrefab = std::function<bool(const std::string& scene, PrefabData& out, std::string& error)>;

struct LevelInstanceReport {
    usize expanded = 0;
    usize already_expanded = 0;
    std::vector<std::string> failed; // one line per instance that could not be expanded
};

// Instantiates every level instance that has not been expanded yet, once each, and parents it under
// its instance entity. Expanding the same world twice does not duplicate anything.
LevelInstanceReport ExpandLevelInstances(World& world, GuidIndex& guids, const SceneToPrefab& to_prefab);

// The `SceneToPrefab` a caller wants for a scene on disk: loads it into a temporary world, then turns it
// into prefab data. A prefab has one root and a scene has as many as it has, so the scene's roots are
// parented under a synthetic one and the prefab is made from that -- which is the only glue the prefab
// API does not already provide.
bool LoadSceneAsPrefab(const std::string& path, PrefabData& out, std::string& error);

} // namespace aether
// Reflected so a saved scene keeps its level instances and their expanded flag; without this a load
// would forget both and every instance would expand again.
AETHER_REFLECT(aether::LevelInstance, 1,
    AETHER_FIELD(scene, Field_EditAnywhere, {.tooltip = "The scene this instance places"}),
    AETHER_FIELD(expanded, Field_EditAnywhere, {.tooltip = "Set once the instance has been created"}))