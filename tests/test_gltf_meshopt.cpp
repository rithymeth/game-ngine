#include "aether/assets/gltf_loader.h"
#include "test_framework.h"

#include <meshoptimizer.h>

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

using namespace aether;
using namespace aether::assets;

namespace {

void PutU32(std::vector<u8>& out, u32 v) {
    const u8* b = reinterpret_cast<const u8*>(&v);
    out.insert(out.end(), b, b + 4);
}

std::vector<u8> MakeGlb(std::string json, std::vector<u8> bin) {
    while (json.size() % 4 != 0) json.push_back(' ');
    while (bin.size() % 4 != 0) bin.push_back(0);
    std::vector<u8> out;
    PutU32(out, 0x46546C67);
    PutU32(out, 2);
    PutU32(out, 0);
    PutU32(out, static_cast<u32>(json.size()));
    PutU32(out, 0x4E4F534A);
    out.insert(out.end(), json.begin(), json.end());
    PutU32(out, static_cast<u32>(bin.size()));
    PutU32(out, 0x004E4942);
    out.insert(out.end(), bin.begin(), bin.end());
    const u32 length = static_cast<u32>(out.size());
    std::memcpy(out.data() + 8, &length, 4);
    return out;
}

std::vector<u8> CompressVertices(const void* vertices, usize count, usize stride) {
    const usize bound = meshopt_encodeVertexBufferBound(count, stride);
    std::vector<u8> encoded(bound);
    const usize size = meshopt_encodeVertexBuffer(encoded.data(), encoded.size(), vertices, count, stride);
    encoded.resize(size);
    return encoded;
}

bool Near(f32 a, f32 b, f32 tol) { return std::fabs(a - b) <= tol; }

} // namespace

// EXPONENTIAL writes real floats (8-bit exponent, 24-bit mantissa). Positions come back as FLOAT.
AETHER_TEST(GltfMeshopt_ExponentialPositionsRoundTrip) {
    const f32 src[9] = {0.0f, 0.0f, 0.0f, 1.5f, -2.0f, 0.25f, 0.0f, 4.0f, -1.0f};
    u8 filtered[3 * 12];
    meshopt_encodeFilterExp(filtered, 3, 12, 15, src, meshopt_EncodeExpSharedVector);
    const std::vector<u8> bin = CompressVertices(filtered, 3, 12);
    const std::string json =
        std::string(R"({"asset":{"version":"2.0"},"extensionsRequired":["EXT_meshopt_compression"],"buffers":[{"byteLength":)") +
        std::to_string(bin.size()) +
        R"(}],"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":)" + std::to_string(bin.size()) +
        R"(,"byteStride":12,"extensions":{"EXT_meshopt_compression":{"buffer":0,"byteOffset":0,"byteLength":)" +
        std::to_string(bin.size()) +
        R"(,"byteStride":12,"count":3,"mode":"ATTRIBUTES","filter":"EXPONENTIAL"}}}],"accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"}],"meshes":[{"primitives":[{"attributes":{"POSITION":0}}]}],"nodes":[{"mesh":0}],"scenes":[{"nodes":[0]}],"scene":0})";

    GltfScene scene;
    std::string error;
    AETHER_CHECK(LoadGltfFromMemory(MakeGlb(json, bin), scene, &error));
    AETHER_CHECK(scene.meshes.size() == 1 && scene.meshes[0].primitives.size() == 1);
    const GltfPrimitive& prim = scene.meshes[0].primitives[0];
    AETHER_CHECK(prim.vertices.size() == 3);
    for (int i = 0; i < 3; ++i) {
        AETHER_CHECK(Near(prim.vertices[static_cast<usize>(i)].position[0], src[i * 3 + 0], 0.02f));
        AETHER_CHECK(Near(prim.vertices[static_cast<usize>(i)].position[1], src[i * 3 + 1], 0.02f));
        AETHER_CHECK(Near(prim.vertices[static_cast<usize>(i)].position[2], src[i * 3 + 2], 0.02f));
    }
}

// gltfpack writes octahedral normals as VEC3 normalized bytes in a stride-4 slot
// (16-bit uses stride 8). The fourth component is padding. The loader must rebuild a unit vector.
AETHER_TEST(GltfMeshopt_OctahedralNormalsRoundTrip) {
    const f32 src[12] = {0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f};
    u8 filtered[3 * 4];
    meshopt_encodeFilterOct(filtered, 3, 4, 8, src);
    const std::vector<u8> bin = CompressVertices(filtered, 3, 4);
    const std::string json =
        std::string(R"({"asset":{"version":"2.0"},"extensionsRequired":["EXT_meshopt_compression"],"buffers":[{"byteLength":)") +
        std::to_string(bin.size()) +
        R"(}],"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":)" + std::to_string(bin.size()) +
        R"(,"byteStride":4,"extensions":{"EXT_meshopt_compression":{"buffer":0,"byteOffset":0,"byteLength":)" +
        std::to_string(bin.size()) +
        R"(,"byteStride":4,"count":3,"mode":"ATTRIBUTES","filter":"OCTAHEDRAL"}}}],"accessors":[{"bufferView":0,"componentType":5120,"normalized":true,"count":3,"type":"VEC3"},{"bufferView":0,"componentType":5120,"normalized":true,"count":3,"type":"VEC3"}],"meshes":[{"primitives":[{"attributes":{"POSITION":0,"NORMAL":1}}]}],"nodes":[{"mesh":0}],"scenes":[{"nodes":[0]}],"scene":0})";

    GltfScene scene;
    std::string error;
    AETHER_CHECK(LoadGltfFromMemory(MakeGlb(json, bin), scene, &error));
    AETHER_CHECK(scene.meshes.size() == 1);
    const GltfPrimitive& prim = scene.meshes[0].primitives[0];
    AETHER_CHECK(prim.vertices.size() == 3);
    auto unit = [](const f32 n[3]) {
        const f32 len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        return std::fabs(len - 1.0f) < 1e-4f;
    };
    AETHER_CHECK(unit(prim.vertices[0].normal));
    AETHER_CHECK(unit(prim.vertices[1].normal));
    AETHER_CHECK(unit(prim.vertices[2].normal));
    AETHER_CHECK(Near(prim.vertices[0].normal[2], 1.0f, 0.05f));
    AETHER_CHECK(Near(prim.vertices[1].normal[1], 1.0f, 0.05f));
    AETHER_CHECK(Near(prim.vertices[2].normal[0], 1.0f, 0.05f));
}

// A filter on an index buffer is not a meshopt mode. The load fails instead of decoding garbage.
AETHER_TEST(GltfMeshopt_FilterOnIndicesIsRejected) {
    const u8 dummy[4] = {0, 1, 2, 3};
    const std::vector<u8> bin(dummy, dummy + 4);
    const char* json =
        R"({"asset":{"version":"2.0"},"buffers":[{"byteLength":4}],"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":4,"extensions":{"EXT_meshopt_compression":{"buffer":0,"byteLength":4,"byteStride":2,"count":2,"mode":"INDICES","filter":"OCTAHEDRAL"}}}],"accessors":[{"bufferView":0,"componentType":5123,"count":2,"type":"SCALAR"}],"meshes":[{"primitives":[{"attributes":{"POSITION":0}}]}]})";
    GltfScene scene;
    std::string error;
    AETHER_CHECK(!LoadGltfFromMemory(MakeGlb(json, bin), scene, &error));
}
