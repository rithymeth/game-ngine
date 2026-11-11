#include "aether/job/job_system.h"
#include "aether/physics/character.h"
#include "aether/physics/components.h"
#include "aether/scene/components.h"
#include "aether/scene/serialization.h"
#include "test_framework.h"
#include "world/procedural_document.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>

using namespace aether;
using namespace aether::editor;
using namespace aether::terrain;

// A character on a baked procedural world: the ground's collision mesh is the drawn ground's triangles, so a capsule dropped on it stands where the surface is
// seen and walks over its hills without falling through.

namespace {

constexpr f32 kDt = 1.0f / 60.0f;

ProceduralWorldSettings Hills() {
    ProceduralWorldSettings s = MakePreset(WorldPreset::Temperate);
    s.terrain.width = s.terrain.height = 129;
    s.terrain.hydraulic_droplets = 4000;
    s.terrain.river_min_catchment = 80;
    return s;
}

struct Walk {
    JobSystem jobs{2};
    World world;
    PhysicsWorld physics{jobs};
    PhysicsScene scene{world, physics};
    CharacterSystem characters{world, physics, &scene};
    void Step() {
        characters.Step(kDt);
        scene.Step(kDt);
    }
};

} // namespace

AETHER_TEST(ProceduralWalk_ACharacterStandsOnTheBakedGroundAndWalksItsHills) {
    ProceduralWorldDocument doc(Hills());
    doc.Generate();
    const GeneratedTerrain& t = doc.Terrain();
    const ProceduralWorldDocument::CollisionMesh mesh = ProceduralWorldDocument::BuildTerrainCollisionMesh(t, 128);
    AETHER_CHECK(!mesh.vertices.empty() && mesh.indices.size() % 3 == 0);
    // The vertices are the heightmap's own samples: a point on the mesh is a point on the ground.
    for (const Vec3& v : mesh.vertices) {
        const f32 sampled = t.heightmap.SampleLinear(v.x, v.z);
        AETHER_CHECK(std::fabs(sampled - v.y) < 1e-3f);
    }

    Walk w;
    MeshCollider collider;
    collider.vertices = mesh.vertices;
    collider.indices = mesh.indices;
    w.world.CreateEntity(Transform{Vec3(0, 0, 0), Quaternion::Identity()}, collider);

    // A walk that is walkable: dry land well above the sea, and a line of 10 m ahead (sampled every metre) that never climbs or drops more than 0.45 m between samples (a character slides
    // down what is steeper than its 45 degree limit, and a ledge is a fall, as they should be), a few spread across the map.
    const f32 cell = t.heightmap.cell_size;
    struct Walk1 { f32 x, z, dx, dz; };
    std::vector<Walk1> candidates, spots;
    const f32 extent = static_cast<f32>(t.Width() - 1) * cell;
    for (u32 z = 12; z + 12 < t.Height(); z += 5)
        for (u32 x = 12; x + 12 < t.Width(); x += 5) {
            const f32 wx = static_cast<f32>(x) * cell, wz = static_cast<f32>(z) * cell;
            if (t.heightmap.SampleLinear(wx, wz) < t.sea_height + 4.0f) continue;
            for (int k = 0; k < 8; ++k) {
                const f32 angle = static_cast<f32>(k) * 0.7853982f, dx = std::cos(angle), dz = std::sin(angle);
                bool ok = wx + 11.0f * dx > 0 && wx + 11.0f * dx < extent && wz + 11.0f * dz > 0 && wz + 11.0f * dz < extent;
                f32 last = t.heightmap.SampleLinear(wx, wz);
                for (int m = 1; ok && m <= 11; ++m) {
                    const f32 h = t.heightmap.SampleLinear(wx + static_cast<f32>(m) * dx, wz + static_cast<f32>(m) * dz);
                    ok = std::fabs(h - last) < 0.45f && h > t.sea_height + 1.0f;
                    last = h;
                }
                if (ok) { candidates.push_back({wx, wz, dx, dz}); break; }
            }
        }
    for (usize i = 0; i < candidates.size() && spots.size() < 4; i += std::max<usize>(candidates.size() / 4, 1)) spots.push_back(candidates[i]);
    std::printf("    %zu walkable lines among %zu candidates on a %ux%u map (%.0f m)\n", spots.size(), candidates.size(), t.Width(), t.Height(), extent);
    AETHER_CHECK(spots.size() >= 2);

    f32 worst_below = 0.0f, worst_above = 0.0f, total_walked = 0.0f;
    for (const Walk1& spot : spots) {
        const f32 ground = t.heightmap.SampleLinear(spot.x, spot.z);
        const Entity c = w.world.CreateEntity(Transform{Vec3(spot.x, ground + 3.0f, spot.z), Quaternion::Identity()}, CharacterMovement{});
        for (int i = 0; i < 120; ++i) w.Step(); // falls and lands
        const CharacterMovement* m = w.world.GetComponent<CharacterMovement>(c);
        const Vec3 start = w.world.GetComponent<Transform>(c)->position;
        std::printf("    spot (%.0f, %.0f): ground %.2f, feet %.2f, grounded %d, mode %d, slope normal y %.2f\n", spot.x, spot.z, ground, start.y, m ? (m->grounded ? 1 : 0) : -1, m ? static_cast<int>(m->mode) : -1, m ? m->ground_normal.y : 0.0f);
        AETHER_CHECK(m && m->grounded);
        AETHER_CHECK(std::fabs(start.y - ground) < 0.35f); // standing on the surface, within a triangle's slope of the sampled height
        const Vec3 toward(spot.dx, 0.0f, spot.dz);
        for (int i = 0; i < 180; ++i) {
            w.world.GetComponent<CharacterMovement>(c)->AddInput(toward);
            w.Step();
            const Vec3 p = w.world.GetComponent<Transform>(c)->position;
            const f32 under = p.y - t.heightmap.SampleLinear(p.x, p.z); // feet height over the sampled ground
            worst_below = std::min(worst_below, under);
            worst_above = std::max(worst_above, under);
        }
        const Vec3 end = w.world.GetComponent<Transform>(c)->position;
        total_walked += std::sqrt((end.x - start.x) * (end.x - start.x) + (end.z - start.z) * (end.z - start.z));
    }
    std::printf("    feet over the ground while walking: %.2f m lowest, %.2f m highest; walked %.1f m in all\n", worst_below, worst_above, total_walked);
    AETHER_CHECK(worst_below > -0.35f); // never through it
    AETHER_CHECK(worst_above < 0.8f);  // never floating (no jump, no ledge on these lines)
    AETHER_CHECK(total_walked > 7.0f * static_cast<f32>(spots.size())); // 3 s at 4 m/s is up to 12 m: the ground did not stop it
}

AETHER_TEST(ProceduralWalk_TheBakedSceneCarriesTheCollisionAndItsSizeIsKnown) {
    ProceduralWorldSettings s = Hills();
    ProceduralWorldDocument doc(s);
    doc.Generate();
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "aether_procedural_walk";
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    BakeOptions options;
    options.name = "Walk";
    options.terrain_texture = 0;
    options.water_texture = 0;
    const BakeReport report = doc.Bake(root.string(), options);
    AETHER_CHECK(report.ok);
    World loaded;
    AETHER_CHECK(LoadSceneJson(loaded, (root / report.scene).string()));
    usize colliders = 0, triangles = 0;
    loaded.ForEach<MeshCollider>([&](MeshCollider& m) { ++colliders; triangles += m.indices.size() / 3; });
    const auto bytes = std::filesystem::file_size(root / report.scene);
    std::printf("    baked scene: %zu mesh collider(s), %zu triangles, %.1f MB of json\n", colliders, triangles, static_cast<double>(bytes) / 1e6);
    AETHER_CHECK(colliders == 1 && triangles > 1000);
    // Off: no collider, as before.
    options.terrain_collision = false;
    options.name = "WalkOff";
    const BakeReport off = doc.Bake(root.string(), options);
    AETHER_CHECK(off.ok);
    World without;
    AETHER_CHECK(LoadSceneJson(without, (root / off.scene).string()));
    usize none = 0;
    without.ForEach<MeshCollider>([&](MeshCollider&) { ++none; });
    AETHER_CHECK(none == 0);
    std::filesystem::remove_all(root, ignored);
}

AETHER_TEST(ProceduralWalk_HowFarACoarserCollisionMeshStraysFromTheGround) {
    // The collision mesh is the ground on a grid of at most `max_grid` quads a side, so between its vertices it is a flat triangle where the ground is not. At the middle of each quad
    // (where the two triangles meet) it is the average of two corners; the ground's own height there is the heightmap's. The gap is the most a foot can sink or float.
    ProceduralWorldSettings s = MakePreset(WorldPreset::Temperate); // the default 193 x 193 map
    s.terrain.hydraulic_droplets = 8000;
    ProceduralWorldDocument doc(s);
    doc.Generate();
    const GeneratedTerrain& t = doc.Terrain();
    f32 previous_worst = 1e9f;
    for (const u32 grid : {48u, 96u, 192u}) {
        const ProceduralWorldDocument::CollisionMesh mesh = ProceduralWorldDocument::BuildTerrainCollisionMesh(t, grid);
        const u32 per_side = static_cast<u32>(std::lround(std::sqrt(static_cast<double>(mesh.vertices.size())))) ;
        f32 worst = 0.0f, sum = 0.0f;
        usize count = 0;
        for (u32 z = 0; z + 1 < per_side; ++z)
            for (u32 x = 0; x + 1 < per_side; ++x) {
                const Vec3 &a = mesh.vertices[z * per_side + x + 1], &b = mesh.vertices[(z + 1) * per_side + x]; // the corners the shared diagonal joins
                const f32 on_mesh = 0.5f * (a.y + b.y);
                const f32 on_ground = t.heightmap.SampleLinear(0.5f * (a.x + b.x), 0.5f * (a.z + b.z));
                const f32 gap = std::fabs(on_mesh - on_ground);
                worst = std::max(worst, gap);
                sum += gap;
                ++count;
            }
        std::printf("    collision grid %3u: %6zu triangles, mean gap %.3f m, worst %.2f m (%u m cells)\n", grid, mesh.indices.size() / 3, sum / static_cast<f32>(count), worst, static_cast<u32>(t.heightmap.cell_size * static_cast<f32>(t.Width() / per_side + 1)));
        AETHER_CHECK(worst <= previous_worst + 1e-3f); // a finer grid never strays more
        previous_worst = worst;
    }
}
