#include "aether/assets/gltf_references.h"

#include "aether/platform/filesystem.h"

#include <cctype>
#include <cstdio>

namespace aether::assets {

namespace stdfs = std::filesystem;

namespace {

bool IsDataUri(const std::string& uri) { return uri.rfind("data:", 0) == 0; }

bool LoadJson(const stdfs::path& file, nlohmann::json& out) {
    std::vector<u8> bytes;
    if (!fs::ReadFileBytes(file.string(), bytes)) {
        return false;
    }
    out = nlohmann::json::parse(bytes.begin(), bytes.end(), nullptr, /*allow_exceptions=*/false);
    return !out.is_discarded() && out.is_object();
}

// Calls `visit` with each external buffer/image "uri" string in the document.
template <typename Json, typename Visit>
void ForEachUri(Json& gltf, Visit&& visit) {
    for (const char* list : {"buffers", "images"}) {
        auto it = gltf.find(list);
        if (it == gltf.end() || !it->is_array()) {
            continue;
        }
        for (auto& item : *it) {
            if (!item.is_object()) {
                continue;
            }
            auto uri = item.find("uri");
            if (uri != item.end() && uri->is_string() && !IsDataUri(uri->template get<std::string>())) {
                visit(*uri);
            }
        }
    }
}

} // namespace

std::string UriEncodePath(const std::string& path) {
    std::string out;
    for (unsigned char c : path) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' || c == '/') {
            out.push_back(static_cast<char>(c));
        } else {
            char buffer[4];
            std::snprintf(buffer, sizeof(buffer), "%%%02X", c);
            out += buffer;
        }
    }
    return out;
}

std::string UriDecodePath(const std::string& uri) {
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::string out;
    for (usize i = 0; i < uri.size(); ++i) {
        if (uri[i] == '%' && i + 2 < uri.size() && hex(uri[i + 1]) >= 0 && hex(uri[i + 2]) >= 0) {
            out.push_back(static_cast<char>((hex(uri[i + 1]) << 4) | hex(uri[i + 2])));
            i += 2;
        } else {
            out.push_back(uri[i]);
        }
    }
    return out;
}

std::vector<GltfFileReference> GltfFileReferences(const stdfs::path& content_root, const std::string& gltf_path) {
    std::vector<GltfFileReference> refs;
    if (stdfs::path(gltf_path).extension() != ".gltf") {
        return refs;
    }
    nlohmann::json gltf;
    if (!LoadJson(content_root / gltf_path, gltf)) {
        return refs;
    }
    const stdfs::path folder = stdfs::path(gltf_path).parent_path();
    ForEachUri(gltf, [&](const nlohmann::json& uri) {
        GltfFileReference ref;
        ref.uri = uri.get<std::string>();
        const stdfs::path target = (folder / UriDecodePath(ref.uri)).lexically_normal();
        const std::string text = target.generic_string();
        ref.path = (text.empty() || text.rfind("..", 0) == 0 || target.is_absolute()) ? std::string() : text;
        refs.push_back(std::move(ref));
    });
    return refs;
}

int RewriteGltfUris(const stdfs::path& gltf_file,
                    const std::function<std::optional<std::string>(const std::string& uri)>& rewrite,
                    std::string* error) {
    nlohmann::json gltf;
    if (!LoadJson(gltf_file, gltf)) {
        if (error != nullptr) {
            *error = "Couldn't read " + gltf_file.string() + " as glTF JSON";
        }
        return -1;
    }
    int changed = 0;
    ForEachUri(gltf, [&](nlohmann::json& uri) {
        if (std::optional<std::string> replacement = rewrite(uri.get<std::string>());
            replacement && *replacement != uri.get<std::string>()) {
            uri = *replacement;
            ++changed;
        }
    });
    if (changed == 0) {
        return 0;
    }
    std::string text = gltf.dump(2);
    text.push_back('\n');
    if (!fs::WriteFileBytes(gltf_file.string(), text.data(), text.size())) {
        if (error != nullptr) {
            *error = "Couldn't write " + gltf_file.string();
        }
        return -1;
    }
    return changed;
}

} // namespace aether::assets
