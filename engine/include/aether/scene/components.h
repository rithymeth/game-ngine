#pragma once

#include "aether/assets/asset_ref.h"
#include "aether/core/base.h"
#include "aether/ecs/component.h"
#include "aether/math/math.h"
#include "aether/reflection/reflection.h"

#include <cstring>
#include <string>

namespace aether {

// An entity's position and orientation, relative to its parent if it has a
// Parent (see hierarchy.h), otherwise in world space. Plain data. (Lived in
// the physics module until Phase 7; it isn't physics-specific.)
struct Transform {
    Vec3 position;
    Quaternion rotation;
};

// A model-carrying entity's reference to a glTF asset — not the loaded
// GPU/CPU data itself, which renderers cache separately, shared across every
// entity that references it.
//
// `model` (Phase 8) is the permanent reference, by asset GUID, and survives
// the file being renamed or moved. `asset_path` (relative to the content
// folder, e.g. "models/foo.gltf") is what today's renderer loads from; with an
// AssetDatabase, ResolveModelAssets (scene/model_assets.h) keeps the two in
// step: it fills `model` from the path for old data, and updates the path
// from `model` after a rename. Plain data (fixed-size path buffer).
struct ModelRenderer {
    char asset_path[128] = {};
    assets::AssetRef<assets::ModelAsset> model;
};

inline void SetModelPath(ModelRenderer& renderer, const std::string& path) {
    std::memset(renderer.asset_path, 0, sizeof(renderer.asset_path));
    std::strncpy(renderer.asset_path, path.c_str(), sizeof(renderer.asset_path) - 1);
}

} // namespace aether

AETHER_REFLECT(aether::Transform, 1,
    AETHER_FIELD(position, Field_EditAnywhere, {.units = "m"}),
    AETHER_FIELD(rotation, Field_EditAnywhere)
)

AETHER_REFLECT(aether::ModelRenderer, 1,
    AETHER_FIELD(model, Field_EditAnywhere, {.tooltip = "The glTF model asset"}),
    AETHER_FIELD(asset_path, Field_ReadOnly, {.tooltip = "Path the renderer loads, kept in step with the model asset"})
)

namespace aether::detail {
// Scenes saved before ModelRenderer gained `model` stored it as 128 raw bytes
// (just the path). Load those into the path; ResolveModelAssets then fills in
// the GUID.
inline bool LoadLegacyModelRenderer(void* component, const u8* data, usize size) {
    if (size != sizeof(ModelRenderer::asset_path)) {
        return false;
    }
    auto* renderer = static_cast<ModelRenderer*>(component);
    std::memcpy(renderer->asset_path, data, size);
    renderer->asset_path[sizeof(renderer->asset_path) - 1] = '\0';
    return true;
}
inline const bool kModelRendererLegacyLoader = SetLegacyRawLoader<ModelRenderer>(&LoadLegacyModelRenderer);
} // namespace aether::detail
