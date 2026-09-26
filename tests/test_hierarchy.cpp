#include "aether/scene/hierarchy.h"
#include "aether/scene/serialization.h"
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
