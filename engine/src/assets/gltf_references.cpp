#include "aether/assets/gltf_references.h"

#include "aether/assets/gltf_loader.h"
#include "aether/platform/filesystem.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>

namespace aether::assets {

namespace stdfs = std::filesystem;

namespace {

constexpr usize kMaxGltfReferenceJsonBytes = 32 * 1024 * 1024;

bool IsDataUri(const std::string& uri) { return uri.rfind("data:", 0) == 0; }

bool LoadJson(const stdfs::path& file, nlohmann::json& out) {
    std::vector<u8> bytes;
    if (!fs::ReadFileBytes(file.string(), bytes, kMaxGltfReferenceJsonBytes)) {
        return false;
    }
    out = nlohmann::json::parse(bytes.begin(), bytes.end(), nullptr, /*allow_exceptions=*/false);
    return !out.is_discarded() && out.is_object();
}

bool IsGlbFile(const stdfs::path& file) { return file.extension() == ".glb"; }

u32 ReadLe32(const unsigned char* p) { return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8) | (static_cast<u32>(p[2]) << 16) | (static_cast<u32>(p[3]) << 24); }

// The JSON of a .glb, read from its first chunk alone (a model's references are listed on every scan, and its geometry can be megabytes).
bool LoadGlbJson(const stdfs::path& file, nlohmann::json& out) {
    std::ifstream in(file, std::ios::binary);
    unsigned char head[20];
    if (!in.read(reinterpret_cast<char*>(head), sizeof(head))) return false;
    if (std::memcmp(head, "glTF", 4) != 0 || ReadLe32(head + 4) != 2 || ReadLe32(head + 16) != 0x4E4F534Au) return false; // magic, version 2, a JSON chunk first
    const u32 length = ReadLe32(head + 12);
    if (length == 0 || length > kMaxGltfReferenceJsonBytes) return false;
    std::vector<char> text(length);
    if (!in.read(text.data(), static_cast<std::streamsize>(length))) return false;
    out = nlohmann::json::parse(text.begin(), text.end(), nullptr, /*allow_exceptions=*/false);
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
    const stdfs::path extension = stdfs::path(gltf_path).extension();
    if (extension != ".gltf" && extension != ".glb") {
        return refs;
    }
    nlohmann::json gltf;
    // A .glb's buffers are embedded, but its images can still be files beside it.
    if (!(extension == ".glb" ? LoadGlbJson(content_root / gltf_path, gltf) : LoadJson(content_root / gltf_path, gltf))) {
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
    std::vector<u8> glb_bin;
    const bool glb = IsGlbFile(gltf_file);
    bool loaded = false;
    if (glb) {
        // The whole file: the JSON is rewritten and the file packed again around the binary chunk, which is not touched.
        std::vector<u8> bytes;
        std::string json_text;
        if (fs::ReadFileBytes(gltf_file.string(), bytes, usize{1} << 30) && ReadGlbChunks(bytes, json_text, glb_bin)) {
            gltf = nlohmann::json::parse(json_text, nullptr, /*allow_exceptions=*/false);
            loaded = !gltf.is_discarded() && gltf.is_object();
        }
    } else {
        loaded = LoadJson(gltf_file, gltf);
    }
    if (!loaded) {
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
    std::string text = gltf.dump(glb ? -1 : 2);
    if (glb) {
        // GLB chunks are 4-byte aligned: the JSON is padded with spaces, the binary chunk with zeros.
        while (text.size() % 4 != 0) text.push_back(' ');
        while (glb_bin.size() % 4 != 0) glb_bin.push_back(0);
        std::vector<u8> out;
        auto put32 = [&](u32 v) { for (int k = 0; k < 4; ++k) out.push_back(static_cast<u8>(v >> (8 * k))); };
        const u32 total = static_cast<u32>(12 + 8 + text.size() + (glb_bin.empty() ? 0 : 8 + glb_bin.size()));
        put32(0x46546C67u); put32(2u); put32(total);
        put32(static_cast<u32>(text.size())); put32(0x4E4F534Au);
        out.insert(out.end(), text.begin(), text.end());
        if (!glb_bin.empty()) {
            put32(static_cast<u32>(glb_bin.size())); put32(0x004E4942u);
            out.insert(out.end(), glb_bin.begin(), glb_bin.end());
        }
        if (!fs::WriteFileBytes(gltf_file.string(), out.data(), out.size())) {
            if (error != nullptr) *error = "Couldn't write " + gltf_file.string();
            return -1;
        }
        return changed;
    }
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
