#include "aether/scene/gameplay.h"
#include "aether/scene/hierarchy.h"
#include "aether/scene/lifecycle.h"
#include "aether/scene/prefab.h"
#include "aether/scene/serialization.h"
#include "aether/scene/world_origin.h"
#include "test_framework.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <span>
#include <unordered_map>
#include <vector>

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
    Vec3d used;
    AETHER_CHECK(LoadScene(loaded, path, options, &used));
    AETHER_CHECK_NEAR(used.x, 100000.0, 1e-6);
    AETHER_CHECK_NEAR(used.z, 200000.0, 1e-6);
    bool found = false;
    loaded.ForEach<Transform>([&](Transform& t) {
        AETHER_CHECK_NEAR(t.position.x, 0.0f, 1e-3);
        AETHER_CHECK_NEAR(t.position.z, 0.0f, 1e-3);
        found = true;
    });
    AETHER_CHECK(found);
    std::filesystem::remove(path);
}

// Phase 42.1: a JSON scene records its origin and writes absolute positions, so
// the loader shifts them back to the small local ones.
AETHER_TEST(WorldOrigin_JsonSceneRecordsItsOrigin) {
    World world;
    world.CreateEntity(Transform{Vec3(3, 0, 4), Quaternion::Identity()}); // local
    const std::string path = (std::filesystem::temp_directory_path() / "aether_test_origin_record.ascene").string();
    SceneSaveOptions save;
    save.origin = Vec3d(100000.0, 0.0, 200000.0);
    AETHER_CHECK(SaveSceneJson(world, path, save));
    World loaded;
    AETHER_CHECK(LoadSceneJson(loaded, path)); // no options: the recorded origin applies
    bool found = false;
    loaded.ForEach<Transform>([&](Transform& t) {
        AETHER_CHECK_NEAR(t.position.x, 3.0f, 1e-2);
        AETHER_CHECK_NEAR(t.position.z, 4.0f, 1e-2);
        found = true;
    });
    AETHER_CHECK(found);
    std::filesystem::remove(path);
}

// Phase 42.1: the loader reports the origin it used, so a game can keep the world
// local and remember where it is.
AETHER_TEST(WorldOrigin_LoaderReportsTheOrigin) {
    World world;
    world.CreateEntity(Transform{Vec3(3, 0, 4), Quaternion::Identity()});
    const std::string path = (std::filesystem::temp_directory_path() / "aether_test_origin_report.ascene").string();
    SceneSaveOptions save;
    save.origin = Vec3d(100000.0, 0.0, 200000.0);
    AETHER_CHECK(SaveSceneJson(world, path, save));
    World loaded;
    Vec3d used;
    AETHER_CHECK(LoadSceneJson(loaded, path, SceneLoadOptions{}, &used));
    AETHER_CHECK_NEAR(used.x, 100000.0, 1e-6);
    AETHER_CHECK_NEAR(used.z, 200000.0, 1e-6);
    std::filesystem::remove(path);
}

// The numbers in docs/design/PERFORMANCE.md ("Subtrees in large scenes") come from this; it does nothing unless a size is given:
//   AETHER_TEST_FILTER=Hierarchy_Measure AETHER_HIERARCHY_ENTITIES=20000 aether_tests
AETHER_TEST(Hierarchy_Measure) {
    const char* text = std::getenv("AETHER_HIERARCHY_ENTITIES");
    const u32 count = text && *text ? static_cast<u32>(std::strtoul(text, nullptr, 10)) : 0;
    if (count == 0) return;
    const auto ms_since = [](std::chrono::steady_clock::time_point start) {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    };
    // One tree: entity i's parent is entity (i - 1) / 8, so every entity is under entity 0.
    const auto build = [&](Scene& s, std::vector<Entity>& entities) {
        for (u32 i = 0; i < count; ++i) {
            entities.push_back(s.Make(Vec3(static_cast<f32>(i), 0, 0)));
            if (i > 0) s.Attach(entities[i], entities[(i - 1) / 8]);
        }
    };
    {
        Scene s;
        std::vector<Entity> entities;
        build(s, entities);
        auto start = std::chrono::steady_clock::now();
        Vec3 sum(0, 0, 0);
        for (Entity e : entities) sum = sum + WorldPosition(s.world, s.guids, e);
        std::printf("%u entities: world position of every entity: %.1f ms (checksum %.0f)\n", count, ms_since(start), sum.x);
        start = std::chrono::steady_clock::now();
        const PrefabData prefab = MakePrefab(s.world, s.guids, entities[0]);
        std::printf("%u entities: MakePrefab of the root: %.1f ms (%zu entities)\n", count, ms_since(start), prefab.entities.size());
    }
    {
        Scene s;
        std::vector<Entity> entities;
        build(s, entities);
        Lifecycle lifecycle(s.world, s.guids);
        const auto start = std::chrono::steady_clock::now();
        lifecycle.Destroy(entities[0]);
        std::printf("%u entities: Lifecycle::Destroy of the root: %.1f ms (%zu left)\n", count, ms_since(start),
                    static_cast<usize>(s.world.EntityCount()));
    }
}

// ChildIndex answers what ChildrenOf answers, for every entity of a forest with the awkward cases in it.
AETHER_TEST(Hierarchy_ChildIndexMatchesChildrenOf) {
    Scene s;
    std::vector<Entity> entities;
    u32 x = 0x2545f491u;
    const auto next = [&] {
        x ^= x << 13, x ^= x >> 17, x ^= x << 5;
        return x;
    };
    for (u32 i = 0; i < 600; ++i) {
        entities.push_back(s.Make(Vec3(static_cast<f32>(i), 0, 0)));
        if (i > 0 && next() % 5 != 0) s.Attach(entities[i], entities[next() % i]); // one in five stays a root
    }
    // A parent that does not exist, a null parent, and a parent destroyed after its children were attached.
    s.world.AddComponent(s.Make(Vec3(0, 0, 0)), Parent{EntityGuid{123, 456}});
    s.world.AddComponent(s.Make(Vec3(0, 0, 0)), Parent{EntityGuid{}});
    s.world.DestroyEntity(entities[7]);
    entities.erase(entities.begin() + 7);

    const ChildIndex index(s.world);
    usize with_children = 0;
    for (Entity e : entities) {
        const std::vector<Entity> expected = ChildrenOf(s.world, s.guids, e);
        const std::span<const Entity> got = index.Children(s.world, e);
        AETHER_CHECK(std::vector<Entity>(got.begin(), got.end()) == expected);
        with_children += expected.empty() ? 0 : 1;
    }
    AETHER_CHECK(with_children > 50);
    AETHER_CHECK(index.Children(s.world, kNullEntity).empty());
}

AETHER_TEST(Hierarchy_SubtreeIsParentsFirstInEntityOrder) {
    Scene s;
    //        a
    //      b   c
    //     d e   f      and g apart
    Entity a = s.Make(Vec3(0, 0, 0)), b = s.Make(Vec3(0, 0, 0)), c = s.Make(Vec3(0, 0, 0)), d = s.Make(Vec3(0, 0, 0));
    Entity e = s.Make(Vec3(0, 0, 0)), f = s.Make(Vec3(0, 0, 0)), g = s.Make(Vec3(0, 0, 0));
    s.Attach(f, c);
    s.Attach(e, b);
    s.Attach(c, a);
    s.Attach(d, b);
    s.Attach(b, a);
    AETHER_CHECK(SubtreeOf(s.world, a) == std::vector<Entity>({a, b, d, e, c, f}));
    AETHER_CHECK(SubtreeOf(s.world, b) == std::vector<Entity>({b, d, e}));
    AETHER_CHECK(SubtreeOf(s.world, g) == std::vector<Entity>({g}));
    AETHER_CHECK(SubtreeOf(s.world, kNullEntity).empty());
}

// Bad data can make two entities each other's parent. Walking from one of them has to end, with each entity once.
AETHER_TEST(Hierarchy_ACycleEndsInsteadOfLooping) {
    {
        Scene s;
        Entity a = s.Make(Vec3(1, 0, 0)), b = s.Make(Vec3(2, 0, 0)), c = s.Make(Vec3(3, 0, 0));
        s.Attach(a, b);
        s.Attach(b, a);
        s.Attach(c, b);
        AETHER_CHECK(SubtreeOf(s.world, a) == std::vector<Entity>({a, b, c}));
        AETHER_CHECK(MakePrefab(s.world, s.guids, a).entities.size() == 3);
        Lifecycle lifecycle(s.world, s.guids);
        lifecycle.Destroy(a);
        AETHER_CHECK(!s.world.IsAlive(a) && !s.world.IsAlive(b) && !s.world.IsAlive(c));
    }
}

// A large subtree: everything under the root goes, children before their parents, and nothing else does.
AETHER_TEST(Hierarchy_DestroyingALargeSubtreeTakesExactlyThatSubtree) {
    Scene s;
    std::vector<Entity> entities;
    const u32 count = 4000;
    for (u32 i = 0; i < count; ++i) {
        entities.push_back(s.Make(Vec3(static_cast<f32>(i), 0, 0)));
        if (i > 1) s.Attach(entities[i], entities[(i - 2) / 6 + (i % 2)]); // two trees, rooted at 0 and 1
    }
    const std::vector<Entity> doomed = SubtreeOf(s.world, entities[0]);
    const std::vector<Entity> kept = SubtreeOf(s.world, entities[1]);
    AETHER_CHECK(doomed.size() + kept.size() == count && doomed.size() > 100 && kept.size() > 100);

    std::vector<EntityGuid> destroyed; // in the order OnDestroy ran
    Lifecycle lifecycle(s.world, s.guids);
    LifecycleCallbacks callbacks;
    callbacks.on_destroy = [&](Entity entity) {
        if (const IdComponent* id = s.world.GetComponent<IdComponent>(entity)) destroyed.push_back(id->guid);
    };
    lifecycle.Register<Transform>(callbacks);
    lifecycle.BeginPlay();
    std::unordered_map<EntityGuid, EntityGuid> parent_of;
    for (Entity e : doomed) {
        if (const Parent* p = s.world.GetComponent<Parent>(e)) parent_of[s.world.GetComponent<IdComponent>(e)->guid] = p->parent;
    }
    lifecycle.Destroy(entities[0]);

    AETHER_CHECK(destroyed.size() == doomed.size());
    std::unordered_map<EntityGuid, usize> position;
    for (usize i = 0; i < destroyed.size(); ++i) position[destroyed[i]] = i;
    bool children_first = true;
    for (const auto& [child, parent] : parent_of) {
        children_first = children_first && position.count(child) && position.count(parent) && position[child] < position[parent];
    }
    AETHER_CHECK(children_first);
    for (Entity e : doomed) AETHER_CHECK(!s.world.IsAlive(e));
    for (Entity e : kept) AETHER_CHECK(s.world.IsAlive(e));
    AETHER_CHECK(s.world.EntityCount() == kept.size());
}

// HierarchySnapshot gives, bit for bit, what ComputeWorldTransform and IsActiveInHierarchy give for every entity, with the awkward
// cases in the world: rotation and scale, inactive ancestors, an ancestor without a Transform, a parent that does not exist,
// a chain longer than the depth limit, a parent cycle, and an entity destroyed after the snapshot's world was built.
AETHER_TEST(Hierarchy_SnapshotMatchesThePerEntityWalks) {
    Scene s;
    std::vector<Entity> entities;
    u32 x = 0x1234abcdu;
    const auto next = [&] {
        x ^= x << 13, x ^= x >> 17, x ^= x << 5;
        return x;
    };
    const auto unit = [&] { return static_cast<f32>(next() % 2000) / 1000.0f - 1.0f; };
    for (u32 i = 0; i < 500; ++i) {
        Transform t;
        t.position = Vec3(unit() * 50, unit() * 50, unit() * 50);
        t.rotation = Quaternion::FromAxisAngle(Vec3(0, 1, 0), unit() * 3.0f);
        t.scale = Vec3(1.0f + unit() * 0.5f, 1.0f + unit() * 0.5f, 1.0f + unit() * 0.5f);
        const Entity e = s.world.CreateEntity(t);
        EnsureGuid(s.world, e, &s.guids);
        entities.push_back(e);
        if (i > 0 && next() % 6 != 0) s.Attach(e, entities[next() % i]);
        if (next() % 9 == 0) s.world.AddComponent(e, Active{next() % 2 == 0});
    }
    // A chain of 80, deeper than kMaxHierarchyDepth.
    Entity link = entities[0];
    for (int i = 0; i < 80; ++i) {
        const Entity e = s.Make(Vec3(1, 0.5f, 0));
        s.Attach(e, link);
        entities.push_back(link = e);
    }
    // An ancestor without a Transform, with a child under it.
    const Entity bare = s.world.CreateEntity(IdComponent{NewEntityGuid()});
    s.guids.Add(s.world.GetComponent<IdComponent>(bare)->guid, bare);
    s.world.AddComponent(bare, Parent{s.world.GetComponent<IdComponent>(entities[3])->guid});
    const Entity under_bare = s.Make(Vec3(2, 3, 4));
    s.Attach(under_bare, bare);
    entities.push_back(bare);
    entities.push_back(under_bare);
    // A parent that does not exist, and two entities that are each other's parent.
    const Entity orphan = s.Make(Vec3(7, 8, 9));
    s.world.AddComponent(orphan, Parent{EntityGuid{11, 22}});
    const Entity loop_a = s.Make(Vec3(1, 2, 3)), loop_b = s.Make(Vec3(4, 5, 6));
    s.Attach(loop_a, loop_b);
    s.Attach(loop_b, loop_a);
    entities.insert(entities.end(), {orphan, loop_a, loop_b});
    // A parent destroyed after its children were attached.
    s.world.DestroyEntity(entities[5]);

    HierarchySnapshot snapshot(s.world, s.guids);
    usize inactive = 0, compared = 0;
    for (Entity e : entities) {
        if (!s.world.IsAlive(e)) continue;
        const Mat4 expected = ComputeWorldTransform(s.world, s.guids, e);
        const Mat4 got = snapshot.WorldTransform(e);
        AETHER_CHECK(std::memcmp(&expected, &got, sizeof(Mat4)) == 0);
        const bool active = IsActiveInHierarchy(s.world, s.guids, e);
        AETHER_CHECK(snapshot.ActiveInHierarchy(e) == active);
        inactive += active ? 0 : 1;
        ++compared;
    }
    AETHER_CHECK(compared > 550 && inactive > 20 && inactive < compared);
    // Entities the snapshot does not hold answer as the walks do for a dead entity.
    const Mat4 identity = Mat4::Identity();
    const Mat4 dead = snapshot.WorldTransform(entities[5]);
    AETHER_CHECK(std::memcmp(&identity, &dead, sizeof(Mat4)) == 0);
    AETHER_CHECK(snapshot.ActiveInHierarchy(entities[5]) && snapshot.ActiveInHierarchy(kNullEntity));
}
