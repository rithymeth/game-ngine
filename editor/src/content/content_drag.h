#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <string>

namespace aether::editor {

// What the Content Browser drags: the asset's path relative to the content folder, as a NUL-terminated string. Anything
// that accepts assets (the Scene View, the hierarchy) names this payload.
inline constexpr char kContentPathPayload[] = "AETHER_CONTENT_PATH";

// A model the scene can place as an entity with a ModelRenderer (a glTF or GLB file).
inline bool IsPlaceableModel(const std::string& content_path) {
    const std::size_t dot = content_path.find_last_of('.');
    if (dot == std::string::npos) return false;
    std::string extension = content_path.substr(dot);
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return extension == ".gltf" || extension == ".glb";
}

} // namespace aether::editor
