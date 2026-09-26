#pragma once

#include "aether/core/base.h"

#include <string>
#include <vector>

namespace aether::assets {

struct GltfVertex {
    f32 position[3];
    f32 normal[3];
    f32 uv[2];
};

// Matches the engine's own PBR metallic-roughness workflow (pbr_demo)
// directly — glTF 2.0's default (and only first-class) material model *is*
// metallic-roughness, so no conversion is needed. Texture paths are resolved
// to real filesystem paths (relative to the .gltf file), ready to hand to
// aether::assets::DecodeImageFile/AssetManager; empty if the material has no
// such texture.
struct GltfMaterial {
    f32 base_color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    f32 metallic = 1.0f;
    f32 roughness = 1.0f;
    std::string base_color_texture;
    std::string normal_texture;
    std::string metallic_roughness_texture;
};

struct GltfPrimitive {
    std::vector<GltfVertex> vertices;
    std::vector<u32> indices;
    i32 material_index = -1; // index into GltfScene::materials, -1 if none
};

struct GltfMesh {
    std::vector<GltfPrimitive> primitives;
};

struct GltfScene {
    std::vector<GltfMesh> meshes;
    std::vector<GltfMaterial> materials;
};

// Loads a glTF 2.0 asset: JSON parsed via nlohmann::json, buffers resolved
// either from an external .bin file (relative to `path`) or an embedded
// `data:` URI (base64-decoded). Returns false (logged) on failure.
//
// Scope: POSITION/NORMAL/TEXCOORD_0 vertex attributes (missing NORMAL is
// filled with a placeholder up-vector, missing TEXCOORD_0 with zero — a
// primitive with no POSITION accessor is skipped, not fabricated),
// triangle-mode indexed primitives, and pbrMetallicRoughness materials with
// file-URI textures. Explicitly NOT handled (see the README's glTF loader
// section): the node hierarchy/transform tree (every primitive is loaded in
// its own local/bind space, as if it were the sole node in the scene),
// skinning/animation, embedded (data-URI) images, and non-metallic-roughness
// material extensions (KHR_materials_*, etc).
bool LoadGltf(const std::string& path, GltfScene& out_scene);

} // namespace aether::assets
