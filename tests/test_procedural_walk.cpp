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

    // Dry land well above the sea, a few spread across the map; a walk from each one.
    const f32 cell = t.heightmap.cell_size;
    struct Spot { f32 x, z; };
    std::vector<Spot> spots;
    for (u32 z = 16; z + 16 < t.Height() && spots.size() < 4; z += 29)
        for (u32 x = 16; x + 16 < t.Width() && spots.size() < 4; x += 31) {
            const f32 height = t.heightmap.heights[t.Index(x, z)];
            if (height > t.sea_height + 4.0f) spots.push_back({static_cast<f32>(x) * cell, static_cast<f32>(z) * cell});
        }
    std::printf("    %zu dry spots on a %ux%u map (%.0f m)\n", spots.size(), t.Width(), t.Height(), static_cast<f32>(t.Width() - 1) * cell);
    AETHER_CHECK(spots.size() >= 2);

    f32 worst_below = 0.0f, worst_above = 0.0f, total_walked = 0.0f;
    for (const Spot& spot : spots) {
        const f32 ground = t.heightmap.SampleLinear(spot.x, spot.z);
        const Entity c = w.world.CreateEntity(Transform{Vec3(spot.x, ground + 3.0f, spot.z), Quaternion::Identity()}, CharacterMovement{});
        for (int i = 0; i < 120; ++i) w.Step(); // falls and lands
        const CharacterMovement* m = w.world.GetComponent<CharacterMovement>(c);
        AETHER_CHECK(m && m->grounded);
        const Vec3 start = w.world.GetComponent<Transform>(c)->position;
        AETHER_CHECK(std::fabs(start.y - ground) < 0.35f); // standing on the surface, within a triangle's slope of the sampled height
        // Three seconds of walking toward the middle of the map.
        const f32 centre = static_cast<f32>(t.Width() - 1) * cell * 0.5f;
        Vec3 toward(centre - spot.x, 0.0f, centre - spot.z);
        const f32 len = std::sqrt(toward.x * toward.x + toward.z * toward.z);
        if (len > 1e-3f) toward = Vec3(toward.x / len, 0, toward.z / len);
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
    AETHER_CHECK(worst_below > -0.4f); // never through it
    AETHER_CHECK(worst_above < 1.5f);  // never floating (a jump or a drop off a ledge is not in this walk)
    AETHER_CHECK(total_walked > 4.0f * static_cast<f32>(spots.size())); // it moved: the ground did not stop it dead
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
    loaded.Each<MeshCollider>([&](Entity, MeshCollider& m) { ++colliders; triangles += m.indices.size() / 3; });
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
    without.Each<MeshCollider>([&](Entity, MeshCollider&) { ++none; });
    AETHER_CHECK(none == 0);
    std::filesystem::remove_all(root, ignored);
}
