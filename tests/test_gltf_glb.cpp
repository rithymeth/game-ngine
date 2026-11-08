#include "aether/assets/gltf_loader.h"
#include "test_framework.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace aether;
using namespace aether::assets;

// Binary glTF (.glb): the loader reads the JSON chunk and takes buffer 0 from the BIN chunk. Before this, a .glb was refused outright
// ("Buffer with no uri (GLB-embedded binary chunk) is not supported"), so every exported binary model failed to load.

namespace {

// One triangle: positions (0,0,0) (1,0,0) (0,1,0), normals +Z, uvs, indices 0 1 2 as uint16: 102 bytes.
std::vector<u8> TriangleBin() {
    const f32 positions[9] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    const f32 normals[9] = {0, 0, 1, 0, 0, 1, 0, 0, 1};
    const f32 uvs[6] = {0, 0, 1, 0, 0, 1};
    const u16 indices[3] = {0, 1, 2};
    std::vector<u8> bin(sizeof(positions) + sizeof(normals) + sizeof(uvs) + sizeof(indices));
    u8* p = bin.data();
    std::memcpy(p, positions, sizeof(positions)), p += sizeof(positions);
    std::memcpy(p, normals, sizeof(normals)), p += sizeof(normals);
    std::memcpy(p, uvs, sizeof(uvs)), p += sizeof(uvs);
    std::memcpy(p, indices, sizeof(indices));
    return bin;
}

std::string TriangleJson(bool second_buffer_without_uri = false) {
    std::string buffers = R"({"byteLength": 102})";
    if (second_buffer_without_uri) buffers += R"(, {"byteLength": 4})";
    return R"({"asset": {"version": "2.0"}, "buffers": [)" + buffers + R"(],
  "bufferViews": [
    {"buffer": 0, "byteOffset": 0, "byteLength": 36}, {"buffer": 0, "byteOffset": 36, "byteLength": 36},
    {"buffer": 0, "byteOffset": 72, "byteLength": 24}, {"buffer": 0, "byteOffset": 96, "byteLength": 6}],
  "accessors": [
    {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3"},
    {"bufferView": 1, "componentType": 5126, "count": 3, "type": "VEC3"},
    {"bufferView": 2, "componentType": 5126, "count": 3, "type": "VEC2"},
    {"bufferView": 3, "componentType": 5123, "count": 3, "type": "SCALAR"}],
  "materials": [{"pbrMetallicRoughness": {"baseColorFactor": [0.8, 0.2, 0.1, 1.0]}}],
  "meshes": [{"primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2}, "indices": 3, "material": 0}]}],
  "nodes": [{"mesh": 0}], "scenes": [{"nodes": [0]}], "scene": 0})";
}

void PutU32(std::vector<u8>& out, u32 v) {
    const u8* b = reinterpret_cast<const u8*>(&v);
    out.insert(out.end(), b, b + 4);
}

// A GLB as the spec lays it out: the JSON chunk padded with spaces, the BIN chunk with zeros, both to 4 bytes.
std::vector<u8> MakeGlb(std::string json, std::vector<u8> bin, bool with_bin = true) {
    while (json.size() % 4 != 0) json.push_back(' ');
    while (bin.size() % 4 != 0) bin.push_back(0);
    std::vector<u8> out;
    PutU32(out, 0x46546C67);
    PutU32(out, 2);
    PutU32(out, 0); // total length, patched below
    PutU32(out, static_cast<u32>(json.size()));
    PutU32(out, 0x4E4F534A);
    out.insert(out.end(), json.begin(), json.end());
    if (with_bin) {
        PutU32(out, static_cast<u32>(bin.size()));
        PutU32(out, 0x004E4942);
        out.insert(out.end(), bin.begin(), bin.end());
    }
    const u32 length = static_cast<u32>(out.size());
    std::memcpy(out.data() + 8, &length, 4);
    return out;
}

bool IsTheTriangle(const GltfScene& scene) {
    if (scene.meshes.size() != 1 || scene.meshes[0].primitives.size() != 1) return false;
    const GltfPrimitive& p = scene.meshes[0].primitives[0];
    if (p.vertices.size() != 3 || p.indices.size() != 3) return false;
    return p.vertices[1].position[0] == 1.0f && p.vertices[2].position[1] == 1.0f && p.vertices[0].normal[2] == 1.0f &&
           p.indices[2] == 2 && scene.materials.size() == 1 && scene.materials[0].base_color[0] == 0.8f;
}

std::filesystem::path WriteTemp(const char* name, const std::vector<u8>& bytes) {
    const std::filesystem::path path = std::filesystem::temp_directory_path() / name;
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return path;
}

} // namespace

AETHER_TEST(GltfGlb_LoadsFromAFileWithItsBinChunk) {
    const std::filesystem::path path = WriteTemp("aether_triangle.glb", MakeGlb(TriangleJson(), TriangleBin()));
    GltfScene scene;
    std::string error;
    AETHER_CHECK(LoadGltf(path.string(), scene, &error));
    AETHER_CHECK(IsTheTriangle(scene));
    std::filesystem::remove(path);
}

AETHER_TEST(GltfGlb_LoadsFromMemory) {
    const std::vector<u8> glb = MakeGlb(TriangleJson(), TriangleBin());
    GltfScene scene;
    AETHER_CHECK(LoadGltfFromMemory(glb, scene));
    AETHER_CHECK(IsTheTriangle(scene));
}

AETHER_TEST(GltfGlb_ReadGlbChunksSplitsJsonAndBin) {
    const std::vector<u8> glb = MakeGlb(TriangleJson(), TriangleBin());
    std::string json;
    std::vector<u8> bin;
    AETHER_CHECK(ReadGlbChunks(glb, json, bin));
    AETHER_CHECK(json.rfind(R"({"asset")", 0) == 0);
    AETHER_CHECK(bin.size() == 104); // 102 bytes padded to 4
    // Not a GLB at all: refused.
    const std::string text = TriangleJson();
    AETHER_CHECK(!ReadGlbChunks(std::span<const u8>(reinterpret_cast<const u8*>(text.data()), text.size()), json, bin));
}

AETHER_TEST(GltfGlb_MalformedContainersAreRefused) {
    GltfScene scene;
    std::vector<u8> glb = MakeGlb(TriangleJson(), TriangleBin());
    {
        std::vector<u8> bad = glb;
        const u32 version = 1;
        std::memcpy(bad.data() + 4, &version, 4);
        AETHER_CHECK(!LoadGltfFromMemory(bad, scene));
    }
    {
        std::vector<u8> bad = glb;
        const u32 length = static_cast<u32>(bad.size() + 100); // claims more than there is
        std::memcpy(bad.data() + 8, &length, 4);
        AETHER_CHECK(!LoadGltfFromMemory(bad, scene));
    }
    {
        std::vector<u8> bad = glb;
        const u32 chunk = 0x7fffffff; // the JSON chunk runs past the end
        std::memcpy(bad.data() + 12, &chunk, 4);
        AETHER_CHECK(!LoadGltfFromMemory(bad, scene));
    }
    {
        std::vector<u8> truncated(glb.begin(), glb.begin() + 16);
        AETHER_CHECK(!LoadGltfFromMemory(truncated, scene));
    }
    // A buffer with no uri needs the BIN chunk: one without it, and a second uri-less buffer (only buffer 0 may be the chunk), fail.
    AETHER_CHECK(!LoadGltfFromMemory(MakeGlb(TriangleJson(), TriangleBin(), /*with_bin=*/false), scene));
    AETHER_CHECK(!LoadGltfFromMemory(MakeGlb(TriangleJson(true), TriangleBin()), scene));
    // A BIN chunk shorter than the buffer's byteLength fails.
    std::vector<u8> short_bin = TriangleBin();
    short_bin.resize(64);
    AETHER_CHECK(!LoadGltfFromMemory(MakeGlb(TriangleJson(), short_bin), scene));
}

AETHER_TEST(GltfGlb_TextGltfStillLoadsAndRejectsAUrilessBuffer) {
    // A plain .gltf with a uri-less buffer has no BIN chunk to give it: refused, as before.
    const std::string text = TriangleJson();
    GltfScene scene;
    AETHER_CHECK(!LoadGltfFromMemory(std::span<const u8>(reinterpret_cast<const u8*>(text.data()), text.size()), scene));
}
