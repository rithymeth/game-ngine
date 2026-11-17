#pragma once

#include "core/scene_document.h"

#include "aether/renderer/material_instance.h"
#include "aether/scene/components.h"

#include <algorithm>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace aether::editor {

// The assets a scene needs in a cook, once each, sorted: the glTF models its ModelRenderers name, and the images its MaterialParameters name as
// texture parameters (a path that is not a file under `content` is left out, so a typo does not stop the cook). Shared by the Live Viewport, the
// Scene View's shading and the Game View, which all cook the open scene the same way.
inline std::vector<std::string> PreviewAssets(const SceneDocument& document, const std::filesystem::path& content) {
    std::vector<std::string> paths;
    document.GetWorld().ForEach<ModelRenderer>([&](const ModelRenderer& renderer) {
        if (renderer.asset_path[0] != '\0') paths.emplace_back(renderer.asset_path);
    });
    document.GetWorld().ForEach<MaterialParameters>([&](const MaterialParameters& overrides) {
        for (const MaterialParameterValue& value : overrides.values) {
            std::error_code ignored;
            if (value.is_texture && !value.texture.empty() && std::filesystem::is_regular_file(content / value.texture, ignored)) paths.push_back(value.texture);
        }
        std::error_code ignored;
        if (!overrides.material.empty() && std::filesystem::is_regular_file(content / overrides.material, ignored)) paths.push_back(overrides.material); // a graph the cook should carry
    });
    std::sort(paths.begin(), paths.end());
    paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
    return paths;
}

} // namespace aether::editor
