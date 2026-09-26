#include "aether/assets/gltf_loader.h"

#include "aether/core/log.h"
#include "aether/platform/filesystem.h"

#include <nlohmann/json.hpp>

#include <array>
#include <cstring>

namespace aether::assets {

namespace {

using Json = nlohmann::json;

std::vector<u8> DecodeBase64(const std::string& text) {
    static const std::string kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::array<int, 256> reverse{};
    reverse.fill(-1);
    for (usize i = 0; i < kAlphabet.size(); ++i) {
        reverse[static_cast<u8>(kAlphabet[i])] = static_cast<int>(i);
    }

    std::vector<u8> out;
    int value = 0;
    int bits = -8;
    for (u8 c : text) {
        if (c == '=') {
            break;
        }
        if (reverse[c] == -1) {
            continue; // skip whitespace/newlines
        }
        value = (value << 6) + reverse[c];
        bits += 6;
        if (bits >= 0) {
            out.push_back(static_cast<u8>((value >> bits) & 0xFF));
            bits -= 8;
        }
    }
    return out;
}

std::string JoinPath(const std::string& dir, const std::string& relative) {
    if (dir.empty()) {
        return relative;
    }
    char last = dir.back();
    if (last == '/' || last == '\\') {
        return dir + relative;
    }
    return dir + "/" + relative;
}

// glTF URIs are percent-encoded (e.g. spaces as %20).
std::string UriDecode(const std::string& uri) {
    auto hex_val = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::string out;
    out.reserve(uri.size());
    for (usize i = 0; i < uri.size(); ++i) {
        if (uri[i] == '%' && i + 2 < uri.size()) {
            int hi = hex_val(uri[i + 1]);
            int lo = hex_val(uri[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        out.push_back(uri[i]);
    }
    return out;
}

bool ResolveBufferData(const Json& buffer_json, const std::string& base_dir, std::vector<u8>& out_data) {
    if (!buffer_json.contains("uri")) {
        AETHER_LOG_ERROR("glTF", "Buffer with no uri (GLB-embedded binary chunk) is not supported");
        return false;
    }
    std::string uri = buffer_json["uri"].get<std::string>();
    if (uri.rfind("data:", 0) == 0) {
        usize comma = uri.find(',');
        if (comma == std::string::npos) {
            return false;
        }
        out_data = DecodeBase64(uri.substr(comma + 1));
        return true;
    }
    std::string path = JoinPath(base_dir, UriDecode(uri));
    return fs::ReadFileBytes(path, out_data);
}

usize ComponentSize(int component_type) {
    switch (component_type) {
        case 5120: // BYTE
        case 5121: // UNSIGNED_BYTE
            return 1;
        case 5122: // SHORT
        case 5123: // UNSIGNED_SHORT
            return 2;
        case 5125: // UNSIGNED_INT
        case 5126: // FLOAT
            return 4;
        default:
            return 0;
    }
}

usize NumComponents(const std::string& type) {
    if (type == "SCALAR") return 1;
    if (type == "VEC2") return 2;
    if (type == "VEC3") return 3;
    if (type == "VEC4") return 4;
    if (type == "MAT4") return 16;
    return 0;
}

struct AccessorInfo {
    usize count = 0;
    usize num_components = 0;
    int component_type = 0;
    const u8* data = nullptr;
    usize stride = 0;
};

bool ResolveAccessor(const Json& gltf, usize accessor_index, const std::vector<std::vector<u8>>& buffers,
                      AccessorInfo& out) {
    if (accessor_index >= gltf["accessors"].size()) {
        return false;
    }
    const Json& accessor = gltf["accessors"][accessor_index];
    out.count = accessor.value("count", static_cast<usize>(0));
    out.component_type = accessor.value("componentType", 0);
    out.num_components = NumComponents(accessor.value("type", std::string()));
    usize accessor_byte_offset = accessor.value("byteOffset", static_cast<usize>(0));

    if (!accessor.contains("bufferView")) {
        AETHER_LOG_ERROR("glTF", "Accessor %zu has no bufferView (sparse-only accessors are not supported)",
                          accessor_index);
        return false;
    }
    usize bv_index = accessor["bufferView"].get<usize>();
    if (bv_index >= gltf["bufferViews"].size()) {
        return false;
    }
    const Json& buffer_view = gltf["bufferViews"][bv_index];
    usize buffer_index = buffer_view.value("buffer", static_cast<usize>(0));
    usize bv_byte_offset = buffer_view.value("byteOffset", static_cast<usize>(0));

    usize component_size = ComponentSize(out.component_type);
    usize element_size = component_size * out.num_components;
    if (component_size == 0 || out.num_components == 0) {
        AETHER_LOG_ERROR("glTF", "Accessor %zu has an unrecognized componentType/type", accessor_index);
        return false;
    }
    out.stride = buffer_view.value("byteStride", element_size);

    if (buffer_index >= buffers.size()) {
        return false;
    }
    usize total_offset = bv_byte_offset + accessor_byte_offset;
    usize required = total_offset + (out.count > 0 ? (out.count - 1) * out.stride + element_size : 0);
    if (required > buffers[buffer_index].size()) {
        AETHER_LOG_ERROR("glTF", "Accessor %zu reads past the end of its buffer", accessor_index);
        return false;
    }
    out.data = buffers[buffer_index].data() + total_offset;
    return true;
}

void ReadFloatN(const AccessorInfo& info, usize index, f32* out, usize n) {
    const u8* elem = info.data + index * info.stride;
    if (info.component_type == 5126) { // FLOAT
        std::memcpy(out, elem, n * sizeof(f32));
    } else {
        // Normalized-integer vertex attributes (e.g. UNSIGNED_BYTE/SHORT
        // positions or UVs) are outside this loader's scope — see the
        // header's documented limitations.
        for (usize i = 0; i < n; ++i) {
            out[i] = 0.0f;
        }
    }
}

u32 ReadIndex(const AccessorInfo& info, usize index) {
    const u8* elem = info.data + index * info.stride;
    switch (info.component_type) {
        case 5121: // UNSIGNED_BYTE
            return *elem;
        case 5123: { // UNSIGNED_SHORT
            u16 value;
            std::memcpy(&value, elem, sizeof(value));
            return value;
        }
        case 5125: { // UNSIGNED_INT
            u32 value;
            std::memcpy(&value, elem, sizeof(value));
            return value;
        }
        default:
            return 0;
    }
}

} // namespace

bool LoadGltf(const std::string& path, GltfScene& out_scene) {
    std::string text;
    if (!fs::ReadFileText(path, text)) {
        AETHER_LOG_ERROR("glTF", "Failed to read \"%s\"", path.c_str());
        return false;
    }

    Json gltf;
    try {
        gltf = Json::parse(text);
    } catch (const Json::parse_error& e) {
        AETHER_LOG_ERROR("glTF", "Failed to parse \"%s\": %s", path.c_str(), e.what());
        return false;
    }

    std::string base_dir = fs::ParentPath(path);

    std::vector<std::vector<u8>> buffers;
    if (gltf.contains("buffers")) {
        for (const auto& buffer_json : gltf["buffers"]) {
            std::vector<u8> data;
            if (!ResolveBufferData(buffer_json, base_dir, data)) {
                AETHER_LOG_ERROR("glTF", "Failed to resolve a buffer referenced by \"%s\"", path.c_str());
                return false;
            }
            buffers.push_back(std::move(data));
        }
    }

    auto resolve_texture_path = [&](const Json& texture_ref) -> std::string {
        if (!texture_ref.contains("index") || !gltf.contains("textures")) {
            return {};
        }
        usize texture_index = texture_ref["index"].get<usize>();
        if (texture_index >= gltf["textures"].size()) {
            return {};
        }
        const Json& texture = gltf["textures"][texture_index];
        if (!texture.contains("source") || !gltf.contains("images")) {
            return {};
        }
        usize image_index = texture["source"].get<usize>();
        if (image_index >= gltf["images"].size()) {
            return {};
        }
        const Json& image = gltf["images"][image_index];
        if (!image.contains("uri")) {
            return {}; // embedded (bufferView-referenced or data-URI) image: out of scope
        }
        std::string uri = image["uri"].get<std::string>();
        if (uri.rfind("data:", 0) == 0) {
            return {};
        }
        return JoinPath(base_dir, UriDecode(uri));
    };

    if (gltf.contains("materials")) {
        for (const auto& material_json : gltf["materials"]) {
            GltfMaterial material;
            if (material_json.contains("pbrMetallicRoughness")) {
                const Json& pbr = material_json["pbrMetallicRoughness"];
                if (pbr.contains("baseColorFactor")) {
                    const Json& factor = pbr["baseColorFactor"];
                    for (usize i = 0; i < 4 && i < factor.size(); ++i) {
                        material.base_color[i] = factor[i].get<f32>();
                    }
                }
                material.metallic = pbr.value("metallicFactor", 1.0f);
                material.roughness = pbr.value("roughnessFactor", 1.0f);
                if (pbr.contains("baseColorTexture")) {
                    material.base_color_texture = resolve_texture_path(pbr["baseColorTexture"]);
                }
                if (pbr.contains("metallicRoughnessTexture")) {
                    material.metallic_roughness_texture = resolve_texture_path(pbr["metallicRoughnessTexture"]);
                }
            }
            if (material_json.contains("normalTexture")) {
                material.normal_texture = resolve_texture_path(material_json["normalTexture"]);
            }
            out_scene.materials.push_back(material);
        }
    }

    if (gltf.contains("meshes")) {
        for (const auto& mesh_json : gltf["meshes"]) {
            GltfMesh mesh;
            if (!mesh_json.contains("primitives")) {
                out_scene.meshes.push_back(std::move(mesh));
                continue;
            }
            for (const auto& prim_json : mesh_json["primitives"]) {
                int mode = prim_json.value("mode", 4);
                if (mode != 4) { // 4 == TRIANGLES; fans/strips/points/lines out of scope
                    AETHER_LOG_WARN("glTF", "Skipping primitive with unsupported mode %d (only TRIANGLES)", mode);
                    continue;
                }
                if (!prim_json.contains("attributes") || !prim_json["attributes"].contains("POSITION")) {
                    AETHER_LOG_WARN("glTF", "Skipping primitive with no POSITION attribute");
                    continue;
                }
                const Json& attributes = prim_json["attributes"];

                AccessorInfo position_info;
                if (!ResolveAccessor(gltf, attributes["POSITION"].get<usize>(), buffers, position_info)) {
                    AETHER_LOG_WARN("glTF", "Skipping primitive: failed to resolve POSITION accessor");
                    continue;
                }

                AccessorInfo normal_info;
                bool has_normal =
                    attributes.contains("NORMAL") &&
                    ResolveAccessor(gltf, attributes["NORMAL"].get<usize>(), buffers, normal_info);

                AccessorInfo uv_info;
                bool has_uv = attributes.contains("TEXCOORD_0") &&
                              ResolveAccessor(gltf, attributes["TEXCOORD_0"].get<usize>(), buffers, uv_info);

                GltfPrimitive primitive;
                primitive.vertices.resize(position_info.count);
                for (usize i = 0; i < position_info.count; ++i) {
                    ReadFloatN(position_info, i, primitive.vertices[i].position, 3);
                    if (has_normal) {
                        ReadFloatN(normal_info, i, primitive.vertices[i].normal, 3);
                    } else {
                        primitive.vertices[i].normal[0] = 0.0f;
                        primitive.vertices[i].normal[1] = 1.0f;
                        primitive.vertices[i].normal[2] = 0.0f;
                    }
                    if (has_uv) {
                        ReadFloatN(uv_info, i, primitive.vertices[i].uv, 2);
                    } else {
                        primitive.vertices[i].uv[0] = 0.0f;
                        primitive.vertices[i].uv[1] = 0.0f;
                    }
                }

                if (prim_json.contains("indices")) {
                    AccessorInfo index_info;
                    if (ResolveAccessor(gltf, prim_json["indices"].get<usize>(), buffers, index_info)) {
                        primitive.indices.resize(index_info.count);
                        for (usize i = 0; i < index_info.count; ++i) {
                            primitive.indices[i] = ReadIndex(index_info, i);
                        }
                    }
                } else {
                    primitive.indices.resize(position_info.count);
                    for (usize i = 0; i < position_info.count; ++i) {
                        primitive.indices[i] = static_cast<u32>(i);
                    }
                }

                primitive.material_index = prim_json.value("material", -1);
                mesh.primitives.push_back(std::move(primitive));
            }
            out_scene.meshes.push_back(std::move(mesh));
        }
    }

    AETHER_LOG_INFO("glTF", "Loaded \"%s\": %zu mesh(es), %zu material(s)", path.c_str(), out_scene.meshes.size(),
                     out_scene.materials.size());
    return true;
}

} // namespace aether::assets
