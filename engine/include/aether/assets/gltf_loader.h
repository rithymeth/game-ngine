#pragma once

#include "aether/core/base.h"
#include "aether/assets/material.h"
#include "aether/math/mat4.h"
#include "aether/math/quaternion.h"

#include <array>
#include <span>
#include <string>
#include <vector>

namespace aether::assets {

struct GltfVertex {
    f32 position[3];
    f32 normal[3];
    f32 uv[2];
    f32 tangent[4]; // xyz tangent direction, w bitangent handedness
};

// Matches the engine's own PBR metallic-roughness workflow (pbr_demo)
// directly — glTF 2.0's default (and only first-class) material model *is*
// metallic-roughness, so no conversion is needed. Texture paths are resolved
// to real filesystem paths (relative to the .gltf file), ready to hand to
// aether::assets::DecodeImageFile/AssetManager; empty if the material has no
// such texture.
struct GltfMaterial {
    f32 base_color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    f32 emissive_factor[3] = {0.0f, 0.0f, 0.0f};
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

struct GltfPrimitive {
    std::vector<GltfVertex> vertices;
    std::vector<u32> indices;
    i32 material_index = -1; // index into GltfScene::materials, -1 if none

    // Skinning data (glTF's JOINTS_0/WEIGHTS_0 attributes), parallel to
    // `vertices` (same size) when BOTH are present on this primitive; empty
    // otherwise, which is how a caller tells a non-skinned primitive apart
    // from a skinned one — don't index these unless they're non-empty.
    // `joint_indices[i][k]` is an index into whichever GltfSkin this
    // primitive's node references (GltfNode::skin_index), not directly into
    // GltfScene::nodes.
    std::vector<std::array<u16, 4>> joint_indices;
    std::vector<std::array<f32, 4>> joint_weights;
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
// (or the sole scene, if only one exists) is flattened. Each instance also
// keeps its source node index for mesh-local skin matrix calculations.
struct GltfNodeInstance {
    usize mesh_index = 0;
    Mat4 world_transform;
    i32 skin_index = -1;
    usize node_index = 0;
};

// A node's raw local transform components (translation/rotation/scale) —
// unlike GltfNodeInstance above, this is kept as separate TRS fields rather
// than pre-baked into a Mat4, specifically so animation playback
// (EvaluateAnimation) can override individual components (a channel might
// animate only rotation, say, leaving translation/scale at these static
// values) without needing to decompose an arbitrary matrix back into TRS.
// `uses_matrix` is true only for the rare node authored with an explicit
// "matrix" instead of TRS fields — per the glTF spec such a node cannot be
// the target of an animation channel, so `matrix` is used verbatim and
// translation/rotation/scale are left at their defaults (never read).
struct GltfNode {
    Vec3 translation{0.0f, 0.0f, 0.0f};
    Quaternion rotation = Quaternion::Identity();
    Vec3 scale{1.0f, 1.0f, 1.0f};
    Mat4 matrix; // valid only if uses_matrix
    bool uses_matrix = false;
    i32 mesh_index = -1; // -1 if this node has no mesh
    i32 skin_index = -1; // -1 if this node's mesh (if any) isn't skinned
    std::vector<usize> children;

    Mat4 LocalTransform() const {
        return uses_matrix ? matrix : Mat4::Translation(translation) * rotation.ToMat4() * Mat4::Scale(scale);
    }
};

// A skin: `joints[k]` is a node index (into GltfScene::nodes) and
// `inverse_bind_matrices[k]` is that joint's inverse bind matrix — the
// transform from mesh-local space into that joint's own rest-pose local
// space, factored out so that runtime skinning uses
// `inverse(mesh_world) * joint_world * inverse_bind_matrices[k]` (see
// ComputeSkinMatrices). Same length as `joints`; per the glTF spec,
// `inverseBindMatrices` is optional and defaults to all-identity when
// omitted (a skin with no inverse binds at all, meaning the joints' rest
// pose already matches mesh space) — that default is applied at parse time
// here, so callers never need to special-case a missing accessor.
struct GltfSkin {
    std::vector<usize> joints;
    std::vector<Mat4> inverse_bind_matrices;
};

enum class GltfAnimationPath { Translation, Rotation, Scale };
enum class GltfAnimationInterpolation { Linear, Step };

// One animated property of one node. `times` is strictly increasing
// (keyframe timestamps, seconds); `values` is `times.size()` groups of
// either 3 floats (Translation/Scale) or 4 floats (Rotation, as an xyzw
// quaternion) laid out contiguously, i.e. `values.size() ==
// times.size() * ComponentsPerKey()`. CUBICSPLINE interpolation (which
// additionally stores in/out tangents per key) is out of this loader's
// scope — a CUBICSPLINE-sampled channel is skipped entirely at parse time,
// same as an unsupported primitive mode elsewhere in this loader — LINEAR
// and STEP cover the overwhelming majority of exported animations.
struct GltfAnimationChannel {
    usize node_index = 0;
    GltfAnimationPath path = GltfAnimationPath::Translation;
    GltfAnimationInterpolation interpolation = GltfAnimationInterpolation::Linear;
    std::vector<f32> times;
    std::vector<f32> values;

    usize ComponentsPerKey() const { return path == GltfAnimationPath::Rotation ? 4 : 3; }
};

struct GltfAnimation {
    std::string name;
    std::vector<GltfAnimationChannel> channels;
    f32 duration = 0.0f; // max keyframe time across all channels, seconds
};

struct GltfScene {
    std::vector<GltfMesh> meshes;
    std::vector<GltfMaterial> materials;
    std::vector<GltfNodeInstance> node_instances;

    // The raw node hierarchy (present alongside the flattened
    // node_instances above specifically so animation playback has
    // something to re-walk) plus skins/animations. Empty when the source
    // glTF has no "nodes"/"skins"/"animations" arrays, same as
    // node_instances is empty for a nodeless file.
    std::vector<GltfNode> nodes;
    std::vector<usize> root_nodes;
    std::vector<GltfSkin> skins;
    std::vector<GltfAnimation> animations;
};

// Reusable result and scratch storage for one sampled animation. Keeping a
// pose around between frames retains vector capacity and shares the sampled
// hierarchy between mesh placement and skin matrix generation.
struct GltfAnimationPose {
    std::vector<GltfNode> animated_nodes;
    std::vector<GltfNodeInstance> node_instances;
    std::vector<Mat4> node_world_transforms;
};

// Re-walks the node hierarchy with `animation` sampled at `time_seconds`
// (channels targeting a node override that node's translation/rotation/
// scale for this evaluation only — GltfScene::nodes itself is never
// mutated) instead of the bind pose ParseNodeInstances used, producing the
// same flattened {mesh_index, world_transform} shape GltfScene::node_instances
// already has. `time_seconds` is clamped to [0, animation.duration], not
// looped — a caller wanting looping playback sends `fmod(t, duration)`
// itself, since "loop" vs. "clamp to last frame" vs. "ping-pong" is a
// policy decision this loader has no basis to make for the caller.
// Multiple channels targeting the same node/path is undefined (last one in
// `animation.channels` wins) — glTF-valid but exotic content this loader
// doesn't need to arbitrate.
void EvaluateAnimation(const GltfScene& scene, const GltfAnimation& animation, f32 time_seconds,
                        std::vector<GltfNodeInstance>& out_node_instances);

// Evaluates an animation into reusable pose storage, including world
// transforms for every node. Prefer this when both mesh instances and skin
// matrices are needed for the same frame.
void EvaluateAnimationPose(const GltfScene& scene, const GltfAnimation& animation, f32 time_seconds,
                            GltfAnimationPose& out_pose);

// Computes mesh-local skin matrices from a pose already evaluated for this
// frame. This avoids sampling channels and walking the hierarchy again.
void ComputeSkinMatrices(const GltfAnimationPose& pose, const GltfSkin& skin,
                          std::vector<Mat4>& out_matrices, i32 mesh_node_index);

// Computes skin matrices in the skinned mesh node's local space:
// inverse(mesh_world) * joint_world * inverse_bind_matrix. Pass the index
// of the mesh node whose vertices will use the matrices. The default -1
// preserves world-space matrices for callers that apply them without a
// mesh-node transform. An invalid/singular mesh transform clears the output.
void ComputeSkinMatrices(const GltfScene& scene, const GltfAnimation* animation, f32 time_seconds,
                          const GltfSkin& skin, std::vector<Mat4>& out_matrices, i32 mesh_node_index = -1);

// Loads a glTF 2.0 asset: JSON parsed via nlohmann::json, buffers resolved
// either from an external .bin file (relative to `path`) or an embedded
// `data:` URI (base64-decoded). Returns false (logged) on failure.
//
// Scope: POSITION/NORMAL/TEXCOORD_0/JOINTS_0/WEIGHTS_0 vertex attributes
// (missing NORMAL is filled with a placeholder up-vector, missing
// TEXCOORD_0 with zero — a primitive with no POSITION accessor is skipped,
// not fabricated; normalized integer NORMAL/TANGENT, TEXCOORD_0, and WEIGHTS_0
// values are decoded per the glTF component rules; JOINTS_0 accepts unsigned
// byte/short and WEIGHTS_0 accepts float or normalized unsigned byte/short.
// Skin attributes are populated only when both accessors are present and valid,
// see GltfPrimitive), triangle-mode indexed primitives,
// pbrMetallicRoughness materials with file-URI textures, the node
// hierarchy/transform tree (TRS or matrix, walked from the default scene's
// root nodes and flattened into GltfScene::node_instances — see its
// comment; also kept unflattened in GltfScene::nodes for animation
// playback), skins (GltfScene::skins — joints + inverse bind matrices), and
// animations (GltfScene::animations — translation/rotation/scale channels,
// LINEAR/STEP interpolation; see EvaluateAnimation/ComputeSkinMatrices for
// playback). A glTF with no "scenes" array at all (rare, technically valid)
// falls back to treating every node that isn't referenced as another node's
// child as a root. Explicitly NOT handled (see the README's glTF loader
// section): CUBICSPLINE animation interpolation, GPU vertex skinning (the
// parsed joint/weight/skin-matrix data is there, but no demo wires it into
// an actual skinned draw call yet), embedded (data-URI) images,
// non-metallic-roughness material extensions (KHR_materials_*, etc), and
// multiple/non-default scenes.
bool LoadGltf(const std::string& path, GltfScene& out_scene);
// Parses a self-contained glTF JSON document. External file URIs are refused;
// data URIs remain available. The input is capped at 32 MiB for untrusted data.
bool LoadGltfFromMemory(std::span<const u8> json, GltfScene& out_scene);

} // namespace aether::assets
