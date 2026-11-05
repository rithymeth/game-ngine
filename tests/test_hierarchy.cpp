#include "aether/scene/hierarchy.h"
#include "aether/scene/serialization.h"
#include "aether/scene/world_origin.h"
#include "test_framework.h"

#include <cmath>
#include <filesystem>

using namespace aether;

namespace {

struct Scene {
    World world;
    GuidIndex guids;

    Entity Make(Vec3 position) {
        Entity e = world.CreateEntity(Transform{position, Quaternion::Identity()});
        EnsureGuid(world, e, &guids);
        return e;
    }
    void Attach(Entity child, Entity parent) {
        world.AddComponent(child, Parent{world.GetComponent<IdComponent>(parent)->guid});
    }
};

bool Near(Vec3 a, Vec3 b) {
    return std::abs(a.x - b.x) < 1e-4f && std::abs(a.y - b.y) < 1e-4f && std::abs(a.z - b.z) < 1e-4f;
}

} // namespace

AETHER_TEST(Hierarchy_WorldTransformComposesUpTheChain) {
    Scene s;
    Entity root = s.Make(Vec3(10, 0, 0));
    Entity mid = s.Make(Vec3(0, 5, 0));
    Entity leaf = s.Make(Vec3(0, 0, 1));
    s.Attach(mid, root);
    s.Attach(leaf, mid);

    AETHER_CHECK(GetParent(s.world, s.guids, leaf) == mid);
    AETHER_CHECK(GetParent(s.world, s.guids, root).IsNull());
    AETHER_CHECK(Near(WorldPosition(s.world, s.guids, leaf), Vec3(10, 5, 1)));

    // A 90-degree turn around +Y on the root rotates everything below it.
    s.world.GetComponent<Transform>(root)->rotation = Quaternion::FromAxisAngle(Vec3(0, 1, 0), 1.57079632f);
    Vec3 p = WorldPosition(s.world, s.guids, leaf);
    AETHER_CHECK(Near(p, Vec3(11, 5, 0))); // local +Z maps to world +X

    std::vector<Entity> children = ChildrenOf(s.world, s.guids, root);
    AETHER_CHECK(children.size() == 1 && children[0] == mid);
    AETHER_CHECK(ChildrenOf(s.world, s.guids, leaf).empty());
}

AETHER_TEST(Hierarchy_CycleDetection) {
    Scene s;
    Entity a = s.Make(Vec3(0, 0, 0));
    Entity b = s.Make(Vec3(0, 0, 0));
    Entity c = s.Make(Vec3(0, 0, 0));
    s.Attach(b, a);
    s.Attach(c, b);
    AETHER_CHECK(WouldCreateCycle(s.world, s.guids, a, c));  // a under c: c -> b -> a
    AETHER_CHECK(WouldCreateCycle(s.world, s.guids, a, a));  // own parent
    AETHER_CHECK(!WouldCreateCycle(s.world, s.guids, c, a)); // already a descendant: fine to re-attach
    AETHER_CHECK(!WouldCreateCycle(s.world, s.guids, a, kNullEntity));

    // A cycle produced by bad data is cut off instead of looping forever.
    s.Attach(a, c);
    Vec3 p = WorldPosition(s.world, s.guids, a);
    AETHER_CHECK(Near(p, Vec3(0, 0, 0)));
}

AETHER_TEST(Hierarchy_LinkSurvivesParentDestroyAndRecreate) {
    Scene s;
    Entity parent = s.Make(Vec3(3, 0, 0));
    Entity child = s.Make(Vec3(0, 1, 0));
    s.Attach(child, parent);
    EntityGuid parent_guid = s.world.GetComponent<IdComponent>(parent)->guid;

    s.world.DestroyEntity(parent);
    s.guids.Remove(parent_guid);
    AETHER_CHECK(GetParent(s.world, s.guids, child).IsNull()); // behaves as a root, no stale handle
    AETHER_CHECK(Near(WorldPosition(s.world, s.guids, child), Vec3(0, 1, 0)));

    // Recreated (as undo does) with the same GUID: the child is attached again.
    Entity recreated = s.world.CreateEntity(IdComponent{parent_guid}, Transform{Vec3(3, 0, 0), Quaternion::Identity()});
    s.guids.Add(parent_guid, recreated);
    AETHER_CHECK(GetParent(s.world, s.guids, child) == recreated);
    AETHER_CHECK(Near(WorldPosition(s.world, s.guids, child), Vec3(3, 1, 0)));
}

AETHER_TEST(Hierarchy_ParentSurvivesSceneFiles) {
    Scene s;
    Entity parent = s.Make(Vec3(2, 0, 0));
    Entity child = s.Make(Vec3(0, 0, 4));
    s.Attach(child, parent);

    for (bool json : {false, true}) {
        std::string path =
            (std::filesystem::temp_directory_path() / (json ? "aether_test_hierarchy.ascene" : "aether_test_hierarchy.aesc"))
                .string();
        AETHER_CHECK(json ? SaveSceneJson(s.world, path) : SaveScene(s.world, path));
        World loaded;
        AETHER_CHECK(json ? LoadSceneJson(loaded, path) : LoadScene(loaded, path));
        GuidIndex guids;
        AETHER_CHECK(guids.Rebuild(loaded).empty());
        EntityGuid child_guid = s.world.GetComponent<IdComponent>(child)->guid;
        Entity loaded_child = guids.Find(loaded, child_guid);
        AETHER_CHECK(!loaded_child.IsNull());
        AETHER_CHECK(!GetParent(loaded, guids, loaded_child).IsNull());
        AETHER_CHECK(Near(WorldPosition(loaded, guids, loaded_child), Vec3(2, 0, 4)));
        std::filesystem::remove(path);
    }
}

// Phase 42.1: moving the world origin shifts every f32 Transform the other way,
// so render-facing coordinates stay small; the inverse restores them.
AETHER_TEST(WorldOrigin_RebaseShiftsTransforms) {
    World world;
    const Entity a = world.CreateEntity(IdComponent{NewEntityGuid()}, Transform{Vec3(10, 0, 0), Quaternion::Identity()});
    const Entity b = world.CreateEntity(IdComponent{NewEntityGuid()}, Transform{Vec3(-5, 2, 1), Quaternion::Identity()});
    // A child's Transform is relative to its parent, so a rebase must leave it alone.
    const Entity child = world.CreateEntity(Transform{Vec3(1, 0, 0), Quaternion::Identity()},
                                            Parent{world.GetComponent<IdComponent>(a)->guid});
    AETHER_CHECK(RebaseTransforms(world, Vec3d(100.0, 0.0, 0.0)) == 2);
    AETHER_CHECK_NEAR(world.GetComponent<Transform>(a)->position.x, -90.0f, 1e-4);
    AETHER_CHECK_NEAR(world.GetComponent<Transform>(b)->position.x, -105.0f, 1e-4);
    AETHER_CHECK_NEAR(world.GetComponent<Transform>(b)->position.y, 2.0f, 1e-4);
    AETHER_CHECK_NEAR(world.GetComponent<Transform>(child)->position.x, 1.0f, 1e-4);
    ShiftTransforms(world, Vec3d(100.0, 0.0, 0.0));
    AETHER_CHECK_NEAR(world.GetComponent<Transform>(a)->position.x, 10.0f, 1e-4);
}

// Phase 42.1: a scene can load at an absolute origin, so its Transforms come in
// small (relative to that origin).
AETHER_TEST(WorldOrigin_LoadOptionsShiftTheScene) {
    World world;
    world.CreateEntity(Transform{Vec3(100000, 0, 200000), Quaternion::Identity()});
    const std::string path = (std::filesystem::temp_directory_path() / "aether_test_origin.ascene").string();
    AETHER_CHECK(SaveSceneJson(world, path));
    World loaded;
    SceneLoadOptions options;
    options.origin = Vec3d(100000.0, 0.0, 200000.0);
    AETHER_CHECK(LoadSceneJson(loaded, path, options));
    bool found = false;
    loaded.ForEach<Transform>([&](Transform& t) {
        AETHER_CHECK_NEAR(t.position.x, 0.0f, 1e-3);
        AETHER_CHECK_NEAR(t.position.z, 0.0f, 1e-3);
        found = true;
    });
    AETHER_CHECK(found);
    std::filesystem::remove(path);
}

// Phase 42.1: the binary loader applies the same origin shift as the JSON one.
AETHER_TEST(WorldOrigin_BinaryLoadShiftsTheScene) {
    World world;
    world.CreateEntity(Transform{Vec3(100000, 0, 200000), Quaternion::Identity()});
    const std::string path = (std::filesystem::temp_directory_path() / "aether_test_origin.aesc").string();
    AETHER_CHECK(SaveScene(world, path));
    World loaded;
    SceneLoadOptions options;
    options.origin = Vec3d(100000.0, 0.0, 200000.0);
    AETHER_CHECK(LoadScene(loaded, path, options));
    bool found = false;
    loaded.ForEach<Transform>([&](Transform& t) {
        AETHER_CHECK_NEAR(t.position.x, 0.0f, 1e-3);
        AETHER_CHECK_NEAR(t.position.z, 0.0f, 1e-3);
        found = true;
    });
    AETHER_CHECK(found);
    std::filesystem::remove(path);
}
