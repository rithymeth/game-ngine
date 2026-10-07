#include "aether/assets/gltf_loader.h"

#include "aether/core/log.h"
#include "aether/math/quaternion.h"
#include "aether/platform/filesystem.h"

#include <nlohmann/json.hpp>

#include <algorithm>
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

bool ResolveBufferData(const Json& buffer_json, const std::string& base_dir, bool allow_external,
                       std::vector<u8>& out_data) {
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
    if (!allow_external) return false;
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

// JOINTS_0 is typically UNSIGNED_BYTE or UNSIGNED_SHORT (rarely UNSIGNED_INT
// in the wild, but the spec allows only the first two) — reused ReadIndex's
// per-component decode rather than duplicating it, just applied 4 times for
// a VEC4 accessor.
void ReadU16x4(const AccessorInfo& info, usize index, std::array<u16, 4>& out) {
    const u8* elem = info.data + index * info.stride;
    usize component_size = ComponentSize(info.component_type);
    for (usize c = 0; c < 4; ++c) {
        AccessorInfo component_info = info;
        component_info.data = elem + c * component_size;
        component_info.stride = 0;
        out[c] = static_cast<u16>(ReadIndex(component_info, 0));
    }
}

// A node's local transform is either an explicit 4x4 "matrix" (glTF stores
// it column-major, 16 floats — the exact same layout aether::Mat4 itself
// uses, hence the direct memcpy) or separate TRS (translation/rotation/
// scale) fields, each defaulting per the spec to identity/zero/one when
// absent — kept as separate fields (GltfNode::translation/rotation/scale),
// not baked into a matrix here, specifically so EvaluateAnimation can later
// override individual components. glTF's TRS-to-matrix order is T * R * S
// (scale applied first, then rotate, then translate) — see
// GltfNode::LocalTransform.
void ParseNodeTRS(const Json& node_json, GltfNode& out_node) {
    if (node_json.contains("matrix")) {
        const Json& matrix_json = node_json["matrix"];
        f32 values[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        for (usize i = 0; i < 16 && i < matrix_json.size(); ++i) {
            values[i] = matrix_json[i].get<f32>();
        }
        std::memcpy(&out_node.matrix, values, sizeof(values));
        out_node.uses_matrix = true;
        return;
    }

    if (node_json.contains("translation")) {
        const Json& t = node_json["translation"];
        out_node.translation = Vec3(t[0].get<f32>(), t[1].get<f32>(), t[2].get<f32>());
    }
    if (node_json.contains("rotation")) {
        const Json& r = node_json["rotation"];
        out_node.rotation = Quaternion(r[0].get<f32>(), r[1].get<f32>(), r[2].get<f32>(), r[3].get<f32>());
    }
    if (node_json.contains("scale")) {
        const Json& s = node_json["scale"];
        out_node.scale = Vec3(s[0].get<f32>(), s[1].get<f32>(), s[2].get<f32>());
    }
}

// Walks `nodes` from `roots`, accumulating world = parent_world * local per
// node, and records one GltfNodeInstance per mesh-carrying node — the
// flattened result a renderer actually needs, rather than exposing the tree
// itself. Iterative (an explicit stack), not recursive, so a pathologically
// deep hierarchy can't blow the call stack. Shared by both the static bind
// pose (LoadGltf, called on GltfScene::nodes as parsed) and animation
// playback (EvaluateAnimation, called on a temporary animated copy).
void FlattenNodeInstances(const std::vector<GltfNode>& nodes, const std::vector<usize>& roots,
                           std::vector<GltfNodeInstance>& out_instances) {
    struct StackEntry {
        usize node_index;
        Mat4 parent_world;
    };
    std::vector<StackEntry> stack;
    for (usize root : roots) {
        stack.push_back({root, Mat4::Identity()});
    }
    while (!stack.empty()) {
        StackEntry entry = stack.back();
        stack.pop_back();
        const GltfNode& node = nodes[entry.node_index];
        Mat4 world = entry.parent_world * node.LocalTransform();
        if (node.mesh_index >= 0) {
            out_instances.push_back({static_cast<usize>(node.mesh_index), world});
        }
        for (usize child : node.children) {
            stack.push_back({child, world});
        }
    }
}

// Builds GltfScene::nodes/root_nodes from the glTF's "nodes"/"scenes"
// arrays (see ParseNodeTRS for the local-transform parsing, and the
// "no scenes array" root-detection fallback this mirrors from the
// pre-animation-follow-up version of this function).
void ParseNodeHierarchy(const Json& gltf, GltfScene& out_scene) {
    if (!gltf.contains("nodes")) {
        return;
    }
    const Json& nodes_json = gltf["nodes"];
    usize node_count = nodes_json.size();

    out_scene.nodes.resize(node_count);
    std::vector<bool> is_child(node_count, false);

    for (usize i = 0; i < node_count; ++i) {
        const Json& node_json = nodes_json[i];
        GltfNode& node = out_scene.nodes[i];
        ParseNodeTRS(node_json, node);
        if (node_json.contains("mesh")) {
            node.mesh_index = node_json["mesh"].get<i32>();
        }
        if (node_json.contains("skin")) {
            node.skin_index = node_json["skin"].get<i32>();
        }
        if (node_json.contains("children")) {
            for (const auto& child_ref : node_json["children"]) {
                usize child_index = child_ref.get<usize>();
                if (child_index < node_count) {
                    node.children.push_back(child_index);
                    is_child[child_index] = true;
                }
            }
        }
    }

    if (gltf.contains("scenes") && !gltf["scenes"].empty()) {
        usize scene_index = gltf.value("scene", static_cast<usize>(0));
        if (scene_index >= gltf["scenes"].size()) {
            scene_index = 0;
        }
        if (gltf["scenes"][scene_index].contains("nodes")) {
            for (const auto& root_ref : gltf["scenes"][scene_index]["nodes"]) {
                usize root_index = root_ref.get<usize>();
                if (root_index < node_count) {
                    out_scene.root_nodes.push_back(root_index);
                }
            }
        }
    } else {
        for (usize i = 0; i < node_count; ++i) {
            if (!is_child[i]) {
                out_scene.root_nodes.push_back(i);
            }
        }
    }
}

// glTF's inverseBindMatrices accessor (when present) is MAT4-typed,
// column-major, same layout as ParseNodeTRS's "matrix" case — read via
// ReadFloatN 16 components at a time rather than a dedicated MAT4 reader,
// since ReadFloatN already handles the only componentType (FLOAT) this
// accessor is ever encoded as.
void ParseSkins(const Json& gltf, const std::vector<std::vector<u8>>& buffers, GltfScene& out_scene) {
    if (!gltf.contains("skins")) {
        return;
    }
    for (const auto& skin_json : gltf["skins"]) {
        GltfSkin skin;
        if (skin_json.contains("joints")) {
            for (const auto& joint_ref : skin_json["joints"]) {
                skin.joints.push_back(joint_ref.get<usize>());
            }
        }
        skin.inverse_bind_matrices.assign(skin.joints.size(), Mat4::Identity());
        if (skin_json.contains("inverseBindMatrices")) {
            AccessorInfo info;
            if (ResolveAccessor(gltf, skin_json["inverseBindMatrices"].get<usize>(), buffers, info)) {
                usize count = std::min(info.count, skin.joints.size());
                for (usize i = 0; i < count; ++i) {
                    f32 values[16];
                    ReadFloatN(info, i, values, 16);
                    std::memcpy(&skin.inverse_bind_matrices[i], values, sizeof(values));
                }
            }
        }
        out_scene.skins.push_back(std::move(skin));
    }
}

GltfAnimationPath ParseAnimationPath(const std::string& path, bool& out_supported) {
    out_supported = true;
    if (path == "translation") return GltfAnimationPath::Translation;
    if (path == "rotation") return GltfAnimationPath::Rotation;
    if (path == "scale") return GltfAnimationPath::Scale;
    out_supported = false; // "weights" (morph targets) — out of scope
    return GltfAnimationPath::Translation;
}

void ParseAnimations(const Json& gltf, const std::vector<std::vector<u8>>& buffers, GltfScene& out_scene) {
    if (!gltf.contains("animations")) {
        return;
    }
    for (const auto& anim_json : gltf["animations"]) {
        if (!anim_json.contains("channels") || !anim_json.contains("samplers")) {
            continue;
        }
        GltfAnimation animation;
        animation.name = anim_json.value("name", std::string());
        const Json& samplers_json = anim_json["samplers"];

        for (const auto& channel_json : anim_json["channels"]) {
            if (!channel_json.contains("target") || !channel_json["target"].contains("node")) {
                continue; // targetless channel (e.g. a pointer extension) — out of scope
            }
            const Json& target = channel_json["target"];
            bool path_supported = false;
            GltfAnimationPath path = ParseAnimationPath(target.value("path", std::string()), path_supported);
            if (!path_supported) {
                AETHER_LOG_WARN("glTF", "Skipping animation channel with unsupported target path \"%s\"",
                                 target.value("path", std::string()).c_str());
                continue;
            }

            usize sampler_index = channel_json.value("sampler", static_cast<usize>(0));
            if (sampler_index >= samplers_json.size()) {
                continue;
            }
            const Json& sampler_json = samplers_json[sampler_index];
            std::string interpolation_str = sampler_json.value("interpolation", std::string("LINEAR"));
            if (interpolation_str == "CUBICSPLINE") {
                AETHER_LOG_WARN("glTF", "Skipping CUBICSPLINE-interpolated animation channel (unsupported)");
                continue;
            }

            AccessorInfo input_info;
            AccessorInfo output_info;
            if (!sampler_json.contains("input") || !sampler_json.contains("output") ||
                !ResolveAccessor(gltf, sampler_json["input"].get<usize>(), buffers, input_info) ||
                !ResolveAccessor(gltf, sampler_json["output"].get<usize>(), buffers, output_info)) {
                continue;
            }

            GltfAnimationChannel channel;
            channel.node_index = target["node"].get<usize>();
            channel.path = path;
            channel.interpolation =
                interpolation_str == "STEP" ? GltfAnimationInterpolation::Step : GltfAnimationInterpolation::Linear;

            channel.times.resize(input_info.count);
            for (usize i = 0; i < input_info.count; ++i) {
                ReadFloatN(input_info, i, &channel.times[i], 1);
                animation.duration = std::max(animation.duration, channel.times[i]);
            }

            usize components = channel.ComponentsPerKey();
            channel.values.resize(output_info.count * components);
            for (usize i = 0; i < output_info.count; ++i) {
                ReadFloatN(output_info, i, &channel.values[i * components], components);
            }

            animation.channels.push_back(std::move(channel));
        }

        out_scene.animations.push_back(std::move(animation));
    }
}

// Linear (STEP: nearest-previous-keyframe) sampling of a channel's raw
// float components at `time_seconds`, already clamped to the channel's own
// [times.front(), times.back()] range by the caller conceptually — done
// here since it needs `channel.times` to compute the clamp anyway. Shared
// by both Vec3 paths (translation/scale) and the quaternion path
// (component-wise LERP + renormalize below — a standard cheap
// approximation of SLERP, accurate enough for reasonably dense keyframes,
// without the extra acos/sin SLERP needs).
void SampleChannelRaw(const GltfAnimationChannel& channel, f32 time_seconds, f32* out) {
    usize n = channel.ComponentsPerKey();
    if (channel.times.empty()) {
        for (usize i = 0; i < n; ++i) {
            out[i] = 0.0f;
        }
        return;
    }
    f32 t = std::clamp(time_seconds, channel.times.front(), channel.times.back());

    usize k = 0;
    while (k + 1 < channel.times.size() && channel.times[k + 1] <= t) {
        ++k;
    }
    const f32* v0 = &channel.values[k * n];
    if (k + 1 >= channel.times.size() || channel.interpolation == GltfAnimationInterpolation::Step) {
        for (usize i = 0; i < n; ++i) {
            out[i] = v0[i];
        }
        return;
    }
    f32 t0 = channel.times[k];
    f32 t1 = channel.times[k + 1];
    f32 alpha = (t1 > t0) ? (t - t0) / (t1 - t0) : 0.0f;
    const f32* v1 = &channel.values[(k + 1) * n];
    for (usize i = 0; i < n; ++i) {
        out[i] = v0[i] + (v1[i] - v0[i]) * alpha;
    }
}

// Applies every channel in `animation` to a working copy of `scene.nodes`
// (translation/rotation/scale only — a matrix-based node's channels, if any
// somehow exist in spec-invalid content, are simply not applied, since
// GltfNode::LocalTransform ignores TRS fields when uses_matrix is set).
std::vector<GltfNode> ApplyAnimationToNodes(const GltfScene& scene, const GltfAnimation& animation,
                                             f32 time_seconds) {
    std::vector<GltfNode> animated_nodes = scene.nodes;
    for (const GltfAnimationChannel& channel : animation.channels) {
        if (channel.node_index >= animated_nodes.size()) {
            continue;
        }
        GltfNode& node = animated_nodes[channel.node_index];
        if (node.uses_matrix) {
            continue;
        }
        f32 values[4];
        SampleChannelRaw(channel, time_seconds, values);
        switch (channel.path) {
            case GltfAnimationPath::Translation:
                node.translation = Vec3(values[0], values[1], values[2]);
                break;
            case GltfAnimationPath::Scale:
                node.scale = Vec3(values[0], values[1], values[2]);
                break;
            case GltfAnimationPath::Rotation:
                node.rotation = Quaternion(values[0], values[1], values[2], values[3]).Normalized();
                break;
        }
    }
    return animated_nodes;
}

} // namespace

static bool ParseGltfText(const std::string& text, const std::string& path, bool allow_external,
                          GltfScene& out_scene) {
    Json gltf;
    try {
        gltf = Json::parse(text);
    } catch (const Json::parse_error& e) {
        AETHER_LOG_ERROR("glTF", "Failed to parse \"%s\": %s", path.c_str(), e.what());
        return false;
    }

    std::string base_dir = allow_external ? fs::ParentPath(path) : std::string();

    std::vector<std::vector<u8>> buffers;
    if (gltf.contains("buffers")) {
        for (const auto& buffer_json : gltf["buffers"]) {
            std::vector<u8> data;
            if (!ResolveBufferData(buffer_json, base_dir, allow_external, data)) {
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
        return allow_external ? JoinPath(base_dir, UriDecode(uri)) : std::string();
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

                AccessorInfo joints_info;
                AccessorInfo weights_info;
                bool has_skinning = attributes.contains("JOINTS_0") && attributes.contains("WEIGHTS_0") &&
                                    ResolveAccessor(gltf, attributes["JOINTS_0"].get<usize>(), buffers, joints_info) &&
                                    ResolveAccessor(gltf, attributes["WEIGHTS_0"].get<usize>(), buffers, weights_info);

                GltfPrimitive primitive;
                primitive.vertices.resize(position_info.count);
                if (has_skinning) {
                    primitive.joint_indices.resize(position_info.count);
                    primitive.joint_weights.resize(position_info.count);
                }
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
                    if (has_skinning) {
                        ReadU16x4(joints_info, i, primitive.joint_indices[i]);
                        ReadFloatN(weights_info, i, primitive.joint_weights[i].data(), 4);
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

    ParseNodeHierarchy(gltf, out_scene);
    FlattenNodeInstances(out_scene.nodes, out_scene.root_nodes, out_scene.node_instances);
    ParseSkins(gltf, buffers, out_scene);
    ParseAnimations(gltf, buffers, out_scene);

    AETHER_LOG_INFO("glTF",
                     "Loaded \"%s\": %zu mesh(es), %zu material(s), %zu node instance(s), %zu skin(s), "
                     "%zu animation(s)",
                     path.c_str(), out_scene.meshes.size(), out_scene.materials.size(),
                     out_scene.node_instances.size(), out_scene.skins.size(), out_scene.animations.size());
    return true;
}

bool LoadGltf(const std::string& path, GltfScene& out_scene) {
    std::string text;
    if (!fs::ReadFileText(path, text)) {
        AETHER_LOG_ERROR("glTF", "Failed to read \"%s\"", path.c_str());
        return false;
    }
    GltfScene scene;
    try {
        if (!ParseGltfText(text, path, true, scene)) return false;
    } catch (const std::exception& e) {
        AETHER_LOG_ERROR("glTF", "Malformed \"%s\": %s", path.c_str(), e.what());
        return false;
    }
    out_scene = std::move(scene);
    return true;
}

bool LoadGltfFromMemory(std::span<const u8> json, GltfScene& out_scene) {
    constexpr usize kMaxJsonBytes = 32 * 1024 * 1024;
    if (json.empty() || json.size() > kMaxJsonBytes) return false;
    const std::string text(reinterpret_cast<const char*>(json.data()), json.size());
    GltfScene scene;
    try {
        if (!ParseGltfText(text, "<memory>", false, scene)) return false;
    } catch (const std::exception&) {
        return false;
    }
    out_scene = std::move(scene);
    return true;
}

void EvaluateAnimation(const GltfScene& scene, const GltfAnimation& animation, f32 time_seconds,
                        std::vector<GltfNodeInstance>& out_node_instances) {
    out_node_instances.clear();
    std::vector<GltfNode> animated_nodes = ApplyAnimationToNodes(scene, animation, time_seconds);
    FlattenNodeInstances(animated_nodes, scene.root_nodes, out_node_instances);
}

void ComputeSkinMatrices(const GltfScene& scene, const GltfAnimation* animation, f32 time_seconds,
                          const GltfSkin& skin, std::vector<Mat4>& out_matrices) {
    std::vector<GltfNode> animated_nodes =
        animation ? ApplyAnimationToNodes(scene, *animation, time_seconds) : scene.nodes;

    // World transforms for every node, not just the skin's joints — a
    // joint's ancestors may not themselves be joints, so the cheapest
    // correct approach is one full hierarchy walk (same cost
    // FlattenNodeInstances already pays for rendering) rather than N partial
    // walks up from each joint individually.
    std::vector<Mat4> world_transforms(animated_nodes.size(), Mat4::Identity());
    struct StackEntry {
        usize node_index;
        Mat4 parent_world;
    };
    std::vector<StackEntry> stack;
    for (usize root : scene.root_nodes) {
        stack.push_back({root, Mat4::Identity()});
    }
    while (!stack.empty()) {
        StackEntry entry = stack.back();
        stack.pop_back();
        Mat4 world = entry.parent_world * animated_nodes[entry.node_index].LocalTransform();
        world_transforms[entry.node_index] = world;
        for (usize child : animated_nodes[entry.node_index].children) {
            stack.push_back({child, world});
        }
    }

    out_matrices.resize(skin.joints.size());
    for (usize k = 0; k < skin.joints.size(); ++k) {
        usize joint_node = skin.joints[k];
        Mat4 joint_world = joint_node < world_transforms.size() ? world_transforms[joint_node] : Mat4::Identity();
        out_matrices[k] = joint_world * skin.inverse_bind_matrices[k];
    }
}

} // namespace aether::assets
