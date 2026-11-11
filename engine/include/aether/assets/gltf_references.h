#pragma once

#include "aether/core/base.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace aether::assets {

// A file a .gltf refers to by relative URI (a buffer's .bin, an image).
// Unlike AssetRef GUIDs these are plain paths, so moving either file breaks
// them unless they're rewritten (see MoveContent).
struct GltfFileReference {
    std::string uri;  // as written in the file (percent-encoded)
    std::string path; // the file it points at, relative to the content root; "" if outside it
};

// Percent-encoding for URI paths: keeps unreserved characters and '/'.
std::string UriEncodePath(const std::string& path);
std::string UriDecodePath(const std::string& uri);

// Every external buffer and image URI of the .gltf at `gltf_path` (relative
// to `content_root`); data: URIs are skipped. Empty if it can't be parsed,
// and for a .glb only its external images are listed (its buffers are embedded). A .glb is read for its JSON chunk alone.
std::vector<GltfFileReference> GltfFileReferences(const std::filesystem::path& content_root,
                                                  const std::string& gltf_path);

// Rewrites buffer and image URIs in a .gltf: `rewrite` gets each external URI
// and returns the replacement, or nullopt to keep it. The file is re-saved
// (2-space indented JSON; a .glb is packed again around its untouched binary chunk) only if something changed. Returns the number of
// URIs changed, or -1 if the file couldn't be read or written.
int RewriteGltfUris(const std::filesystem::path& gltf_file,
                    const std::function<std::optional<std::string>(const std::string& uri)>& rewrite,
                    std::string* error = nullptr);

} // namespace aether::assets
