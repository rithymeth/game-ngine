#pragma once

#include "aether/core/base.h"
#include "aether/reflection/reflection.h"

#include <cstring>
#include <string>

namespace aether {

// A model-carrying entity's reference to a glTF asset (relative to the asset
// directory, e.g. "models/foo.gltf"), not the loaded GPU/CPU data itself —
// renderers cache that separately, keyed by this same path, shared across
// every entity that references it. A fixed-size buffer rather than
// std::string so the component stays plain data. Moved here from the editor
// (docs/design/EDITOR_UI.md §10): it's a runtime component, not an editor one.
// Phase 8 replaces the path with an asset GUID reference.
struct ModelRenderer {
    char asset_path[128] = {};
};

inline void SetModelPath(ModelRenderer& renderer, const std::string& path) {
    std::memset(renderer.asset_path, 0, sizeof(renderer.asset_path));
    std::strncpy(renderer.asset_path, path.c_str(), sizeof(renderer.asset_path) - 1);
}

} // namespace aether

AETHER_REFLECT(aether::ModelRenderer, 1,
    AETHER_FIELD(asset_path, Field_EditAnywhere, {.tooltip = "glTF model, relative to the asset directory"})
)
