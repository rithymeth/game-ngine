#pragma once

#include "aether/assets/importer.h"
#include "aether/assets/material.h"
#include "aether/math/math.h"
#include "aether/reflection/reflection.h"

#include <array>
#include <string>
#include <vector>

namespace aether::assets {

// ---------------------------------------------------------------------------
// Sub-asset data produced by the model importer
// ---------------------------------------------------------------------------

struct MeshVertex {
    f32 position[3];
    f32 normal[3];
    f32 uv[2];
    f32 tangent[4];
};

struct MeshPrimitiveData {
    std::vector<MeshVertex> vertices;
    std::vector<u32> indices;
    i32 material = -1; // index into ModelData::materials, -1 if none
    // Skinning: one entry per vertex when the primitive is skinned, else empty.
    std::vector<std::array<u16, 4>> joints;
    std::vector<std::array<f32, 4>> weights;
    Vec3 bounds_min{0.0f, 0.0f, 0.0f};
    Vec3 bounds_max{0.0f, 0.0f, 0.0f};
};

// A "Mesh" sub-asset. Stored as a compact binary format ("AMSH" v1) rather
// than through reflection: vertex data is large and read as-is by the GPU.
struct MeshData {
    std::vector<MeshPrimitiveData> primitives;
};

std::vector<u8> EncodeMeshData(const MeshData& mesh);
bool DecodeMeshData(const std::vector<u8>& bytes, MeshData& out);

// A "Material" sub-asset (glTF metallic-roughness). Texture paths are
// relative to the content root, like AssetRecord::path, so they can be looked
// up with AssetDatabase::FindByPath; "" if the material has no such texture.
struct MaterialData {
    Vec4 base_color{1.0f, 1.0f, 1.0f, 1.0f};
    f32 metallic = 1.0f;
    f32 roughness = 1.0f;
    f32 normal_scale = 1.0f;
    f32 occlusion_strength = 1.0f;
    f32 alpha_cutoff = 0.5f;
    MaterialAlphaMode alpha_mode = MaterialAlphaMode::Opaque;
    std::string base_color_texture;
    std::string normal_texture;
    std::string metallic_roughness_texture;
    std::string occlusion_texture;
};

enum class AnimationPath { Translation, Rotation, Scale };
enum class AnimationInterpolation { Linear, Step };

// `values` holds times.size() keys of 3 floats (translation, scale) or 4
// (rotation quaternion x, y, z, w).
struct AnimationChannelData {
    u32 node = 0; // index into ModelData::nodes
    AnimationPath path = AnimationPath::Translation;
    AnimationInterpolation interpolation = AnimationInterpolation::Linear;
    std::vector<f32> times;
    std::vector<f32> values;
};

// An "Animation" sub-asset.
struct AnimationData {
    std::string name;
    f32 duration = 0.0f; // seconds
    std::vector<AnimationChannelData> channels;
};

struct ModelNodeData {
    Vec3 translation{0.0f, 0.0f, 0.0f};
    Quaternion rotation = Quaternion::Identity();
    Vec3 scale{1.0f, 1.0f, 1.0f};
    Mat4 matrix; // used instead of TRS when uses_matrix
    bool uses_matrix = false;
    i32 mesh = -1; // index into ModelData::meshes
    i32 skin = -1; // index into ModelData::skins
    std::vector<u32> children;
};

struct SkinData {
    std::vector<u32> joints; // node indices
    std::vector<Mat4> inverse_bind_matrices;
};

// The model's main imported data: its node tree and skins, plus the keys of
// its sub-assets ("mesh:0", ...) in glTF order. AssetRecord::sub_assets gives
// their GUIDs.
struct ModelData {
    std::vector<ModelNodeData> nodes;
    std::vector<u32> root_nodes;
    std::vector<SkinData> skins;
    std::vector<std::string> meshes;
    std::vector<std::string> materials;
    std::vector<std::string> animations;
};

// Splits a glTF file into sub-assets (docs/design/PHASE_SPECS.md §8.5 step 4):
// one "Mesh" per glTF mesh ("mesh:<i>"), one "Material" per material
// ("material:<i>") and one "Animation" per animation ("animation:<i>").
// The main data is the ModelData manifest. Materials and animations are
// saved with reflection (SaveBinary); meshes with EncodeMeshData.
//
// Settings: "import_materials" (bool, default true; if false, meshes have no
// material) and "import_animations" (bool, default true).
class ModelImporter final : public IAssetImporter {
public:
    const char* Name() const override { return "Model"; }
    u32 Version() const override { return 4; }
    nlohmann::json DefaultSettings() const override;
    ImportResult Import(const ImportContext& context) const override;
};

} // namespace aether::assets

AETHER_ENUM(aether::assets::AnimationPath, 1,
    AETHER_ENUM_VALUE(Translation),
    AETHER_ENUM_VALUE(Rotation),
    AETHER_ENUM_VALUE(Scale)
)

AETHER_ENUM(aether::assets::AnimationInterpolation, 1,
    AETHER_ENUM_VALUE(Linear),
    AETHER_ENUM_VALUE(Step)
)

AETHER_ENUM(aether::assets::MaterialAlphaMode, 1,
    AETHER_ENUM_VALUE(Opaque),
    AETHER_ENUM_VALUE(Mask),
    AETHER_ENUM_VALUE(Blend)
)

AETHER_REFLECT(aether::assets::MaterialData, 4,
    AETHER_FIELD(base_color),
    AETHER_FIELD(metallic),
    AETHER_FIELD(roughness),
    AETHER_FIELD(normal_scale),
    AETHER_FIELD(occlusion_strength),
    AETHER_FIELD(alpha_cutoff),
    AETHER_FIELD(alpha_mode),
    AETHER_FIELD(base_color_texture),
    AETHER_FIELD(normal_texture),
    AETHER_FIELD(metallic_roughness_texture),
    AETHER_FIELD(occlusion_texture)
)

AETHER_REFLECT(aether::assets::AnimationChannelData, 1,
    AETHER_FIELD(node),
    AETHER_FIELD(path),
    AETHER_FIELD(interpolation),
    AETHER_FIELD(times),
    AETHER_FIELD(values)
)

AETHER_REFLECT(aether::assets::AnimationData, 1,
    AETHER_FIELD(name),
    AETHER_FIELD(duration),
    AETHER_FIELD(channels)
)

AETHER_REFLECT(aether::assets::ModelNodeData, 1,
    AETHER_FIELD(translation),
    AETHER_FIELD(rotation),
    AETHER_FIELD(scale),
    AETHER_FIELD(matrix),
    AETHER_FIELD(uses_matrix),
    AETHER_FIELD(mesh),
    AETHER_FIELD(skin),
    AETHER_FIELD(children)
)

AETHER_REFLECT(aether::assets::SkinData, 1,
    AETHER_FIELD(joints),
    AETHER_FIELD(inverse_bind_matrices)
)

AETHER_REFLECT(aether::assets::ModelData, 1,
    AETHER_FIELD(nodes),
    AETHER_FIELD(root_nodes),
    AETHER_FIELD(skins),
    AETHER_FIELD(meshes),
    AETHER_FIELD(materials),
    AETHER_FIELD(animations)
)
