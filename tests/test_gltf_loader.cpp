#include "aether/assets/gltf_loader.h"
#include "aether/platform/filesystem.h"
#include "test_framework.h"

#include <cmath>
#include <cstring>
#include <filesystem>
#include <string>

using namespace aether;
using namespace aether::assets;

namespace {

// A minimal, hand-built (not exported from a DCC tool) glTF 2.0 asset: one
// triangle, with an embedded base64 data-URI buffer (positions, normals,
// UVs, unsigned-short indices, each in their own non-interleaved
// bufferView) and one pbrMetallicRoughness material. The base64 payload was
// generated once via a small script and is exact byte-for-byte: 3x float3
// positions (0,0,0)/(1,0,0)/(0,1,0), 3x float3 normals (all (0,0,1)), 3x
// float2 UVs (0,0)/(1,0)/(0,1), then 3x uint16 indices 0,1,2.
constexpr const char* kTriangleGltf = R"({
  "asset": {"version": "2.0"},
  "buffers": [
    {
      "byteLength": 102,
      "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAAAAAAAAAAAAIA/AAAAAAAAAAAAAIA/AAAAAAAAAAAAAIA/AAAAAAAAAAAAAIA/AAAAAAAAAAAAAIA/AAABAAIA"
    }
  ],
  "bufferViews": [
    {"buffer": 0, "byteOffset": 0, "byteLength": 36},
    {"buffer": 0, "byteOffset": 36, "byteLength": 36},
    {"buffer": 0, "byteOffset": 72, "byteLength": 24},
    {"buffer": 0, "byteOffset": 96, "byteLength": 6}
  ],
  "accessors": [
    {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3"},
    {"bufferView": 1, "componentType": 5126, "count": 3, "type": "VEC3"},
    {"bufferView": 2, "componentType": 5126, "count": 3, "type": "VEC2"},
    {"bufferView": 3, "componentType": 5123, "count": 3, "type": "SCALAR"}
  ],
  "materials": [
    {
      "pbrMetallicRoughness": {
        "baseColorFactor": [0.8, 0.2, 0.1, 1.0],
        "metallicFactor": 0.3,
        "roughnessFactor": 0.6
      }
    }
  ],
  "meshes": [
    {
      "primitives": [
        {
          "attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2},
          "indices": 3,
          "material": 0,
          "mode": 4
        }
      ]
    }
  ]
})";

std::string WriteTempGltf(const char* filename, const char* contents) {
    std::string path = (std::filesystem::temp_directory_path() / filename).string();
    AETHER_CHECK(fs::WriteFileBytes(path, contents, std::strlen(contents)));
    return path;
}

// Writes a raw binary blob next to where WriteTempGltf writes its .gltf
// files, so a test .gltf can reference it as an external ".bin" buffer by
// bare filename — much easier to construct by hand than a base64 data URI
// for anything beyond a handful of bytes (kTriangleGltf's embedded buffer
// above is the last time this file did that).
std::string WriteTempBinary(const char* filename, const void* data, usize size) {
    std::string path = (std::filesystem::temp_directory_path() / filename).string();
    AETHER_CHECK(fs::WriteFileBytes(path, data, size));
    return path;
}

// One node (mesh-carrying, no rotation/scale so translation is the only
// moving part) animated from (0,0,0) at t=0 to (10,0,0) at t=1, LINEAR
// interpolation — shared by the channel-parsing test and the two
// EvaluateAnimation tests so the asset itself is only built once.
struct TempAnimAsset {
    std::string gltf_path;
    std::string bin_path;
};

TempAnimAsset WriteTempTranslationAnimGltf() {
    f32 times[2] = {0.0f, 1.0f};
    f32 values[6] = {0.0f, 0.0f, 0.0f, 10.0f, 0.0f, 0.0f};
    std::vector<u8> blob(sizeof(times) + sizeof(values));
    std::memcpy(blob.data(), times, sizeof(times));
    std::memcpy(blob.data() + sizeof(times), values, sizeof(values));

    TempAnimAsset asset;
    asset.bin_path = WriteTempBinary("aether_test_anim.bin", blob.data(), blob.size());
    std::string bin_filename = std::filesystem::path(asset.bin_path).filename().string();

    std::string gltf_json = std::string(R"({
      "asset": {"version": "2.0"},
      "buffers": [ {"uri": ")") +
                             bin_filename + R"(", "byteLength": )" + std::to_string(blob.size()) + R"(} ],
      "bufferViews": [
        {"buffer": 0, "byteOffset": 0, "byteLength": 8},
        {"buffer": 0, "byteOffset": 8, "byteLength": 24}
      ],
      "accessors": [
        {"bufferView": 0, "componentType": 5126, "count": 2, "type": "SCALAR"},
        {"bufferView": 1, "componentType": 5126, "count": 2, "type": "VEC3"}
      ],
      "meshes": [ {"primitives": []} ],
      "nodes": [ {"mesh": 0} ],
      "animations": [
        {
          "channels": [ {"sampler": 0, "target": {"node": 0, "path": "translation"}} ],
          "samplers": [ {"input": 0, "output": 1, "interpolation": "LINEAR"} ]
        }
      ]
    })";
    asset.gltf_path = WriteTempGltf("aether_test_anim.gltf", gltf_json.c_str());
    return asset;
}

} // namespace

AETHER_TEST(Gltf_LoadMissingFileReturnsFalse) {
    GltfScene scene;
    AETHER_CHECK(!LoadGltf("does/not/exist.gltf", scene));
}

AETHER_TEST(Gltf_LoadMalformedJsonReturnsFalse) {
    std::string path = WriteTempGltf("aether_test_malformed.gltf", "{ not valid json");
    GltfScene scene;
    AETHER_CHECK(!LoadGltf(path, scene));
    std::filesystem::remove(path);
}

AETHER_TEST(Gltf_LoadTriangleDecodesVerticesIndicesAndMaterial) {
    std::string path = WriteTempGltf("aether_test_triangle.gltf", kTriangleGltf);

    GltfScene scene;
    AETHER_CHECK(LoadGltf(path, scene));

    AETHER_CHECK(scene.meshes.size() == 1);
    AETHER_CHECK(scene.meshes[0].primitives.size() == 1);
    const GltfPrimitive& prim = scene.meshes[0].primitives[0];

    AETHER_CHECK(prim.vertices.size() == 3);
    AETHER_CHECK(prim.indices.size() == 3);
    AETHER_CHECK(prim.indices[0] == 0);
    AETHER_CHECK(prim.indices[1] == 1);
    AETHER_CHECK(prim.indices[2] == 2);

    const GltfVertex& v0 = prim.vertices[0];
    AETHER_CHECK(v0.position[0] == 0.0f && v0.position[1] == 0.0f && v0.position[2] == 0.0f);
    AETHER_CHECK(v0.normal[0] == 0.0f && v0.normal[1] == 0.0f && v0.normal[2] == 1.0f);
    AETHER_CHECK(v0.uv[0] == 0.0f && v0.uv[1] == 0.0f);

    const GltfVertex& v1 = prim.vertices[1];
    AETHER_CHECK(v1.position[0] == 1.0f && v1.position[1] == 0.0f && v1.position[2] == 0.0f);
    AETHER_CHECK(v1.uv[0] == 1.0f && v1.uv[1] == 0.0f);

    const GltfVertex& v2 = prim.vertices[2];
    AETHER_CHECK(v2.position[0] == 0.0f && v2.position[1] == 1.0f && v2.position[2] == 0.0f);
    AETHER_CHECK(v2.uv[0] == 0.0f && v2.uv[1] == 1.0f);

    AETHER_CHECK(prim.material_index == 0);
    AETHER_CHECK(scene.materials.size() == 1);
    const GltfMaterial& material = scene.materials[0];
    AETHER_CHECK(std::abs(material.base_color[0] - 0.8f) < 1e-5f);
    AETHER_CHECK(std::abs(material.base_color[1] - 0.2f) < 1e-5f);
    AETHER_CHECK(std::abs(material.base_color[2] - 0.1f) < 1e-5f);
    AETHER_CHECK(std::abs(material.base_color[3] - 1.0f) < 1e-5f);
    AETHER_CHECK(std::abs(material.metallic - 0.3f) < 1e-5f);
    AETHER_CHECK(std::abs(material.roughness - 0.6f) < 1e-5f);
    AETHER_CHECK(material.base_color_texture.empty());

    std::filesystem::remove(path);
}

AETHER_TEST(Gltf_MaterialDefaultsMatchSpecWhenFieldsOmitted) {
    constexpr const char* kNoMaterialFields = R"({
      "asset": {"version": "2.0"},
      "materials": [ {"pbrMetallicRoughness": {}} ]
    })";
    std::string path = WriteTempGltf("aether_test_defaults.gltf", kNoMaterialFields);

    GltfScene scene;
    AETHER_CHECK(LoadGltf(path, scene));
    AETHER_CHECK(scene.materials.size() == 1);
    // glTF 2.0 spec defaults: baseColorFactor [1,1,1,1], metallicFactor 1.0,
    // roughnessFactor 1.0.
    const GltfMaterial& material = scene.materials[0];
    AETHER_CHECK(material.base_color[0] == 1.0f && material.base_color[1] == 1.0f && material.base_color[2] == 1.0f &&
                 material.base_color[3] == 1.0f);
    AETHER_CHECK(material.metallic == 1.0f);
    AETHER_CHECK(material.roughness == 1.0f);

    std::filesystem::remove(path);
}

AETHER_TEST(Gltf_NodeHierarchyComposesParentThenChildTransform) {
    // Root: scale x2, no mesh. Child: translate (1,0,0), references mesh 0
    // (an empty mesh — geometry is irrelevant to this test, only the
    // resulting world transform is). If composition order were wrong
    // (child * parent instead of parent * child), the mesh-local origin
    // would land at world (1,0,0) instead of the correct (2,0,0) — scale
    // and translation don't commute, unlike two pure translations, so this
    // genuinely distinguishes the two orders rather than passing either way.
    constexpr const char* kHierarchyGltf = R"({
      "asset": {"version": "2.0"},
      "meshes": [ {"primitives": []} ],
      "nodes": [
        {"scale": [2.0, 2.0, 2.0], "children": [1]},
        {"translation": [1.0, 0.0, 0.0], "mesh": 0}
      ],
      "scenes": [ {"nodes": [0]} ],
      "scene": 0
    })";
    std::string path = WriteTempGltf("aether_test_hierarchy.gltf", kHierarchyGltf);

    GltfScene scene;
    AETHER_CHECK(LoadGltf(path, scene));
    AETHER_CHECK(scene.node_instances.size() == 1);
    AETHER_CHECK(scene.node_instances[0].mesh_index == 0);

    const Vec4& world_origin = scene.node_instances[0].world_transform.cols[3];
    AETHER_CHECK(std::abs(world_origin.x - 2.0f) < 1e-5f);
    AETHER_CHECK(std::abs(world_origin.y - 0.0f) < 1e-5f);
    AETHER_CHECK(std::abs(world_origin.z - 0.0f) < 1e-5f);

    std::filesystem::remove(path);
}

AETHER_TEST(Gltf_NodeHierarchyMultipleMeshInstances) {
    constexpr const char* kMultiInstanceGltf = R"({
      "asset": {"version": "2.0"},
      "meshes": [ {"primitives": []} ],
      "nodes": [
        {"mesh": 0, "translation": [-5.0, 0.0, 0.0]},
        {"mesh": 0, "translation": [5.0, 0.0, 0.0]}
      ],
      "scenes": [ {"nodes": [0, 1]} ],
      "scene": 0
    })";
    std::string path = WriteTempGltf("aether_test_multi_instance.gltf", kMultiInstanceGltf);

    GltfScene scene;
    AETHER_CHECK(LoadGltf(path, scene));
    AETHER_CHECK(scene.node_instances.size() == 2);

    bool found_left = false;
    bool found_right = false;
    for (const auto& instance : scene.node_instances) {
        AETHER_CHECK(instance.mesh_index == 0);
        f32 x = instance.world_transform.cols[3].x;
        if (std::abs(x - -5.0f) < 1e-5f) found_left = true;
        if (std::abs(x - 5.0f) < 1e-5f) found_right = true;
    }
    AETHER_CHECK(found_left);
    AETHER_CHECK(found_right);

    std::filesystem::remove(path);
}

AETHER_TEST(Gltf_NoScenesArrayFallsBackToNonChildNodesAsRoots) {
    // No "scenes"/"scene" at all (technically valid glTF) — every node that
    // isn't referenced as another node's child should be treated as a root.
    // Node 0 is a child of node 1, so only node 1 (and transitively node 0
    // under it) should be walked; node 0 must NOT also be walked a second
    // time as a spurious extra root.
    constexpr const char* kNoScenesGltf = R"({
      "asset": {"version": "2.0"},
      "meshes": [ {"primitives": []} ],
      "nodes": [
        {"mesh": 0},
        {"translation": [3.0, 0.0, 0.0], "children": [0]}
      ]
    })";
    std::string path = WriteTempGltf("aether_test_no_scenes.gltf", kNoScenesGltf);

    GltfScene scene;
    AETHER_CHECK(LoadGltf(path, scene));
    AETHER_CHECK(scene.node_instances.size() == 1);
    AETHER_CHECK(std::abs(scene.node_instances[0].world_transform.cols[3].x - 3.0f) < 1e-5f);

    std::filesystem::remove(path);
}

AETHER_TEST(Gltf_AnimationChannelParsesTimesAndValues) {
    TempAnimAsset asset = WriteTempTranslationAnimGltf();

    GltfScene scene;
    AETHER_CHECK(LoadGltf(asset.gltf_path, scene));
    AETHER_CHECK(scene.animations.size() == 1);
    AETHER_CHECK(scene.animations[0].channels.size() == 1);

    const GltfAnimationChannel& channel = scene.animations[0].channels[0];
    AETHER_CHECK(channel.node_index == 0);
    AETHER_CHECK(channel.path == GltfAnimationPath::Translation);
    AETHER_CHECK(channel.interpolation == GltfAnimationInterpolation::Linear);
    AETHER_CHECK(channel.times.size() == 2);
    AETHER_CHECK(std::abs(channel.times[0] - 0.0f) < 1e-5f);
    AETHER_CHECK(std::abs(channel.times[1] - 1.0f) < 1e-5f);
    AETHER_CHECK(channel.values.size() == 6);
    AETHER_CHECK(std::abs(channel.values[3] - 10.0f) < 1e-5f);
    AETHER_CHECK(std::abs(scene.animations[0].duration - 1.0f) < 1e-5f);

    std::filesystem::remove(asset.gltf_path);
    std::filesystem::remove(asset.bin_path);
}

AETHER_TEST(Gltf_EvaluateAnimationInterpolatesLinearlyBetweenKeyframes) {
    TempAnimAsset asset = WriteTempTranslationAnimGltf();

    GltfScene scene;
    AETHER_CHECK(LoadGltf(asset.gltf_path, scene));
    AETHER_CHECK(scene.animations.size() == 1);

    std::vector<GltfNodeInstance> animated;
    EvaluateAnimation(scene, scene.animations[0], 0.5f, animated);
    AETHER_CHECK(animated.size() == 1);
    AETHER_CHECK(std::abs(animated[0].world_transform.cols[3].x - 5.0f) < 1e-4f);

    std::filesystem::remove(asset.gltf_path);
    std::filesystem::remove(asset.bin_path);
}

AETHER_TEST(Gltf_EvaluateAnimationClampsPastTheLastKeyframe) {
    TempAnimAsset asset = WriteTempTranslationAnimGltf();

    GltfScene scene;
    AETHER_CHECK(LoadGltf(asset.gltf_path, scene));

    std::vector<GltfNodeInstance> animated;
    EvaluateAnimation(scene, scene.animations[0], 5.0f, animated); // past duration=1.0
    AETHER_CHECK(animated.size() == 1);
    AETHER_CHECK(std::abs(animated[0].world_transform.cols[3].x - 10.0f) < 1e-4f);

    std::filesystem::remove(asset.gltf_path);
    std::filesystem::remove(asset.bin_path);
}

AETHER_TEST(Gltf_SkinMatricesReduceToIdentityAtRestPoseWithNonIdentityInverseBind) {
    // Joint node's bind-pose translation is (2,0,0); its inverse bind matrix
    // is translate(-2,0,0) — this checks the standard invariant
    // "joint_world_transform * inverse_bind_matrix == identity" at rest
    // pose, with a genuinely non-identity inverse bind (not the degenerate
    // identity-identity case, which would pass even with the matrices
    // multiplied in the wrong order).
    f32 inv_bind[16] = {
        1, 0, 0, 0, //
        0, 1, 0, 0, //
        0, 0, 1, 0, //
        -2, 0, 0, 1,
    };
    std::string bin_path = WriteTempBinary("aether_test_skin.bin", inv_bind, sizeof(inv_bind));
    std::string bin_filename = std::filesystem::path(bin_path).filename().string();

    std::string gltf_json = std::string(R"({
      "asset": {"version": "2.0"},
      "buffers": [ {"uri": ")") +
                             bin_filename + R"(", "byteLength": 64} ],
      "bufferViews": [ {"buffer": 0, "byteOffset": 0, "byteLength": 64} ],
      "accessors": [ {"bufferView": 0, "componentType": 5126, "count": 1, "type": "MAT4"} ],
      "nodes": [ {"translation": [2.0, 0.0, 0.0]} ],
      "skins": [ {"joints": [0], "inverseBindMatrices": 0} ]
    })";
    std::string path = WriteTempGltf("aether_test_skin.gltf", gltf_json.c_str());

    GltfScene scene;
    AETHER_CHECK(LoadGltf(path, scene));
    AETHER_CHECK(scene.skins.size() == 1);
    AETHER_CHECK(scene.skins[0].joints.size() == 1);
    AETHER_CHECK(scene.skins[0].joints[0] == 0);
    AETHER_CHECK(std::abs(scene.skins[0].inverse_bind_matrices[0].cols[3].x - -2.0f) < 1e-5f);

    std::vector<Mat4> skin_matrices;
    ComputeSkinMatrices(scene, nullptr, 0.0f, scene.skins[0], skin_matrices);
    AETHER_CHECK(skin_matrices.size() == 1);
    AETHER_CHECK(std::abs(skin_matrices[0].cols[3].x) < 1e-4f);
    AETHER_CHECK(std::abs(skin_matrices[0].cols[0].x - 1.0f) < 1e-4f);
    AETHER_CHECK(std::abs(skin_matrices[0].cols[1].y - 1.0f) < 1e-4f);
    AETHER_CHECK(std::abs(skin_matrices[0].cols[2].z - 1.0f) < 1e-4f);

    std::filesystem::remove(path);
    std::filesystem::remove(bin_path);
}

AETHER_TEST(Gltf_SkinDefaultsToIdentityInverseBindMatricesWhenOmitted) {
    constexpr const char* kSkinNoInverseBindGltf = R"({
      "asset": {"version": "2.0"},
      "nodes": [ {}, {} ],
      "skins": [ {"joints": [0, 1]} ]
    })";
    std::string path = WriteTempGltf("aether_test_skin_no_inv.gltf", kSkinNoInverseBindGltf);

    GltfScene scene;
    AETHER_CHECK(LoadGltf(path, scene));
    AETHER_CHECK(scene.skins.size() == 1);
    AETHER_CHECK(scene.skins[0].inverse_bind_matrices.size() == 2);
    for (const Mat4& m : scene.skins[0].inverse_bind_matrices) {
        AETHER_CHECK(std::abs(m.cols[0].x - 1.0f) < 1e-5f);
        AETHER_CHECK(std::abs(m.cols[3].x) < 1e-5f);
    }

    std::filesystem::remove(path);
}

AETHER_TEST(Gltf_PrimitiveParsesJointsAndWeightsWhenBothPresent) {
    f32 position[3] = {0.0f, 0.0f, 0.0f};
    u8 joints[4] = {2, 5, 0, 0};
    f32 weights[4] = {0.6f, 0.4f, 0.0f, 0.0f};

    std::vector<u8> blob;
    auto append = [&](const void* p, usize n) {
        const u8* b = static_cast<const u8*>(p);
        blob.insert(blob.end(), b, b + n);
    };
    append(position, sizeof(position));
    append(joints, sizeof(joints));
    append(weights, sizeof(weights));

    std::string bin_path = WriteTempBinary("aether_test_skin_vertex.bin", blob.data(), blob.size());
    std::string bin_filename = std::filesystem::path(bin_path).filename().string();

    std::string gltf_json = std::string(R"({
      "asset": {"version": "2.0"},
      "buffers": [ {"uri": ")") +
                             bin_filename + R"(", "byteLength": )" + std::to_string(blob.size()) + R"(} ],
      "bufferViews": [
        {"buffer": 0, "byteOffset": 0, "byteLength": 12},
        {"buffer": 0, "byteOffset": 12, "byteLength": 4},
        {"buffer": 0, "byteOffset": 16, "byteLength": 16}
      ],
      "accessors": [
        {"bufferView": 0, "componentType": 5126, "count": 1, "type": "VEC3"},
        {"bufferView": 1, "componentType": 5121, "count": 1, "type": "VEC4"},
        {"bufferView": 2, "componentType": 5126, "count": 1, "type": "VEC4"}
      ],
      "meshes": [
        { "primitives": [ {"attributes": {"POSITION": 0, "JOINTS_0": 1, "WEIGHTS_0": 2}, "mode": 4} ] }
      ]
    })";
    std::string path = WriteTempGltf("aether_test_skin_vertex.gltf", gltf_json.c_str());

    GltfScene scene;
    AETHER_CHECK(LoadGltf(path, scene));
    AETHER_CHECK(scene.meshes.size() == 1);
    const GltfPrimitive& prim = scene.meshes[0].primitives[0];
    AETHER_CHECK(prim.joint_indices.size() == 1);
    AETHER_CHECK(prim.joint_weights.size() == 1);
    AETHER_CHECK(prim.joint_indices[0][0] == 2);
    AETHER_CHECK(prim.joint_indices[0][1] == 5);
    AETHER_CHECK(std::abs(prim.joint_weights[0][0] - 0.6f) < 1e-5f);
    AETHER_CHECK(std::abs(prim.joint_weights[0][1] - 0.4f) < 1e-5f);

    std::filesystem::remove(path);
    std::filesystem::remove(bin_path);
}
