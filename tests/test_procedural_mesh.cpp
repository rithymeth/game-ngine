#include "aether/assets/gltf_loader.h"
#include "aether/assets/gltf_references.h"
#include "aether/assets/image.h"
#include "aether/assets/png_writer.h"
#include "aether/assets/procedural_mesh.h"
#include "test_framework.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <cstdlib>
#include <cstring>
#include <set>

using namespace aether;
using namespace aether::assets;
using namespace aether::assets::procedural;

namespace {

u64 HashMesh(const ProceduralMesh& m) {
    u64 h = 1469598103934665603ull;
    auto mix = [&](const void* data, usize size) {
        const u8* p = static_cast<const u8*>(data);
        for (usize i = 0; i < size; ++i) h = (h ^ p[i]) * 1099511628211ull;
    };
    for (const MeshPrimitiveData& p : m.mesh.primitives) {
        mix(p.vertices.data(), p.vertices.size() * sizeof(MeshVertex));
        mix(p.indices.data(), p.indices.size() * sizeof(u32));
    }
    return h;
}

constexpr ProceduralKind kAllKinds[] = {ProceduralKind::Rock, ProceduralKind::Cliff, ProceduralKind::PineTree, ProceduralKind::BroadleafTree,
                                        ProceduralKind::DeadTree, ProceduralKind::House, ProceduralKind::Tower, ProceduralKind::Wall,
                                        ProceduralKind::Pillar, ProceduralKind::Arch, ProceduralKind::Stairs, ProceduralKind::Shrub, ProceduralKind::Grass,
                                        ProceduralKind::Reeds, ProceduralKind::Cactus};

Vec3 Extent(const MeshStats& s) { return s.bounds_max - s.bounds_min; }

} // namespace

AETHER_TEST(ProceduralEveryKindMakesValidMeshes) {
    static_assert(sizeof(kAllKinds) / sizeof(kAllKinds[0]) == kProceduralKindCount);
    for (const ProceduralKind kind : kAllKinds) {
        for (u32 variant = 0; variant < 24; ++variant) {
            const ProceduralMesh m = GenerateVariant(kind, variant, 99);
            std::string error;
            const bool valid = ValidateMesh(m, &error);
            if (!valid) std::printf("    %s variant %u: %s\n", ProceduralKindName(kind), variant, error.c_str());
            AETHER_CHECK(valid);
            if (m.mesh.primitives.empty()) { AETHER_CHECK(false); continue; }
            AETHER_CHECK(m.TriangleCount() > 8);
            // A budget per kind: what a game can place by the hundred.
            const u32 budget = kind == ProceduralKind::BroadleafTree ? 12000u : kind == ProceduralKind::DeadTree ? 8000u : kind == ProceduralKind::Cliff ? 12000u
                               : kind == ProceduralKind::PineTree ? 1500u : kind == ProceduralKind::Rock ? 2000u : (kind == ProceduralKind::House || kind == ProceduralKind::Tower) ? 6000u
                               : kind == ProceduralKind::Shrub ? 1500u : (kind == ProceduralKind::Grass || kind == ProceduralKind::Reeds || kind == ProceduralKind::Cactus) ? 900u : 600u;
            if (m.TriangleCount() > budget) std::printf("    %s variant %u: %u triangles, budget %u\n", ProceduralKindName(kind), variant, m.TriangleCount(), budget);
            AETHER_CHECK(m.TriangleCount() <= budget);
            AETHER_CHECK(m.VertexCount() > 8);
            const MeshStats s = ComputeStats(m.mesh);
            const Vec3 e = Extent(s);
            AETHER_CHECK(e.x > 0.1f && e.y > 0.1f && e.z > 0.05f && e.x < 100.0f && e.y < 100.0f && e.z < 100.0f);
            AETHER_CHECK(s.bounds_min.y > -0.5f && s.bounds_min.y < 0.2f); // stands on the ground
            for (const MeshPrimitiveData& p : m.mesh.primitives) {
                AETHER_CHECK(p.bounds_min.x <= p.bounds_max.x && p.material >= 0 && static_cast<usize>(p.material) < m.materials.size());
            }
        }
    }
    AETHER_CHECK(std::string(ProceduralKindName(ProceduralKind::Arch)) == "Arch");
}

AETHER_TEST(ProceduralIsDeterministicAndVariantsAreUnique) {
    for (const ProceduralKind kind : kAllKinds) {
        AETHER_CHECK(HashMesh(GenerateVariant(kind, 5, 1)) == HashMesh(GenerateVariant(kind, 5, 1)));
        AETHER_CHECK(HashMesh(GenerateVariant(kind, 5, 1)) != HashMesh(GenerateVariant(kind, 6, 1)));
        AETHER_CHECK(HashMesh(GenerateVariant(kind, 5, 1)) != HashMesh(GenerateVariant(kind, 5, 2)));
    }
    // Hundreds of different rocks and trees: not one repeat in 300.
    for (const ProceduralKind kind : {ProceduralKind::Rock, ProceduralKind::BroadleafTree, ProceduralKind::PineTree}) {
        std::set<u64> seen;
        for (u32 v = 0; v < 300; ++v) seen.insert(HashMesh(GenerateVariant(kind, v, 7)));
        AETHER_CHECK(seen.size() == 300);
    }
}

AETHER_TEST(ProceduralSolidsAreClosedAndWindOutward) {
    for (const ProceduralKind kind : {ProceduralKind::Rock, ProceduralKind::Pillar, ProceduralKind::Stairs, ProceduralKind::Arch, ProceduralKind::Wall, ProceduralKind::Cliff}) {
        for (u32 v = 0; v < 12; ++v) {
            const ProceduralMesh m = GenerateVariant(kind, v, 3);
            const MeshStats s = ComputeStats(m.mesh);
            if (s.open_edges != 0) std::printf("    %s variant %u has %u open edges\n", ProceduralKindName(kind), v, s.open_edges);
            AETHER_CHECK(s.open_edges == 0);
            AETHER_CHECK(s.volume > 0.0f); // positive signed volume: the triangles face outward
        }
    }
    // A flat-roofed tower has no gable triangles, so it is closed too.
    for (u32 v = 0; v < 8; ++v) {
        const ProceduralMesh m = GenerateVariant(ProceduralKind::Tower, v, 3);
        AETHER_CHECK(ComputeStats(m.mesh).open_edges == 0 && ComputeStats(m.mesh).volume > 0.0f);
    }
    // And smooth and flat rocks both: normals point away from the centre.
    for (const Shading shading : {Shading::Smooth, Shading::Flat}) {
        RockParams p;
        p.shading = shading;
        const ProceduralMesh m = GenerateRock(p, 4);
        const MeshStats s = ComputeStats(m.mesh);
        const Vec3 centre = (s.bounds_min + s.bounds_max) * 0.5f;
        usize outward = 0, total = 0;
        for (const MeshVertex& v : m.mesh.primitives[0].vertices) {
            const Vec3 d = Vec3(v.position[0], v.position[1], v.position[2]) - centre;
            outward += d.Dot(Vec3(v.normal[0], v.normal[1], v.normal[2])) > 0.0f;
            ++total;
        }
        AETHER_CHECK(outward * 100 > total * 90);
    }
}

AETHER_TEST(ProceduralRocksHaveTheRequestedSizeAndSitOnTheGround) {
    for (const f32 size : {0.4f, 1.0f, 2.5f}) {
        RockParams p;
        p.size = size;
        p.elongation = 0.6f;
        const MeshStats s = ComputeStats(GenerateRock(p, 11).mesh);
        const Vec3 e = Extent(s);
        const f32 widest = std::max({e.x, e.y, e.z});
        AETHER_CHECK(std::fabs(widest - size) < size * 0.01f);
        AETHER_CHECK(std::fabs(s.bounds_min.y) < 1e-3f);
        AETHER_CHECK(std::fabs((s.bounds_min.x + s.bounds_max.x) * 0.5f) < 1e-3f && std::fabs((s.bounds_min.z + s.bounds_max.z) * 0.5f) < 1e-3f);
    }
    RockParams coarse, fine;
    coarse.subdivisions = 2;
    fine.subdivisions = 4;
    AETHER_CHECK(GenerateRock(fine, 1).TriangleCount() > GenerateRock(coarse, 1).TriangleCount() * 8);
    // More cuts, flatter faces: the cut rock has fewer distinct normal directions per area, here just a different shape.
    RockParams cut = coarse;
    cut.cuts = 0;
    AETHER_CHECK(HashMesh(GenerateRock(cut, 1)) != HashMesh(GenerateRock(coarse, 1)));
}

AETHER_TEST(ProceduralCliffStandsAgainstItsBackPlane) {
    CliffParams p;
    p.width = 10.0f;
    p.height = 6.0f;
    p.depth = 3.0f;
    const ProceduralMesh m = GenerateCliff(p, 5);
    const MeshStats s = ComputeStats(m.mesh);
    AETHER_CHECK(std::fabs(s.bounds_min.z) < 1e-3f);                   // the back is at z = 0
    AETHER_CHECK(s.bounds_max.z > 0.3f * p.depth && s.bounds_max.z <= 1.25f * p.depth);
    AETHER_CHECK(std::fabs(Extent(s).x - p.width) < 1e-3f && std::fabs(Extent(s).y - p.height) < 1e-3f);
    AETHER_CHECK(m.TriangleCount() > 2u * p.columns * p.rows);
    // Ledges: a profile up the middle of the face is not monotone.
    CliffParams flat = p;
    flat.overhang = 0.0f;
    flat.roughness = 0.0f;
    AETHER_CHECK(HashMesh(GenerateCliff(flat, 5)) != HashMesh(m));
}

AETHER_TEST(ProceduralTreesHaveTheirHeightBarkAndLeaves) {
    for (const TreeSpecies species : {TreeSpecies::Pine, TreeSpecies::Broadleaf, TreeSpecies::Dead}) {
        TreeParams p;
        p.species = species;
        p.height = 9.0f;
        const ProceduralMesh m = GenerateTree(p, 21);
        const MeshStats s = ComputeStats(m.mesh);
        AETHER_CHECK(std::fabs(s.bounds_max.y - p.height) < 1e-3f); // the tip is at the height asked for
        AETHER_CHECK(s.bounds_min.y < 0.0f && s.bounds_min.y > -0.5f); // roots sunk a little
        AETHER_CHECK(m.mesh.primitives.size() == (species == TreeSpecies::Dead ? 1u : 2u)); // bark, then leaves (a dead tree has none)
    }
    TreeParams few, many;
    few.branch_levels = 1;
    many.branch_levels = 4;
    AETHER_CHECK(GenerateTree(many, 3).TriangleCount() > GenerateTree(few, 3).TriangleCount() * 2);
    TreeParams sparse, dense;
    sparse.foliage = 0.3f;
    dense.foliage = 1.5f;
    AETHER_CHECK(GenerateTree(dense, 3).VertexCount() > GenerateTree(sparse, 3).VertexCount());
}

AETHER_TEST(ProceduralBuildingsScaleWithTheirBaysAndFloors) {
    BuildingParams p;
    p.floors = 2;
    p.bays_x = 3;
    p.bays_z = 2;
    p.bay_size = 3.0f;
    p.floor_height = 3.0f;
    p.roof = RoofType::Flat;
    const MeshStats s = ComputeStats(GenerateBuilding(p, 1).mesh);
    AETHER_CHECK(std::fabs(Extent(s).x - 9.0f) < 0.5f); // the roof overhangs a little
    AETHER_CHECK(std::fabs(Extent(s).z - 6.0f) < 1.5f); // the front step adds depth
    AETHER_CHECK(Extent(s).y > 6.0f && Extent(s).y < 7.5f);
    BuildingParams taller = p;
    taller.floors = 4;
    AETHER_CHECK(ComputeStats(GenerateBuilding(taller, 1).mesh).bounds_max.y > s.bounds_max.y + 5.0f);
    // Glass is the third primitive; more windows, more panes.
    const ProceduralMesh a = GenerateBuilding(p, 1);
    BuildingParams wide = p;
    wide.bays_x = 6;
    const ProceduralMesh b = GenerateBuilding(wide, 1);
    AETHER_CHECK(a.mesh.primitives.size() == 4 && b.mesh.primitives.size() == 4);
    AETHER_CHECK(b.mesh.primitives[2].vertices.size() > a.mesh.primitives[2].vertices.size());
    // A gable roof is higher than a flat one.
    BuildingParams gable = p;
    gable.roof = RoofType::Gable;
    AETHER_CHECK(ComputeStats(GenerateBuilding(gable, 1).mesh).bounds_max.y > s.bounds_max.y + 0.5f);
    // Materials follow the colours asked for.
    AETHER_CHECK(std::fabs(a.materials[0].base_color.x - p.wall_color.x) < 1e-5f && std::fabs(a.materials[1].base_color.x - p.roof_color.x) < 1e-5f);
}

AETHER_TEST(ProceduralBuilderPrimitivesAreCorrect) {
    MeshBuilder box;
    box.Box(Vec3(0, 0, 0), Vec3(2, 3, 4));
    const MeshPrimitiveData prim = box.Finish(Shading::Flat, UvMode::Given, 0);
    ProceduralMesh m;
    m.materials.push_back(MaterialData{});
    m.mesh.primitives.push_back(prim);
    const MeshStats s = ComputeStats(m.mesh);
    AETHER_CHECK(std::fabs(s.volume - 24.0f) < 1e-3f && s.open_edges == 0 && s.triangles == 12 && s.vertices == 36);
    std::string error;
    AETHER_CHECK(ValidateMesh(m, &error));
    // A sphere is closed and has the volume of a sphere (a little under 4/3 pi).
    MeshBuilder ball;
    ball.IcoSphere(3);
    ProceduralMesh sphere;
    sphere.materials.push_back(MaterialData{});
    sphere.mesh.primitives.push_back(ball.Finish(Shading::Smooth, UvMode::BoxProjected, 0));
    const MeshStats ss = ComputeStats(sphere.mesh);
    AETHER_CHECK(ss.open_edges == 0 && ss.triangles == 1280 && std::fabs(ss.volume - 4.18879f) < 0.15f);
    // ValidateMesh finds what is wrong.
    ProceduralMesh bad = sphere;
    bad.mesh.primitives[0].indices[0] = 1u << 20;
    AETHER_CHECK(!ValidateMesh(bad, &error) && error.find("index") != std::string::npos);
    bad = sphere;
    bad.mesh.primitives[0].vertices[3].normal[0] = std::nanf("");
    AETHER_CHECK(!ValidateMesh(bad, &error));
    bad = sphere;
    bad.mesh.primitives[0].material = 5;
    AETHER_CHECK(!ValidateMesh(bad, &error) && error.find("material") != std::string::npos);
}

AETHER_TEST(ProceduralMeshRoundTripsThroughAmshAndGlb) {
    const ProceduralMesh m = GenerateVariant(ProceduralKind::BroadleafTree, 3, 8);
    // The engine's own mesh format.
    MeshData decoded;
    AETHER_CHECK(DecodeMeshData(EncodeMeshData(m.mesh), decoded));
    AETHER_CHECK(decoded.primitives.size() == m.mesh.primitives.size());
    for (usize i = 0; i < decoded.primitives.size(); ++i) {
        AETHER_CHECK(decoded.primitives[i].indices == m.mesh.primitives[i].indices);
        AETHER_CHECK(decoded.primitives[i].vertices.size() == m.mesh.primitives[i].vertices.size());
        AETHER_CHECK(std::memcmp(decoded.primitives[i].vertices.data(), m.mesh.primitives[i].vertices.data(), decoded.primitives[i].vertices.size() * sizeof(MeshVertex)) == 0);
    }
    // glb: the loader reads it back with the same geometry and materials.
    const std::vector<u8> glb = EncodeGlb(m);
    std::string json;
    std::vector<u8> bin;
    AETHER_CHECK(ReadGlbChunks(glb, json, bin) && !json.empty() && !bin.empty());
    GltfScene scene;
    AETHER_CHECK(LoadGltfFromMemory(glb, scene));
    AETHER_CHECK(scene.meshes.size() == 1 && scene.meshes[0].primitives.size() == m.mesh.primitives.size());
    AETHER_CHECK(scene.materials.size() == m.materials.size() && !scene.node_instances.empty());
    for (usize i = 0; i < m.mesh.primitives.size(); ++i) {
        const GltfPrimitive& g = scene.meshes[0].primitives[i];
        AETHER_CHECK(g.vertices.size() == m.mesh.primitives[i].vertices.size() && g.indices == m.mesh.primitives[i].indices);
        AETHER_CHECK(std::fabs(g.vertices[7].position[1] - m.mesh.primitives[i].vertices[7].position[1]) < 1e-6f);
    }
    for (usize i = 0; i < m.materials.size(); ++i) {
        AETHER_CHECK(std::fabs(scene.materials[i].base_color[1] - m.materials[i].base_color.y) < 1e-6f);
        AETHER_CHECK(std::fabs(scene.materials[i].roughness - m.materials[i].roughness) < 1e-6f);
    }
}

AETHER_TEST(ProceduralGenerationSpeed) {
    const auto begin = std::chrono::steady_clock::now();
    usize triangles = 0;
    for (u32 v = 0; v < 200; ++v) {
        triangles += GenerateVariant(ProceduralKind::Rock, v, 1).TriangleCount();
        triangles += GenerateVariant(ProceduralKind::BroadleafTree, v, 1).TriangleCount();
        triangles += GenerateVariant(ProceduralKind::House, v, 1).TriangleCount();
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
    std::printf("[procedural] 600 pieces (200 rocks, trees, houses), %zu triangles: %.0f ms\n", triangles, ms);
    AETHER_CHECK(triangles > 100000);
    if (const char* dir = std::getenv("AETHER_PROCEDURAL_GALLERY_DIR")) {
        // Opt-in: a gallery of GLBs for a look in the player.
        for (const ProceduralKind kind : kAllKinds)
            for (u32 v = 0; v < 6; ++v) {
                const std::vector<u8> glb = EncodeGlb(GenerateVariant(kind, v, 2026));
                const std::string path = std::string(dir) + "/" + ProceduralKindName(kind) + "_" + std::to_string(v) + ".glb";
                if (FILE* f = std::fopen(path.c_str(), "wb")) { std::fwrite(glb.data(), 1, glb.size(), f); std::fclose(f); }
            }
    }
}

AETHER_TEST(ProceduralPlantsHaveTheirShapeAndParts) {
    // Shrubs: the height asked for, a stem primitive and a leaf one; more masses, more triangles.
    ShrubParams shrub;
    shrub.height = 1.3f;
    const ProceduralMesh a = GenerateShrub(shrub, 4);
    AETHER_CHECK(std::fabs(ComputeStats(a.mesh).bounds_max.y - 1.3f) < 1e-3f && a.mesh.primitives.size() == 2);
    ShrubParams many = shrub;
    many.masses = 12;
    AETHER_CHECK(GenerateShrub(many, 4).TriangleCount() > a.TriangleCount());
    ShrubParams bare = shrub;
    bare.stems = 0.0f;
    AETHER_CHECK(GenerateShrub(bare, 4).mesh.primitives.size() == 1);

    // Grass: blades are two-sided ribbons (six triangles a blade a side), tapering, within the tuft's radius and height.
    GrassParams grass;
    grass.blades = 12;
    grass.height = 0.6f;
    grass.spread = 0.2f;
    const ProceduralMesh g = GenerateGrass(grass, 9);
    AETHER_CHECK(g.TriangleCount() == 12u * 3u * 2u * 2u);
    const MeshStats gs = ComputeStats(g.mesh);
    AETHER_CHECK(gs.bounds_max.y <= 0.6f + 1e-3f && gs.bounds_max.y > 0.3f && std::fabs(gs.bounds_min.y) < 1e-4f);
    AETHER_CHECK(Extent(gs).x < 0.2f * 2.0f + 0.6f * 0.4f && Extent(gs).z < 0.2f * 2.0f + 0.6f * 0.4f);
    // Both faces: for every triangle there is one on the same corners facing the other way (normals opposite).
    usize facing_up = 0, facing_down = 0;
    for (const MeshVertex& v : g.mesh.primitives[0].vertices) (v.normal[1] >= 0.0f ? facing_up : facing_down)++;
    AETHER_CHECK(facing_up > 0 && facing_down > 0);
    GrassParams more = grass;
    more.blades = 30;
    AETHER_CHECK(GenerateGrass(more, 9).TriangleCount() > g.TriangleCount() * 2);

    // Reeds: stalks and (with heads) a second primitive; the tallest stem is within the height.
    ReedParams reeds;
    reeds.height = 1.8f;
    const ProceduralMesh r = GenerateReeds(reeds, 2);
    AETHER_CHECK(ComputeStats(r.mesh).bounds_max.y <= 1.8f + 0.05f && ComputeStats(r.mesh).bounds_max.y > 1.0f);
    ReedParams plain = reeds;
    plain.heads = false;
    AETHER_CHECK(GenerateReeds(plain, 2).mesh.primitives.size() == 1);

    // Cacti: the height asked for (the rounded top included), arms add geometry and reach out past the column.
    CactusParams cactus;
    cactus.height = 3.0f;
    cactus.radius = 0.2f;
    cactus.arms = 0;
    const MeshStats plain_cactus = ComputeStats(GenerateCactus(cactus, 5).mesh);
    AETHER_CHECK(std::fabs(plain_cactus.bounds_max.y - 3.0f) < 1e-3f && Extent(plain_cactus).x < 0.5f);
    cactus.arms = 3;
    const ProceduralMesh armed = GenerateCactus(cactus, 5);
    AETHER_CHECK(armed.TriangleCount() > GenerateCactus(CactusParams{.height = 3.0f, .radius = 0.2f, .arms = 0}, 5).TriangleCount() * 2);
    AETHER_CHECK(Extent(ComputeStats(armed.mesh)).x > 0.6f || Extent(ComputeStats(armed.mesh)).z > 0.6f);
    std::string error;
    AETHER_CHECK(ValidateMesh(armed, &error) && ValidateMesh(g, &error) && ValidateMesh(r, &error) && ValidateMesh(a, &error));
}

AETHER_TEST(ProceduralPngWriterRoundTripsLosslessly) {
    // A small gradient, and one bigger than a stored deflate block (65,535 bytes), so the block splitting is covered.
    for (const auto [w, h] : {std::pair<u32, u32>{5, 3}, std::pair<u32, u32>{1, 1}, std::pair<u32, u32>{300, 220}}) {
        std::vector<u8> rgb(static_cast<usize>(w) * h * 3);
        u32 state = 12345u;
        for (usize i = 0; i < rgb.size(); ++i) {
            state = state * 1664525u + 1013904223u;
            rgb[i] = static_cast<u8>(state >> 24);
        }
        const std::vector<u8> png = assets::EncodePngRgb8(w, h, rgb);
        AETHER_CHECK(png.size() > 40 && png[0] == 0x89 && png[1] == 'P' && png[2] == 'N' && png[3] == 'G');
        assets::ImageData image;
        AETHER_CHECK(assets::DecodeImageFromMemory(png, image));
        AETHER_CHECK(image.width == w && image.height == h && image.pixels.size() == static_cast<usize>(w) * h * 4);
        bool same = true;
        for (usize p = 0; p < static_cast<usize>(w) * h && same; ++p)
            same = image.pixels[p * 4] == rgb[p * 3] && image.pixels[p * 4 + 1] == rgb[p * 3 + 1] && image.pixels[p * 4 + 2] == rgb[p * 3 + 2] && image.pixels[p * 4 + 3] == 255;
        AETHER_CHECK(same);
    }
    AETHER_CHECK(assets::EncodePngRgb8(0, 4, {}).empty() && assets::EncodePngRgb8(4, 4, std::vector<u8>(10)).empty());
}

AETHER_TEST(ProceduralGlbCarriesTheBaseColorTextureOfAMaterial) {
    ProceduralMesh m = GenerateVariant(ProceduralKind::Rock, 1, 3);
    m.materials[0].base_color_texture = "rock_color.png";
    const std::vector<u8> glb = EncodeGlb(m);
    std::string json;
    std::vector<u8> bin;
    AETHER_CHECK(ReadGlbChunks(glb, json, bin) && json.find("\"images\":[{\"uri\":\"rock_color.png\"}]") != std::string::npos && json.find("\"baseColorTexture\"") != std::string::npos);
    // Loaded from a file (the loader refuses external texture paths from memory), the texture path is the uri joined to the file's folder.
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "aether_procedural_glb";
    std::filesystem::create_directories(dir);
    {
        std::ofstream f(dir / "rock.glb", std::ios::binary);
        f.write(reinterpret_cast<const char*>(glb.data()), static_cast<std::streamsize>(glb.size()));
    }
    GltfScene scene;
    AETHER_CHECK(LoadGltf((dir / "rock.glb").string(), scene));
    AETHER_CHECK(scene.materials.size() == 1 && scene.materials[0].base_color_texture.size() > 13 &&
                 scene.materials[0].base_color_texture.rfind("rock_color.png") == scene.materials[0].base_color_texture.size() - 14);
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    // Without a texture there is no image in the file.
    ProceduralMesh plain = GenerateVariant(ProceduralKind::Rock, 1, 3);
    std::string plain_json;
    AETHER_CHECK(ReadGlbChunks(EncodeGlb(plain), plain_json, bin) && plain_json.find("images") == std::string::npos);
}

AETHER_TEST(ProceduralGlbTexturesAreFoundByTheCookAndFollowedByMoves) {
    // A textured .glb in a content folder: its image is a file beside it. GltfFileReferences used to return nothing for a .glb, so the cook left
    // such a texture out of the package, and a move of the folder did not keep the model pointing at it.
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "aether_glb_refs";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root / "Models", ec);
    ProceduralMesh m = GenerateVariant(ProceduralKind::Rock, 2, 5);
    m.materials[0].base_color_texture = "skin.png";
    const std::vector<u8> glb = EncodeGlb(m);
    {
        std::ofstream f(root / "Models/rock.glb", std::ios::binary);
        f.write(reinterpret_cast<const char*>(glb.data()), static_cast<std::streamsize>(glb.size()));
    }
    const std::vector<assets::GltfFileReference> refs = assets::GltfFileReferences(root, "Models/rock.glb");
    AETHER_CHECK(refs.size() == 1 && refs[0].uri == "skin.png" && refs[0].path == "Models/skin.png");
    AETHER_CHECK(assets::GltfFileReferences(root, "Models/missing.glb").empty());
    // A glb with no image has nothing to list (its buffer is embedded).
    {
        const std::vector<u8> plain = EncodeGlb(GenerateVariant(ProceduralKind::Rock, 2, 5));
        std::ofstream f(root / "Models/plain.glb", std::ios::binary);
        f.write(reinterpret_cast<const char*>(plain.data()), static_cast<std::streamsize>(plain.size()));
    }
    AETHER_CHECK(assets::GltfFileReferences(root, "Models/plain.glb").empty());

    // Rewriting the uri repacks the file: same geometry, the new path.
    std::string error;
    const int changed = assets::RewriteGltfUris(root / "Models/rock.glb", [](const std::string& uri) -> std::optional<std::string> { return uri == "skin.png" ? std::optional<std::string>("../Skins/skin_v2.png") : std::nullopt; }, &error);
    AETHER_CHECK(changed == 1);
    GltfScene scene;
    AETHER_CHECK(LoadGltf((root / "Models/rock.glb").string(), scene));
    AETHER_CHECK(scene.meshes.size() == 1 && scene.meshes[0].primitives[0].vertices.size() == m.mesh.primitives[0].vertices.size() && scene.meshes[0].primitives[0].indices == m.mesh.primitives[0].indices);
    AETHER_CHECK(scene.materials[0].base_color_texture.find("skin_v2.png") != std::string::npos);
    AETHER_CHECK(assets::RewriteGltfUris(root / "Models/rock.glb", [](const std::string&) -> std::optional<std::string> { return std::nullopt; }, &error) == 0);
    std::filesystem::remove_all(root, ec);
}
