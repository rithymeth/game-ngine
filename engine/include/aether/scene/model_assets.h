#pragma once

#include "aether/assets/asset_database.h"
#include "aether/ecs/world.h"
#include "aether/scene/components.h"

namespace aether {

struct ModelAssetResolveResult {
    usize linked = 0;      // path -> GUID filled in (old data, or a path set by hand)
    usize path_updated = 0; // GUID -> path changed (the asset was renamed/moved)
    usize unresolved = 0;   // neither the GUID nor the path is a known model asset
};

// Keeps every ModelRenderer's `model` GUID and `asset_path` consistent with
// the asset database (docs/design/PHASE_SPECS.md §8.4 path -> GUID migration):
// - `model` set and known: `asset_path` is updated to that asset's current
//   path, so a renamed or moved model keeps rendering.
// - `model` not set (or unknown) but the path is a known model asset:
//   `model` is filled in from it.
// Paths are relative to the database's content root.
ModelAssetResolveResult ResolveModelAssets(World& world, const assets::AssetDatabase& database);

} // namespace aether
