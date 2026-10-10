#include "test_framework.h"

#include "aether/cook/mesh_cook.h"

#include <cmath>

// Cooked meshes (Phase 39 step 6a): round trip, determinism, LOD shape, and a reader that survives bad bytes.

using namespace aether;
using namespace aether::cook;

namespace {

assets::MeshData MakeGridMesh(u32 n) {
    assets::MeshData mesh;
    assets::MeshPrimitiveData prim;
    for (u32 z = 0; z <= n; ++z) {
        for (u32 x = 0; x <= n; ++x) {
            assets::MeshVertex v{};
            v.position[0] = static_cast<f32>(x);
            v.position[1] = 0.3f * std::sin(static_cast<f32>(x) * 0.5f) * std::cos(static_cast<f32>(z) * 0.5f);
            v.position[2] = static_cast<f32>(z);
            v.normal[1] = 1.0f;
            v.uv[0] = static_cast<f32>(x) / static_cast<f32>(n);
            v.uv[1] = static_cast<f32>(z) / static_cast<f32>(n);
            prim.vertices.push_back(v);
        }
    }
    for (u32 z = 0; z < n; ++z) {
        for (u32 x = 0; x < n; ++x) {
            const u32 a = z * (n + 1) + x, b = a + 1, c = a + (n + 1), d = c + 1;
            for (u32 i : {a, c, b, b, c, d}) prim.indices.push_back(i);
        }
    }
    prim.material = 2;
    prim.bounds_max = Vec3(static_cast<f32>(n), 0.3f, static_cast<f32>(n));
    mesh.primitives.push_back(std::move(prim));
    return mesh;
}

} // namespace

AETHER_TEST(MeshCook_RoundTripsAndIsDeterministic) {
    const assets::MeshData src = MakeGridMesh(24);
    CookedMesh a, b;
    std::string error;
    AETHER_CHECK(CookMesh(src, MeshCookSettings{}, a, &error));
    AETHER_CHECK(CookMesh(src, MeshCookSettings{}, b, &error));
    const std::vector<u8> bytes = SaveAmesh(a);
    AETHER_CHECK(bytes == SaveAmesh(b));

    CookedMesh loaded;
    AETHER_CHECK(LoadAmesh(bytes, loaded, &error));
    AETHER_CHECK(SaveAmesh(loaded) == bytes);
    AETHER_CHECK(loaded.primitives.size() == 1);
    const CookedMeshPrimitive& p = loaded.primitives[0];
    AETHER_CHECK(p.material == 2);
    AETHER_CHECK(p.vertices.size() == src.primitives[0].vertices.size());
    AETHER_CHECK(p.lods.size() == 4);
    AETHER_CHECK(p.lods[0].indices == src.primitives[0].indices);
    AETHER_CHECK(!p.lods[0].meshlets.meshlets.empty());
}

AETHER_TEST(MeshCook_LodsShrinkAndMeshletsCoverEveryTriangle) {
    CookedMesh cooked;
    AETHER_CHECK(CookMesh(MakeGridMesh(32), MeshCookSettings{}, cooked));
    const CookedMeshPrimitive& p = cooked.primitives[0];
    usize previous = ~usize(0);
    for (const CookedMeshLod& lod : p.lods) {
        AETHER_CHECK(lod.indices.size() <= previous);
        previous = lod.indices.size();
        usize covered = 0;
        for (const Meshlet& m : lod.meshlets.meshlets) covered += m.triangle_count;
        AETHER_CHECK(covered == lod.indices.size() / 3);
    }
}

AETHER_TEST(MeshCook_ReaderRejectsDamagedBytes) {
    CookedMesh cooked;
    AETHER_CHECK(CookMesh(MakeGridMesh(8), MeshCookSettings{}, cooked));
    const std::vector<u8> bytes = SaveAmesh(cooked);
    CookedMesh out;
    for (usize len = 0; len < bytes.size(); len += 7) {
        AETHER_CHECK(!LoadAmesh(std::span<const u8>(bytes.data(), len), out));
    }
    std::vector<u8> bad = bytes;
    bad[bad.size() / 2] ^= 0x5A; // the checksum catches it
    AETHER_CHECK(!LoadAmesh(bad, out));
    bad = bytes;
    bad[0] = 'X';
    AETHER_CHECK(!LoadAmesh(bad, out));
    AETHER_CHECK(LoadAmesh(bytes, out));
}

AETHER_TEST(MeshCook_HandlesEmptySkinnedAndBadPrimitives) {
    assets::MeshData mesh;
    mesh.primitives.emplace_back(); // empty
    assets::MeshPrimitiveData skinned = MakeGridMesh(4).primitives[0];
    skinned.joints.assign(skinned.vertices.size(), std::array<u16, 4>{0, 0, 0, 0});
    skinned.weights.assign(skinned.vertices.size(), std::array<f32, 4>{1, 0, 0, 0});
    mesh.primitives.push_back(skinned);
    CookedMesh cooked;
    AETHER_CHECK(CookMesh(mesh, MeshCookSettings{}, cooked));
    AETHER_CHECK(cooked.primitives.size() == 2);
    AETHER_CHECK(cooked.primitives[1].lods.size() == 1);
    AETHER_CHECK(cooked.primitives[1].lods[0].meshlets.meshlets.empty());
    AETHER_CHECK(cooked.warnings.size() == 1);
    CookedMesh again;
    AETHER_CHECK(LoadAmesh(SaveAmesh(cooked), again));

    assets::MeshData broken = MakeGridMesh(4);
    broken.primitives[0].indices.back() = 9999;
    std::string error;
    AETHER_CHECK(!CookMesh(broken, MeshCookSettings{}, cooked, &error));
    AETHER_CHECK(!error.empty());
}
