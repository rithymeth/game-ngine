#pragma once

#include "aether/core/base.h"
#include "aether/math/mat4.h"

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

// One instance of a mesh placed in the scene, with its fully-resolved
// world-space transform — i.e. the product of every ancestor node's local
// transform up to (and including) the scene root, already baked in. This is
// deliberately the *flattened* result of walking the node tree, not the
// tree itself: a renderer just iterates GltfScene::node_instances and draws
// each mesh with its world_transform, with no separate scene-graph walk of
// its own needed. Node *names*, non-mesh nodes (cameras, empty transform
// nodes), and multiple scenes are not preserved — only the default scene
// (or the sole scene, if only one exists) is flattened.
struct GltfNodeInstance {
    usize mesh_index = 0;
    Mat4 world_transform;
};

struct GltfScene {
    std::vector<GltfMesh> meshes;
    std::vector<GltfMaterial> materials;
    std::vector<GltfNodeInstance> node_instances;
};

// Loads a glTF 2.0 asset: JSON parsed via nlohmann::json, buffers resolved
// either from an external .bin file (relative to `path`) or an embedded
// `data:` URI (base64-decoded). Returns false (logged) on failure.
//
// Scope: POSITION/NORMAL/TEXCOORD_0 vertex attributes (missing NORMAL is
// filled with a placeholder up-vector, missing TEXCOORD_0 with zero — a
// primitive with no POSITION accessor is skipped, not fabricated),
// triangle-mode indexed primitives, pbrMetallicRoughness materials with
// file-URI textures, and the node hierarchy/transform tree (TRS or matrix,
// walked from the default scene's root nodes and flattened into
// GltfScene::node_instances — see its comment). A glTF with no "scenes"
// array at all (rare, technically valid) falls back to treating every node
// that isn't referenced as another node's child as a root. Explicitly NOT
// handled (see the README's glTF loader section): skinning/animation,
// embedded (data-URI) images, non-metallic-roughness material extensions
// (KHR_materials_*, etc), and multiple/non-default scenes.
bool LoadGltf(const std::string& path, GltfScene& out_scene);

} // namespace aether::assets
