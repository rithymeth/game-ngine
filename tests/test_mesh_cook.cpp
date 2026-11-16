#include "test_framework.h"

#include "aether/cook/mesh_cook.h"
#include "aether/pak/pak.h"

#include <cmath>
#include <cstdio>

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

// Two patches ten units apart, for HLOD clustering.
assets::MeshData MakeTwoPrimitives() {
    assets::MeshData mesh = MakeGridMesh(4);
    assets::MeshPrimitiveData second = mesh.primitives[0];
    for (assets::MeshVertex& v : second.vertices) v.position[0] += 10.0f;
    second.bounds_min.x += 10.0f;
    second.bounds_max.x += 10.0f;
    mesh.primitives.push_back(std::move(second));
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

// Phase 42.4: cooked mesh v2 carries an HLOD proxy table and round-trips it.
AETHER_TEST(MeshCook_HlodProxiesRoundTrip) {
    MeshCookSettings settings;
    settings.build_hlod = true;
    settings.hlod_cluster_radius = 1000.0f; // one cluster over both patches
    CookedMesh cooked;
    std::string error;
    AETHER_CHECK(CookMesh(MakeTwoPrimitives(), settings, cooked, &error));
    AETHER_CHECK(cooked.primitives.size() == 2);
    AETHER_CHECK(cooked.hlod.size() == 1);
    AETHER_CHECK(cooked.hlod[0].sources.size() == 2);
    AETHER_CHECK(!cooked.hlod[0].vertices.empty());
    AETHER_CHECK(!cooked.hlod[0].lods.empty());
    AETHER_CHECK(!cooked.hlod[0].lods[0].meshlets.meshlets.empty()); // the proxy is drawable as meshlets

    const std::vector<u8> bytes = SaveAmesh(cooked);
    CookedMesh loaded;
    AETHER_CHECK(LoadAmesh(bytes, loaded, &error));
    AETHER_CHECK(loaded.hlod.size() == 1);
    AETHER_CHECK((loaded.hlod[0].sources == cooked.hlod[0].sources));
    AETHER_CHECK(loaded.hlod[0].lods[0].meshlets.meshlets.size() == cooked.hlod[0].lods[0].meshlets.meshlets.size());
    AETHER_CHECK(SaveAmesh(loaded) == bytes); // deterministic round trip

    // Without build_hlod there is no proxy table.
    CookedMesh plain;
    AETHER_CHECK(CookMesh(MakeTwoPrimitives(), MeshCookSettings{}, plain, &error));
    AETHER_CHECK(plain.hlod.empty());
}

// Phase 42.4/48.2: a version-1 cooked mesh (no HLOD table) still loads.
AETHER_TEST(MeshCook_LoadsVersionOneBytes) {
    const char magic[4] = {'A', 'M', 'S', 'C'};
    const auto put32 = [](std::vector<u8>& out, u32 v) {
        for (int i = 0; i < 4; ++i) out.push_back(static_cast<u8>(v >> (8 * i)));
    };
    std::vector<u8> bytes;
    bytes.insert(bytes.end(), magic, magic + 4);
    put32(bytes, 1); // version 1: no HLOD table
    put32(bytes, 0); // no primitives
    const std::vector<u8> body = bytes; // everything before the CRC
    put32(bytes, pak::Crc32(body));
    CookedMesh out;
    AETHER_CHECK(LoadAmesh(bytes, out));
    AETHER_CHECK(out.primitives.empty());
    AETHER_CHECK(out.hlod.empty());
}

// ---- virtual geometry stage 3: the cluster hierarchy in the cooked mesh (v3) ----

namespace {

CookedMesh CookWithClusters() {
    MeshCookSettings settings;
    settings.build_clusters = true;
    CookedMesh cooked;
    std::string error;
    AETHER_CHECK(CookMesh(MakeGridMesh(24), settings, cooked, &error));
    return cooked;
}

// Rewrites the bytes' version word and CRC, after the caller changed what precedes the CRC.
void Reseal(std::vector<u8>& bytes, u32 version) {
    for (int i = 0; i < 4; ++i) bytes[4 + i] = static_cast<u8>(version >> (8 * i));
    const std::vector<u8> body(bytes.begin(), bytes.end() - 4);
    const u32 crc = pak::Crc32(body);
    for (int i = 0; i < 4; ++i) bytes[bytes.size() - 4 + i] = static_cast<u8>(crc >> (8 * i));
}

} // namespace

AETHER_TEST(MeshCook_ClustersRoundTripAndAreDeterministic) {
    const CookedMesh cooked = CookWithClusters();
    AETHER_CHECK(cooked.primitives.size() == 1 && !cooked.primitives[0].clusters.clusters.empty());
    if (cooked.primitives.empty() || cooked.primitives[0].clusters.clusters.empty()) return;
    const ClusterHierarchy& h = cooked.primitives[0].clusters;
    AETHER_CHECK(h.leaf_triangles() == cooked.primitives[0].lods[0].indices.size() / 3); // the leaves are the mesh, exactly
    const std::vector<u8> bytes = SaveAmesh(cooked);
    CookedMesh loaded;
    std::string error;
    AETHER_CHECK(LoadAmesh(bytes, loaded, &error));
    AETHER_CHECK(loaded.primitives.size() == 1);
    if (loaded.primitives.size() != 1) return;
    const ClusterHierarchy& l = loaded.primitives[0].clusters;
    AETHER_CHECK(l.clusters.size() == h.clusters.size() && l.groups.size() == h.groups.size() && l.indices == h.indices);
    AETHER_CHECK(l.level_count == h.level_count && l.leaf_count == h.leaf_count && l.root_count() == h.root_count());
    AETHER_CHECK(SaveAmesh(loaded) == bytes);
    // The loaded hierarchy selects the same clusters as the one that was cooked.
    ClusterView view;
    view.position = Vec3(12.0f, 6.0f, 40.0f);
    ClusterSelection a, b;
    AETHER_CHECK(SelectClusters(h, view, 2.0f, a) && SelectClusters(l, view, 2.0f, b));
    AETHER_CHECK(a.clusters == b.clusters && a.triangles == b.triangles && !a.clusters.empty());
}

AETHER_TEST(MeshCook_ClustersAreOptInAndOlderVersionsStillLoad) {
    CookedMesh plain;
    std::string error;
    AETHER_CHECK(CookMesh(MakeGridMesh(8), MeshCookSettings{}, plain, &error));
    AETHER_CHECK(plain.primitives.size() == 1 && plain.primitives[0].clusters.clusters.empty());
    // A version-2 file is the v3 bytes without the trailing cluster count.
    std::vector<u8> bytes = SaveAmesh(plain);
    bytes.erase(bytes.end() - 8, bytes.end() - 4);
    Reseal(bytes, 2);
    CookedMesh loaded;
    AETHER_CHECK(LoadAmesh(bytes, loaded, &error));
    AETHER_CHECK(loaded.primitives.size() == 1 && loaded.primitives[0].clusters.clusters.empty());
    // A v3 file whose table claims a cluster the file does not hold is refused, not misread.
    std::vector<u8> short_file = SaveAmesh(plain);
    short_file[short_file.size() - 8] = 1;
    Reseal(short_file, 3);
    AETHER_CHECK(!LoadAmesh(short_file, loaded, &error));
}

AETHER_TEST(MeshCook_RefusesClusterHierarchiesThatPointOutsideThemselves) {
    const CookedMesh good = CookWithClusters();
    if (good.primitives.empty() || good.primitives[0].clusters.groups.empty()) return;
    const auto refused = [&](const char* what, auto&& tamper) {
        CookedMesh bad = good;
        tamper(bad.primitives[0].clusters);
        const std::vector<u8> bytes = SaveAmesh(bad); // valid CRC: only the content is wrong
        CookedMesh out;
        std::string error;
        const bool loaded = LoadAmesh(bytes, out, &error);
        if (loaded) std::printf("    cluster tamper \"%s\" was accepted\n", what);
        AETHER_CHECK(!loaded);
    };
    const u32 vertex_count = static_cast<u32>(good.primitives[0].vertices.size());
    refused("index past the vertices", [&](ClusterHierarchy& h) { h.indices[0] = vertex_count; });
    refused("index count not a multiple of 3", [](ClusterHierarchy& h) { h.indices.pop_back(); });
    refused("triangles past the indices", [](ClusterHierarchy& h) { h.clusters.back().triangle_count += 1000; });
    refused("unaligned triangle offset", [](ClusterHierarchy& h) { h.clusters[0].index_offset = 1; });
    refused("level past level_count", [](ClusterHierarchy& h) { h.clusters[0].level = h.level_count; });
    refused("leaf_count past the clusters", [](ClusterHierarchy& h) { h.leaf_count = static_cast<u32>(h.clusters.size()) + 1; });
    refused("group reference past the groups", [](ClusterHierarchy& h) { h.clusters[0].group = static_cast<i32>(h.groups.size()); });
    refused("group reference below -1", [](ClusterHierarchy& h) { h.clusters[0].source_group = -2; });
    refused("group member past the clusters", [](ClusterHierarchy& h) { h.groups[0].inputs.push_back(static_cast<u32>(h.clusters.size())); });
    refused("NaN radius", [](ClusterHierarchy& h) { h.clusters[0].radius = std::nanf(""); });
    refused("negative error", [](ClusterHierarchy& h) { h.clusters[0].error = -1.0f; });
    refused("infinite parent sphere", [](ClusterHierarchy& h) { h.clusters[0].parent_radius = INFINITY; });
    refused("negative extent", [](ClusterHierarchy& h) { h.mesh_extent = -1.0f; });
    refused("absurd level count", [](ClusterHierarchy& h) { h.level_count = 100000; });
}
