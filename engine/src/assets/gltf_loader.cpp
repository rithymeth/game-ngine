#include "aether/assets/gltf_loader.h"

#include "aether/core/log.h"
#include "aether/math/quaternion.h"
#include "aether/platform/filesystem.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
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
    bool normalized = false;
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
    out.normalized = accessor.value("normalized", false);
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
        return;
    }
    const usize component_size = ComponentSize(info.component_type);
    for (usize i = 0; i < n; ++i) {
        const u8* component = elem + i * component_size;
        f32 value = 0.0f;
        switch (info.component_type) {
            case 5120: { // BYTE
                i8 raw = 0;
                std::memcpy(&raw, component, sizeof(raw));
                if (info.normalized) value = std::max(-1.0f, static_cast<f32>(raw) / 127.0f);
                break;
            }
            case 5121: // UNSIGNED_BYTE
                if (info.normalized) value = static_cast<f32>(*component) / 255.0f;
                break;
            case 5122: { // SHORT
                i16 raw = 0;
                std::memcpy(&raw, component, sizeof(raw));
                if (info.normalized) value = std::max(-1.0f, static_cast<f32>(raw) / 32767.0f);
                break;
            }
            case 5123: { // UNSIGNED_SHORT
                u16 raw = 0;
                std::memcpy(&raw, component, sizeof(raw));
                if (info.normalized) value = static_cast<f32>(raw) / 65535.0f;
                break;
            }
            default:
                break;
        }
        out[i] = value;
    }
}

bool IsFloatOrNormalized(const AccessorInfo& info, bool allow_unsigned) {
    if (info.component_type == 5126) return true;
    if (!info.normalized) return false;
    if (allow_unsigned) return info.component_type == 5121 || info.component_type == 5123;
    return info.component_type == 5120 || info.component_type == 5122;
}

bool IsValidJoints(const AccessorInfo& info, usize vertex_count) {
    return info.count == vertex_count && info.num_components == 4 && !info.normalized &&
           (info.component_type == 5121 || info.component_type == 5123);
}

bool IsValidWeights(const AccessorInfo& info, usize vertex_count) {
    return info.count == vertex_count && info.num_components == 4 &&
           (info.component_type == 5126 ||
            (info.normalized && (info.component_type == 5121 || info.component_type == 5123)));
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

Vec3 ReadVertexVector(const f32 value[3]) { return {value[0], value[1], value[2]}; }

Vec3 StableTangent(const Vec3& normal, const Vec3& tangent) {
    Vec3 orthogonal = tangent - normal * normal.Dot(tangent);
    if (orthogonal.LengthSq() <= 1e-12f) {
        const Vec3 axis = std::abs(normal.x) < 0.8f ? Vec3{1.0f, 0.0f, 0.0f} : Vec3{0.0f, 1.0f, 0.0f};
        orthogonal = axis - normal * normal.Dot(axis);
    }
    return orthogonal.Normalized();
}

void FinalizeTangents(GltfPrimitive& primitive, bool has_tangent_attribute) {
    std::vector<Vec3> tangent_sums;
    std::vector<Vec3> bitangent_sums;
    if (!has_tangent_attribute) {
        tangent_sums.resize(primitive.vertices.size());
        bitangent_sums.resize(primitive.vertices.size());
        for (usize triangle = 0; triangle + 2 < primitive.indices.size(); triangle += 3) {
            const u32 i0 = primitive.indices[triangle];
            const u32 i1 = primitive.indices[triangle + 1];
            const u32 i2 = primitive.indices[triangle + 2];
            if (i0 >= primitive.vertices.size() || i1 >= primitive.vertices.size() || i2 >= primitive.vertices.size()) continue;

            const GltfVertex& v0 = primitive.vertices[i0];
            const GltfVertex& v1 = primitive.vertices[i1];
            const GltfVertex& v2 = primitive.vertices[i2];
            const Vec3 edge1 = ReadVertexVector(v1.position) - ReadVertexVector(v0.position);
            const Vec3 edge2 = ReadVertexVector(v2.position) - ReadVertexVector(v0.position);
            const f32 du1 = v1.uv[0] - v0.uv[0];
            const f32 dv1 = v1.uv[1] - v0.uv[1];
            const f32 du2 = v2.uv[0] - v0.uv[0];
            const f32 dv2 = v2.uv[1] - v0.uv[1];
            const f32 determinant = du1 * dv2 - du2 * dv1;
            if (std::abs(determinant) <= 1e-10f) continue;

            const f32 reciprocal = 1.0f / determinant;
            const Vec3 tangent = (edge1 * dv2 - edge2 * dv1) * reciprocal;
            const Vec3 bitangent = (edge2 * du1 - edge1 * du2) * reciprocal;
            tangent_sums[i0] = tangent_sums[i0] + tangent;
            tangent_sums[i1] = tangent_sums[i1] + tangent;
            tangent_sums[i2] = tangent_sums[i2] + tangent;
            bitangent_sums[i0] = bitangent_sums[i0] + bitangent;
            bitangent_sums[i1] = bitangent_sums[i1] + bitangent;
            bitangent_sums[i2] = bitangent_sums[i2] + bitangent;
        }
    }

    for (usize i = 0; i < primitive.vertices.size(); ++i) {
        GltfVertex& vertex = primitive.vertices[i];
        Vec3 normal = ReadVertexVector(vertex.normal);
        normal = normal.LengthSq() > 1e-12f ? normal.Normalized() : Vec3{0.0f, 1.0f, 0.0f};
        const Vec3 source_tangent = has_tangent_attribute
                                        ? Vec3{vertex.tangent[0], vertex.tangent[1], vertex.tangent[2]}
                                        : tangent_sums[i];
        const Vec3 tangent = StableTangent(normal, source_tangent);
        f32 handedness = has_tangent_attribute ? vertex.tangent[3] : normal.Cross(tangent).Dot(bitangent_sums[i]);
        if (std::abs(handedness) <= 1e-8f) handedness = 1.0f;
        vertex.tangent[0] = tangent.x;
        vertex.tangent[1] = tangent.y;
        vertex.tangent[2] = tangent.z;
        vertex.tangent[3] = handedness < 0.0f ? -1.0f : 1.0f;
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
                           std::vector<GltfNodeInstance>& out_instances,
                           std::vector<Mat4>* out_world_transforms = nullptr) {
    out_instances.clear();
    if (out_world_transforms) out_world_transforms->assign(nodes.size(), Mat4::Identity());
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
        if (out_world_transforms) (*out_world_transforms)[entry.node_index] = world;
        if (node.mesh_index >= 0) {
            out_instances.push_back({static_cast<usize>(node.mesh_index), world, node.skin_index, entry.node_index});
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
        node.name = node_json.value("name", std::string{});
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

            const usize components = channel.ComponentsPerKey();
            if (channel.node_index >= out_scene.nodes.size() || input_info.count == 0 ||
                output_info.count != input_info.count || input_info.component_type != 5126 ||
                input_info.num_components != 1 || output_info.component_type != 5126 ||
                output_info.num_components != components) {
                AETHER_LOG_WARN("glTF", "Skipping animation channel with invalid node or accessor layout");
                continue;
            }

            channel.times.resize(input_info.count);
            bool valid_times = true;
            for (usize i = 0; i < input_info.count; ++i) {
                ReadFloatN(input_info, i, &channel.times[i], 1);
                if (!std::isfinite(channel.times[i]) ||
                    (i > 0 && channel.times[i] <= channel.times[i - 1])) {
                    valid_times = false;
                    break;
                }
            }
            if (!valid_times) {
                AETHER_LOG_WARN("glTF", "Skipping animation channel with non-finite or unordered key times");
                continue;
            }

            channel.values.resize(output_info.count * components);
            bool valid_values = true;
            for (usize i = 0; i < output_info.count; ++i) {
                ReadFloatN(output_info, i, &channel.values[i * components], components);
                for (usize component = 0; component < components; ++component) {
                    if (!std::isfinite(channel.values[i * components + component])) valid_values = false;
                }
            }
            if (!valid_values) {
                AETHER_LOG_WARN("glTF", "Skipping animation channel with non-finite key values");
                continue;
            }

            animation.duration = std::max(animation.duration, channel.times.back());
            animation.channels.push_back(std::move(channel));
        }

        out_scene.animations.push_back(std::move(animation));
    }
}

// Samples a channel's raw float components at `time_seconds`. Binary search
// keeps long animation tracks inexpensive while preserving STEP and LINEAR
// behavior. Shared by both Vec3 paths and the quaternion path
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

    const auto upper = std::upper_bound(channel.times.begin(), channel.times.end(), t);
    const usize k = upper == channel.times.begin() ? 0 : static_cast<usize>(upper - channel.times.begin() - 1);
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
void ApplyAnimationToNodes(const GltfScene& scene, const GltfAnimation& animation, f32 time_seconds,
                           std::vector<GltfNode>& animated_nodes) {
    animated_nodes = scene.nodes;
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
                material.normal_scale = material_json["normalTexture"].value("scale", 1.0f);
            }
            if (material_json.contains("occlusionTexture")) {
                material.occlusion_texture = resolve_texture_path(material_json["occlusionTexture"]);
                material.occlusion_strength = material_json["occlusionTexture"].value("strength", 1.0f);
            }
            if (material_json.contains("emissiveFactor")) {
                const Json& factor = material_json["emissiveFactor"];
                for (usize i = 0; i < 3 && i < factor.size(); ++i) {
                    material.emissive_factor[i] = factor[i].get<f32>();
                }
            }
            const std::string alpha_mode = material_json.value("alphaMode", std::string("OPAQUE"));
            if (alpha_mode == "MASK") material.alpha_mode = MaterialAlphaMode::Mask;
            else if (alpha_mode == "BLEND") material.alpha_mode = MaterialAlphaMode::Blend;
            material.alpha_cutoff = material_json.value("alphaCutoff", 0.5f);
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
                    ResolveAccessor(gltf, attributes["NORMAL"].get<usize>(), buffers, normal_info) &&
                    normal_info.num_components == 3 && IsFloatOrNormalized(normal_info, false) &&
                    normal_info.count == position_info.count;

                AccessorInfo uv_info;
                bool has_uv = attributes.contains("TEXCOORD_0") &&
                              ResolveAccessor(gltf, attributes["TEXCOORD_0"].get<usize>(), buffers, uv_info) &&
                              uv_info.num_components == 2 && IsFloatOrNormalized(uv_info, true) &&
                              uv_info.count == position_info.count;

                AccessorInfo tangent_info;
                const bool has_tangent_attribute =
                    attributes.contains("TANGENT") &&
                    ResolveAccessor(gltf, attributes["TANGENT"].get<usize>(), buffers, tangent_info) &&
                    tangent_info.num_components == 4 && IsFloatOrNormalized(tangent_info, false) &&
                    tangent_info.count == position_info.count;

                AccessorInfo joints_info;
                AccessorInfo weights_info;
                bool has_skinning = attributes.contains("JOINTS_0") && attributes.contains("WEIGHTS_0") &&
                                    ResolveAccessor(gltf, attributes["JOINTS_0"].get<usize>(), buffers, joints_info) &&
                                    ResolveAccessor(gltf, attributes["WEIGHTS_0"].get<usize>(), buffers, weights_info) &&
                                    IsValidJoints(joints_info, position_info.count) &&
                                    IsValidWeights(weights_info, position_info.count);

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
                    if (has_tangent_attribute) {
                        ReadFloatN(tangent_info, i, primitive.vertices[i].tangent, 4);
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

                FinalizeTangents(primitive, has_tangent_attribute);

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
    std::vector<GltfNode> animated_nodes;
    ApplyAnimationToNodes(scene, animation, time_seconds, animated_nodes);
    FlattenNodeInstances(animated_nodes, scene.root_nodes, out_node_instances);
}

void EvaluateAnimationPose(const GltfScene& scene, const GltfAnimation& animation, f32 time_seconds,
                            GltfAnimationPose& out_pose) {
    ApplyAnimationToNodes(scene, animation, time_seconds, out_pose.animated_nodes);
    FlattenNodeInstances(out_pose.animated_nodes, scene.root_nodes, out_pose.node_instances,
                         &out_pose.node_world_transforms);
}

void ComputeSkinMatrices(const GltfAnimationPose& pose, const GltfSkin& skin,
                          std::vector<Mat4>& out_matrices, i32 mesh_node_index) {
    Mat4 inverse_mesh_world = Mat4::Identity();
    if (mesh_node_index >= 0) {
        const usize mesh_node = static_cast<usize>(mesh_node_index);
        if (mesh_node >= pose.node_world_transforms.size() ||
            !pose.node_world_transforms[mesh_node].TryInverse(inverse_mesh_world)) {
            out_matrices.clear();
            return;
        }
    }

    out_matrices.resize(skin.joints.size());
    for (usize k = 0; k < skin.joints.size(); ++k) {
        usize joint_node = skin.joints[k];
        Mat4 joint_world = joint_node < pose.node_world_transforms.size()
                               ? pose.node_world_transforms[joint_node]
                               : Mat4::Identity();
        const Mat4 inverse_bind = k < skin.inverse_bind_matrices.size() ? skin.inverse_bind_matrices[k] : Mat4::Identity();
        out_matrices[k] = inverse_mesh_world * joint_world * inverse_bind;
    }
}

void ComputeSkinMatrices(const GltfScene& scene, const GltfAnimation* animation, f32 time_seconds,
                          const GltfSkin& skin, std::vector<Mat4>& out_matrices, i32 mesh_node_index) {
    GltfAnimationPose pose;
    if (animation) {
        EvaluateAnimationPose(scene, *animation, time_seconds, pose);
    } else {
        pose.animated_nodes = scene.nodes;
        FlattenNodeInstances(pose.animated_nodes, scene.root_nodes, pose.node_instances,
                             &pose.node_world_transforms);
    }
    ComputeSkinMatrices(pose, skin, out_matrices, mesh_node_index);
}

} // namespace aether::assets
