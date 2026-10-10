#include "aether/assets/model_importer.h"

#include "aether/assets/gltf_loader.h"
#include "aether/reflection/serialize.h"

#include <algorithm>
#include <cstring>
#include <filesystem>

namespace aether::assets {

namespace stdfs = std::filesystem;

namespace {

constexpr char kMeshMagic[4] = {'A', 'M', 'S', 'H'};
constexpr u32 kMeshFormatVersion = 2;

static_assert(sizeof(MeshVertex) == 12 * sizeof(f32), "MeshVertex is stored as raw bytes");

struct LegacyMeshVertex {
    f32 position[3];
    f32 normal[3];
    f32 uv[2];
};

void AppendU32(std::vector<u8>& out, u32 value) {
    const u8* b = reinterpret_cast<const u8*>(&value);
    out.insert(out.end(), b, b + 4);
}

template <typename T>
void AppendArray(std::vector<u8>& out, const std::vector<T>& items) {
    AppendU32(out, static_cast<u32>(items.size()));
    if (!items.empty()) {
        const u8* b = reinterpret_cast<const u8*>(items.data());
        out.insert(out.end(), b, b + items.size() * sizeof(T));
    }
}

bool ReadBytes(const std::vector<u8>& in, usize& offset, void* out, usize size) {
    if (in.size() - offset < size) {
        return false;
    }
    if (size == 0) {
        return true; // `out` may be null (an empty vector's data())
    }
    std::memcpy(out, in.data() + offset, size);
    offset += size;
    return true;
}

template <typename T>
bool ReadArray(const std::vector<u8>& in, usize& offset, std::vector<T>& items) {
    u32 count = 0;
    if (!ReadBytes(in, offset, &count, 4) || (in.size() - offset) / sizeof(T) < count) {
        return false;
    }
    items.resize(count);
    return ReadBytes(in, offset, items.data(), static_cast<usize>(count) * sizeof(T));
}

bool SettingOr(const nlohmann::json& settings, const char* name, bool fallback, std::vector<std::string>& warnings) {
    auto it = settings.find(name);
    if (it == settings.end()) {
        return fallback;
    }
    if (!it->is_boolean()) {
        warnings.push_back(std::string("Setting \"") + name + "\" has the wrong type; using the default");
        return fallback;
    }
    return it->get<bool>();
}

// The loader resolves texture URIs against the .gltf's folder. Re-express
// them relative to the content root, as asset paths are.
std::string ContentRelativeTexture(const std::string& resolved, const ImportContext& context,
                                   std::vector<std::string>& warnings) {
    if (resolved.empty()) {
        return {};
    }
    const stdfs::path from_model =
        stdfs::path(resolved).lexically_normal().lexically_relative(context.source.parent_path().lexically_normal());
    const stdfs::path in_content =
        (stdfs::path(context.record.path).parent_path() / from_model).lexically_normal();
    const std::string text = in_content.generic_string();
    if (from_model.empty() || text.empty() || text.rfind("..", 0) == 0) {
        warnings.push_back("Texture " + resolved + " is outside the content folder; its path is kept as-is");
        return stdfs::path(resolved).generic_string();
    }
    return text;
}

std::string Key(const char* kind, usize index) { return std::string(kind) + ":" + std::to_string(index); }

} // namespace

std::vector<u8> EncodeMeshData(const MeshData& mesh) {
    std::vector<u8> out(kMeshMagic, kMeshMagic + 4);
    AppendU32(out, kMeshFormatVersion);
    AppendU32(out, static_cast<u32>(mesh.primitives.size()));
    for (const MeshPrimitiveData& primitive : mesh.primitives) {
        AppendU32(out, static_cast<u32>(primitive.material));
        const f32 bounds[6] = {primitive.bounds_min.x, primitive.bounds_min.y, primitive.bounds_min.z,
                               primitive.bounds_max.x, primitive.bounds_max.y, primitive.bounds_max.z};
        const u8* b = reinterpret_cast<const u8*>(bounds);
        out.insert(out.end(), b, b + sizeof(bounds));
        AppendArray(out, primitive.vertices);
        AppendArray(out, primitive.indices);
        AppendArray(out, primitive.joints);
        AppendArray(out, primitive.weights);
    }
    return out;
}

bool DecodeMeshData(const std::vector<u8>& bytes, MeshData& out) {
    usize offset = 0;
    char magic[4];
    u32 version = 0;
    u32 count = 0;
    if (!ReadBytes(bytes, offset, magic, 4) || std::memcmp(magic, kMeshMagic, 4) != 0 ||
        !ReadBytes(bytes, offset, &version, 4) || (version != 1 && version != kMeshFormatVersion) ||
        !ReadBytes(bytes, offset, &count, 4)) {
        return false;
    }
    MeshData mesh;
    for (u32 i = 0; i < count; ++i) {
        MeshPrimitiveData primitive;
        f32 bounds[6];
        if (!ReadBytes(bytes, offset, &primitive.material, 4) || !ReadBytes(bytes, offset, bounds, sizeof(bounds))) {
            return false;
        }
        if (version == 1) {
            u32 vertex_count = 0;
            if (!ReadBytes(bytes, offset, &vertex_count, 4) ||
                (bytes.size() - offset) / sizeof(LegacyMeshVertex) < vertex_count) return false;
            std::vector<LegacyMeshVertex> legacy_vertices(vertex_count);
            if (!ReadBytes(bytes, offset, legacy_vertices.data(),
                           static_cast<usize>(vertex_count) * sizeof(LegacyMeshVertex))) return false;
            primitive.vertices.resize(vertex_count);
            for (usize vertex = 0; vertex < vertex_count; ++vertex) {
                std::copy(std::begin(legacy_vertices[vertex].position), std::end(legacy_vertices[vertex].position),
                          primitive.vertices[vertex].position);
                std::copy(std::begin(legacy_vertices[vertex].normal), std::end(legacy_vertices[vertex].normal),
                          primitive.vertices[vertex].normal);
                std::copy(std::begin(legacy_vertices[vertex].uv), std::end(legacy_vertices[vertex].uv),
                          primitive.vertices[vertex].uv);
                primitive.vertices[vertex].tangent[0] = 1.0f;
                primitive.vertices[vertex].tangent[1] = 0.0f;
                primitive.vertices[vertex].tangent[2] = 0.0f;
                primitive.vertices[vertex].tangent[3] = 1.0f;
            }
        } else if (!ReadArray(bytes, offset, primitive.vertices)) {
            return false;
        }
        if (!ReadArray(bytes, offset, primitive.indices) || !ReadArray(bytes, offset, primitive.joints) ||
            !ReadArray(bytes, offset, primitive.weights)) return false;
        primitive.bounds_min = Vec3{bounds[0], bounds[1], bounds[2]};
        primitive.bounds_max = Vec3{bounds[3], bounds[4], bounds[5]};
        mesh.primitives.push_back(std::move(primitive));
    }
    if (offset != bytes.size()) {
        return false;
    }
    out = std::move(mesh);
    return true;
}

nlohmann::json ModelImporter::DefaultSettings() const {
    return {{"import_materials", true}, {"import_animations", true}};
}

ImportResult ModelImporter::Import(const ImportContext& context) const {
    ImportResult result;
    const bool import_materials = SettingOr(context.settings, "import_materials", true, result.warnings);
    const bool import_animations = SettingOr(context.settings, "import_animations", true, result.warnings);

    GltfScene scene;
    if (!LoadGltf(context.source.string(), scene)) {
        result.error = "Couldn't load the glTF file (see the log for details)";
        return result;
    }

    ModelData model;

    if (import_materials) {
        for (usize i = 0; i < scene.materials.size(); ++i) {
            const GltfMaterial& source = scene.materials[i];
            MaterialData material;
            material.base_color = Vec4{source.base_color[0], source.base_color[1], source.base_color[2],
                                       source.base_color[3]};
            material.metallic = source.metallic;
            material.roughness = source.roughness;
            material.normal_scale = source.normal_scale;
            material.occlusion_strength = source.occlusion_strength;
            material.alpha_cutoff = source.alpha_cutoff;
            material.alpha_mode = source.alpha_mode;
            material.base_color_texture = ContentRelativeTexture(source.base_color_texture, context, result.warnings);
            material.normal_texture = ContentRelativeTexture(source.normal_texture, context, result.warnings);
            material.metallic_roughness_texture =
                ContentRelativeTexture(source.metallic_roughness_texture, context, result.warnings);
            material.occlusion_texture = ContentRelativeTexture(source.occlusion_texture, context, result.warnings);
            model.materials.push_back(Key("material", i));
            result.sub_assets.push_back({model.materials.back(), "Material", reflect::SaveBinary(material), {}});
        }
    }

    for (usize i = 0; i < scene.meshes.size(); ++i) {
        MeshData mesh;
        for (const GltfPrimitive& source : scene.meshes[i].primitives) {
            MeshPrimitiveData primitive;
            primitive.vertices.resize(source.vertices.size());
            static_assert(sizeof(MeshVertex) == sizeof(GltfVertex));
            if (!source.vertices.empty()) {
                std::memcpy(primitive.vertices.data(), source.vertices.data(),
                            source.vertices.size() * sizeof(MeshVertex));
            }
            primitive.indices = source.indices;
            primitive.material = import_materials ? source.material_index : -1;
            primitive.joints = source.joint_indices;
            primitive.weights = source.joint_weights;
            if (!primitive.vertices.empty()) {
                Vec3 lo{primitive.vertices[0].position[0], primitive.vertices[0].position[1],
                        primitive.vertices[0].position[2]};
                Vec3 hi = lo;
                for (const MeshVertex& v : primitive.vertices) {
                    lo = Vec3{std::min(lo.x, v.position[0]), std::min(lo.y, v.position[1]), std::min(lo.z, v.position[2])};
                    hi = Vec3{std::max(hi.x, v.position[0]), std::max(hi.y, v.position[1]), std::max(hi.z, v.position[2])};
                }
                primitive.bounds_min = lo;
                primitive.bounds_max = hi;
            }
            mesh.primitives.push_back(std::move(primitive));
        }
        model.meshes.push_back(Key("mesh", i));
        result.sub_assets.push_back({model.meshes.back(), "Mesh", EncodeMeshData(mesh), {}});
    }

    if (import_animations) {
        for (usize i = 0; i < scene.animations.size(); ++i) {
            const GltfAnimation& source = scene.animations[i];
            AnimationData animation;
            animation.name = source.name;
            animation.duration = source.duration;
            for (const GltfAnimationChannel& channel : source.channels) {
                AnimationChannelData data;
                data.node = static_cast<u32>(channel.node_index);
                data.path = channel.path == GltfAnimationPath::Rotation ? AnimationPath::Rotation
                            : channel.path == GltfAnimationPath::Scale  ? AnimationPath::Scale
                                                                        : AnimationPath::Translation;
                data.interpolation = channel.interpolation == GltfAnimationInterpolation::Step
                                         ? AnimationInterpolation::Step
                                         : AnimationInterpolation::Linear;
                data.times = channel.times;
                data.values = channel.values;
                animation.channels.push_back(std::move(data));
            }
            model.animations.push_back(Key("animation", i));
            result.sub_assets.push_back({model.animations.back(), "Animation", reflect::SaveBinary(animation), {}});
        }
    }

    for (const GltfNode& source : scene.nodes) {
        ModelNodeData node;
        node.translation = source.translation;
        node.rotation = source.rotation;
        node.scale = source.scale;
        node.matrix = source.matrix;
        node.uses_matrix = source.uses_matrix;
        node.mesh = source.mesh_index;
        node.skin = source.skin_index;
        for (usize child : source.children) {
            node.children.push_back(static_cast<u32>(child));
        }
        model.nodes.push_back(std::move(node));
    }
    for (usize root : scene.root_nodes) {
        model.root_nodes.push_back(static_cast<u32>(root));
    }
    for (const GltfSkin& source : scene.skins) {
        SkinData skin;
        for (usize joint : source.joints) {
            skin.joints.push_back(static_cast<u32>(joint));
        }
        skin.inverse_bind_matrices = source.inverse_bind_matrices;
        model.skins.push_back(std::move(skin));
    }

    result.data = reflect::SaveBinary(model);
    result.ok = true;
    return result;
}

} // namespace aether::assets
