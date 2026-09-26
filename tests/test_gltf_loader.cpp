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
