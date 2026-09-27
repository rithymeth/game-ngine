#pragma once

#include "aether/ecs/world.h"
#include "aether/nav/dynamic.h"
#include "aether/reflection/reflection.h"
#include "aether/scene/components.h"
#include "aether/scene/entity_guid.h"

#include <functional>
#include <unordered_map>

namespace aether {

enum class NavObstacleShape : u8 { Box, Cylinder };

// Carves a hole in the navigation mesh where the entity is (Phase 20 step
// 2, docs/design/PHASE_SPECS.md §20.2): a box turned with the entity (by
// yaw), or an upright cylinder. The hole follows the entity, rebaked once
// it has moved `move_threshold` or turned 5 degrees.
struct NavObstacle {
    NavObstacleShape shape = NavObstacleShape::Box;
    Vec3 center;                                // from the entity, in its space
    Vec3 half_extents = Vec3(0.5f, 0.5f, 0.5f); // box
    f32 radius = 0.5f, height = 2.0f;           // cylinder
    f32 move_threshold = 0.1f;
    bool carve = true; // off: no hole (agents only steer round it, step 3)
};

// Marks the ground inside a box (turned with the entity by yaw) with an area:
// water, grass, a road, or 0 to block it.
struct NavModifierVolume {
    Vec3 half_extents = Vec3(1, 1, 1);
    i32 area = 1;
};

// An off-mesh link (a jump, ladder or drop) from `start` to `end`, both in
// the entity's space.
struct NavLinkProxy {
    Vec3 start;
    Vec3 end = Vec3(0, 0, 2);
    f32 radius = 0.5f;
    bool bidirectional = true;
    i32 area = 63;
    i32 user_id = 0;
    bool enabled = true;
};

void RegisterNavComponents();

} // namespace aether

namespace aether::nav {

// Runs a world's navigation: bakes the mesh from its static geometry
// (models, through `meshes`), then keeps obstacles, modifier volumes and
// links in step with their entities, rebaking a few tiles per Update.
class NavWorld {
public:
    // A model's triangles in its own space; false when it has none (it's skipped).
    using MeshProvider = std::function<bool(const ModelRenderer& model, std::vector<Vec3>& vertices, std::vector<u32>& indices)>;
    // With `guids`, poses follow parents; without, each Transform is world space.
    NavWorld(World& world, MeshProvider meshes, const GuidIndex* guids = nullptr);
    ~NavWorld();
    NavWorld(const NavWorld&) = delete;
    NavWorld& operator=(const NavWorld&) = delete;

    // The static geometry: every ModelRenderer (not on an obstacle), in world space.
    NavGeometry Gather() const;
    // Bakes from Gather() with the volumes and links as they are now.
    bool Bake(const NavMeshSettings& settings, std::string* error = nullptr, NavBuildStats* stats = nullptr);
    // Uses a baked mesh (a loaded .anav); the geometry is gathered for later rebakes.
    bool Load(const NavMeshData& data, std::string* error = nullptr);
    // Follows the components, then rebakes up to `max_tiles` tiles (0: all).
    void Update(usize max_tiles = 4);

    DynamicNavMesh& Dynamic() { return nav_; }
    NavMesh& Mesh() { return nav_.Mesh(); }
    const NavMesh& Mesh() const { return nav_.Mesh(); }
    bool Ready() const { return nav_.Mesh().Loaded(); }

    // The world the Blueprint and Luau nodes query (step 3): the last made.
    static NavWorld* Active();
    void MakeActive();

private:
    struct Pose {
        Vec3 position;
        Quaternion rotation;
    };
    bool WorldPose(Entity e, Pose& out) const;
    void Sync();
    template <typename C>
    std::vector<Entity> With() const;

    struct Tracked {
        Entity entity;
        DynamicNavMesh::Id id = 0;
        Pose pose;
        u64 seen = 0;
    };
    World& world_;
    MeshProvider meshes_;
    const GuidIndex* guids_;
    DynamicNavMesh nav_;
    std::unordered_map<u32, Tracked> obstacles_, modifiers_, links_; // by entity index
    u64 frame_ = 0;
};

} // namespace aether::nav

AETHER_ENUM(aether::NavObstacleShape, 1, AETHER_ENUM_VALUE(Box), AETHER_ENUM_VALUE(Cylinder))

AETHER_REFLECT(aether::NavObstacle, 1,
    AETHER_FIELD(shape, Field_EditAnywhere),
    AETHER_FIELD(center, Field_EditAnywhere, {.tooltip = "From the entity, in its space", .units = "m"}),
    AETHER_FIELD(half_extents, Field_EditAnywhere, {.tooltip = "The box's half sizes", .units = "m"}),
    AETHER_FIELD(radius, Field_EditAnywhere, {.tooltip = "The cylinder's radius", .range_min = 0.0, .range_max = 1000.0, .units = "m"}),
    AETHER_FIELD(height, Field_EditAnywhere, {.tooltip = "The cylinder's height", .range_min = 0.0, .range_max = 1000.0, .units = "m"}),
    AETHER_FIELD(move_threshold, Field_EditAnywhere, {.tooltip = "Rebake once moved this far", .range_min = 0.0, .range_max = 100.0, .units = "m"}),
    AETHER_FIELD(carve, Field_EditAnywhere, {.tooltip = "Cut a hole in the navigation mesh"})
)

AETHER_REFLECT(aether::NavModifierVolume, 1,
    AETHER_FIELD(half_extents, Field_EditAnywhere, {.units = "m"}),
    AETHER_FIELD(area, Field_EditAnywhere, {.tooltip = "The area inside (0 blocks; 63 is plain ground)", .range_min = 0.0, .range_max = 63.0})
)

AETHER_REFLECT(aether::NavLinkProxy, 1,
    AETHER_FIELD(start, Field_EditAnywhere, {.tooltip = "In the entity's space", .units = "m"}),
    AETHER_FIELD(end, Field_EditAnywhere, {.tooltip = "In the entity's space", .units = "m"}),
    AETHER_FIELD(radius, Field_EditAnywhere, {.tooltip = "How near the mesh each end must be", .range_min = 0.0, .range_max = 100.0, .units = "m"}),
    AETHER_FIELD(bidirectional, Field_EditAnywhere),
    AETHER_FIELD(area, Field_EditAnywhere, {.tooltip = "The area whose cost the link takes", .range_min = 1.0, .range_max = 63.0}),
    AETHER_FIELD(user_id, Field_EditAnywhere),
    AETHER_FIELD(enabled, Field_EditAnywhere)
)
