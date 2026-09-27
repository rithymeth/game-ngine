#include "aether/job/job_system.h"
#include "aether/physics/physics_scene.h"
#include "aether/reflection/serialize.h"
#include "aether/scene/serialization.h"
#include "test_framework.h"

#include <cstring>
#include <filesystem>

using namespace aether;

// Phase 13 step 1: collider components, the motion-only RigidBody (and the
// migration from the old sphere-only one), and PhysicsScene building Jolt
// bodies from them.

namespace {

constexpr f32 kDt = 1.0f / 60.0f;

struct Sim {
    JobSystem jobs{2};
    World world;
    PhysicsWorld physics{jobs};
    PhysicsScene scene{world, physics};

    Sim() { RegisterPhysicsComponentSerializers(); }
    Entity Floor() { // a 20 x 1 x 20 static box whose top is at y = 0.5
        BoxCollider box;
        box.half_extents = Vec3(10, 0.5f, 10);
        return world.CreateEntity(Transform{Vec3(0, 0, 0), Quaternion::Identity()}, box);
    }
    template <typename Collider>
    Entity Body(const Vec3& at, const Collider& collider, BodyMotion motion = BodyMotion::Dynamic) {
        RigidBody rb;
        rb.motion = motion;
        return world.CreateEntity(Transform{at, Quaternion::Identity()}, rb, collider);
    }
    void Run(int steps) {
        for (int i = 0; i < steps; ++i) scene.Step(kDt);
    }
    f32 Y(Entity e) { return world.GetComponent<Transform>(e)->position.y; }
};

SphereCollider Sphere(f32 radius) {
    SphereCollider s;
    s.radius = radius;
    return s;
}

} // namespace

AETHER_TEST(PhysicsScene_OldRigidBodiesMigrateToColliders) {
    RegisterPhysicsComponentSerializers();

    // The pre-split binary payload: radius, mass, is_static.
    std::vector<u8> legacy;
    AppendComponentField(legacy, 0.75f);
    AppendComponentField(legacy, 3.0f);
    AppendComponentField(legacy, true);
    AETHER_CHECK(legacy.size() == 9);
    const ComponentInfo& info = GetComponentInfo(GetComponentId<RigidBody>());
    RigidBody from_binary;
    info.deserialize(&from_binary, legacy.data(), legacy.size());
    AETHER_CHECK(from_binary.legacy_radius == 0.75f && from_binary.mass == 3.0f && from_binary.motion == BodyMotion::Static);

    // The pre-split JSON (schema version 1).
    RigidBody from_json;
    AETHER_CHECK(reflect::FromJson(from_json, reflect::Json::parse(R"({"$v": 1, "radius": 0.25, "mass": 2, "is_static": false})")));
    AETHER_CHECK(from_json.legacy_radius == 0.25f && from_json.mass == 2.0f && from_json.motion == BodyMotion::Dynamic);

    // After loading, the legacy radius becomes a SphereCollider, unless the
    // entity already has a collider.
    World world;
    const Entity a = world.CreateEntity(Transform{}, from_binary);
    const Entity b = world.CreateEntity(Transform{}, from_json, BoxCollider{});
    const Entity c = world.CreateEntity(Transform{}, RigidBody{});
    AETHER_CHECK(MigrateLegacyRigidBodies(world) == 1);
    AETHER_CHECK(world.GetComponent<SphereCollider>(a) != nullptr && world.GetComponent<SphereCollider>(a)->radius == 0.75f);
    AETHER_CHECK(world.GetComponent<SphereCollider>(b) == nullptr && world.GetComponent<RigidBody>(b)->legacy_radius == 0.0f);
    AETHER_CHECK(!HasCollider(world, c) && HasCollider(world, a) && HasCollider(world, b));
    AETHER_CHECK(MigrateLegacyRigidBodies(world) == 0); // once
}

AETHER_TEST(PhysicsScene_CollidersRoundTripThroughBothSceneFormats) {
    RegisterPhysicsComponentSerializers();
    World world;
    CapsuleCollider capsule;
    capsule.height = 2.0f;
    capsule.is_trigger = true;
    ConvexCollider hull;
    hull.points = {Vec3(0, 0, 0), Vec3(1, 0, 0), Vec3(0, 1, 0), Vec3(0, 0, 1)};
    MeshCollider mesh;
    mesh.vertices = {Vec3(0, 0, 0), Vec3(1, 0, 0), Vec3(0, 0, 1)};
    mesh.indices = {0, 1, 2};
    RigidBody rb;
    rb.motion = BodyMotion::Kinematic;
    rb.continuous_collision = true;
    world.CreateEntity(Transform{}, rb, capsule, hull);
    world.CreateEntity(Transform{}, mesh, Sphere(2.0f));
    auto check = [](World& loaded) {
        int n = 0;
        loaded.ForEach<RigidBody, CapsuleCollider, ConvexCollider>([&](RigidBody& r, CapsuleCollider& c, ConvexCollider& h) {
            ++n;
            AETHER_CHECK(r.motion == BodyMotion::Kinematic && r.continuous_collision && r.body_id.IsInvalid());
            AETHER_CHECK(c.height == 2.0f && c.is_trigger && h.points.size() == 4 && h.points[3].z == 1.0f);
        });
        loaded.ForEach<MeshCollider, SphereCollider>([&](MeshCollider& m, SphereCollider& s) {
            ++n;
            AETHER_CHECK(m.indices.size() == 3 && m.vertices[1].x == 1.0f && s.radius == 2.0f);
        });
        AETHER_CHECK(n == 2);
    };
    const std::string binary = (std::filesystem::temp_directory_path() / "aether_test_colliders.aesc").string();
    const std::string json = (std::filesystem::temp_directory_path() / "aether_test_colliders.ascene").string();
    AETHER_CHECK(SaveScene(world, binary) && SaveSceneJson(world, json));
    World a, b;
    AETHER_CHECK(LoadScene(a, binary) && LoadSceneJson(b, json));
    check(a);
    check(b);
    std::filesystem::remove(binary);
    std::filesystem::remove(json);
}

AETHER_TEST(PhysicsScene_BuildsBodiesFromEachColliderKind) {
    Sim s;
    s.Floor();
    const Entity sphere = s.Body(Vec3(-6, 3, 0), Sphere(0.5f));
    BoxCollider box;
    box.half_extents = Vec3(0.5f, 0.25f, 0.5f);
    const Entity crate = s.Body(Vec3(-3, 3, 0), box);
    CapsuleCollider capsule; // 0.35 x 1.8
    RigidBody upright;
    upright.lock_rotation_x = upright.lock_rotation_z = true; // stays standing
    const Entity person = s.world.CreateEntity(Transform{Vec3(0, 3, 0), Quaternion::Identity()}, upright, capsule);
    ConvexCollider hull; // a unit cube from its corners
    for (float x : {-0.5f, 0.5f})
        for (float y : {-0.5f, 0.5f})
            for (float z : {-0.5f, 0.5f}) hull.points.push_back(Vec3(x, y, z));
    const Entity rock = s.Body(Vec3(3, 3, 0), hull);
    // A compound: a box body with a sphere on top (offset colliders).
    BoxCollider base;
    base.half_extents = Vec3(0.5f, 0.5f, 0.5f);
    SphereCollider head = Sphere(0.3f);
    head.center = Vec3(0, 0.8f, 0);
    RigidBody rb;
    const Entity snowman = s.world.CreateEntity(Transform{Vec3(6, 3, 0), Quaternion::Identity()}, rb, base, head);

    s.scene.Sync();
    AETHER_CHECK(s.scene.BodyCount() == 6 && s.scene.Problems().empty());
    AETHER_CHECK(s.scene.EntityOf(s.scene.BodyOf(rock)) == rock);
    AETHER_CHECK(s.world.GetComponent<RigidBody>(rock)->body_id == s.scene.BodyOf(rock));
    AETHER_CHECK(s.physics.BodyInterface().GetUserData(s.scene.BodyOf(sphere)) == PhysicsScene::EntityKey(sphere));

    s.Run(180);
    AETHER_CHECK_NEAR(s.Y(sphere), 1.0f, 0.03);  // floor top 0.5 + radius
    AETHER_CHECK_NEAR(s.Y(crate), 0.75f, 0.03);  // + half height
    AETHER_CHECK_NEAR(s.Y(person), 1.4f, 0.03);  // + half the capsule
    AETHER_CHECK_NEAR(s.Y(rock), 1.0f, 0.05);
    AETHER_CHECK_NEAR(s.Y(snowman), 1.0f, 0.05); // resting on its box, head up
}

AETHER_TEST(PhysicsScene_MeshCollidersAndTriggers) {
    Sim s;
    // A static triangle-mesh floor at y = 0 (two triangles).
    MeshCollider ground;
    ground.vertices = {Vec3(-10, 0, -10), Vec3(10, 0, -10), Vec3(10, 0, 10), Vec3(-10, 0, 10)};
    ground.indices = {0, 2, 1, 0, 3, 2};
    s.world.CreateEntity(Transform{}, ground);
    const Entity ball = s.Body(Vec3(0, 2, 0), Sphere(0.5f));
    // A trigger box in the way of a second ball: it falls through.
    BoxCollider zone;
    zone.half_extents = Vec3(1, 0.2f, 1);
    zone.is_trigger = true;
    const Entity trigger = s.world.CreateEntity(Transform{Vec3(4, 1.5f, 0), Quaternion::Identity()}, zone);
    const Entity through = s.Body(Vec3(4, 3, 0), Sphere(0.5f));
    // A moving mesh collider becomes its convex hull (a flat hull is degenerate, so give it depth).
    MeshCollider block;
    block.vertices = {Vec3(-0.5f, -0.5f, -0.5f), Vec3(0.5f, -0.5f, -0.5f), Vec3(0.5f, 0.5f, -0.5f), Vec3(-0.5f, 0.5f, -0.5f),
                      Vec3(-0.5f, -0.5f, 0.5f),  Vec3(0.5f, -0.5f, 0.5f),  Vec3(0.5f, 0.5f, 0.5f),  Vec3(-0.5f, 0.5f, 0.5f)};
    block.indices = {0, 1, 2};
    const Entity moving_mesh = s.Body(Vec3(-4, 3, 0), block);

    s.Run(180);
    AETHER_CHECK(s.scene.Problems().empty());
    AETHER_CHECK_NEAR(s.Y(ball), 0.5f, 0.03);
    AETHER_CHECK_NEAR(s.Y(through), 0.5f, 0.03); // passed through the trigger
    AETHER_CHECK_NEAR(s.Y(moving_mesh), 0.5f, 0.05);
    JPH::BodyLockRead lock(s.physics.System().GetBodyLockInterface(), s.scene.BodyOf(trigger));
    AETHER_CHECK(lock.Succeeded() && lock.GetBody().IsSensor());
}

AETHER_TEST(PhysicsScene_FollowsEditsMotionAndRemoval) {
    Sim s;
    s.Floor();
    const Entity ball = s.Body(Vec3(0, 3, 0), Sphere(0.5f));
    s.Run(120);
    AETHER_CHECK_NEAR(s.Y(ball), 1.0f, 0.03);

    // A collider edit rebuilds the body where it is.
    const JPH::BodyID before = s.scene.BodyOf(ball);
    s.world.GetComponent<SphereCollider>(ball)->radius = 1.0f;
    s.Run(120);
    AETHER_CHECK(!(s.scene.BodyOf(ball) == before) && s.scene.BodyCount() == 2);
    AETHER_CHECK_NEAR(s.Y(ball), 1.5f, 0.03);

    // Gameplay teleports a dynamic body by setting its Transform.
    s.world.GetComponent<Transform>(ball)->position = Vec3(2, 5, 0);
    s.scene.Step(kDt);
    AETHER_CHECK(s.Y(ball) > 4.9f && s.world.GetComponent<Transform>(ball)->position.x == 2.0f);

    // Static bodies stay put; kinematic ones follow their Transform.
    const Entity pillar = s.Body(Vec3(-3, 3, 0), Sphere(0.5f), BodyMotion::Static);
    const Entity lift = s.Body(Vec3(3, 3, 0), Sphere(0.5f), BodyMotion::Kinematic);
    s.Run(60);
    AETHER_CHECK(s.Y(pillar) == 3.0f && s.Y(lift) == 3.0f);
    s.world.GetComponent<Transform>(lift)->position.y = 4.0f;
    s.scene.Step(kDt);
    AETHER_CHECK_NEAR(s.physics.GetPosition(s.scene.BodyOf(lift)).y, 4.0f, 1e-4);

    // A bad collider reports why and has no body until fixed.
    ConvexCollider flat;
    flat.points = {Vec3(0, 0, 0), Vec3(1, 0, 0)};
    const Entity broken = s.Body(Vec3(0, 8, 0), flat);
    s.scene.Sync();
    AETHER_CHECK(s.scene.BodyOf(broken).IsInvalid() && s.scene.Problems().count(broken.index) == 1);
    s.world.GetComponent<ConvexCollider>(broken)->points = {Vec3(0, 0, 0), Vec3(1, 0, 0), Vec3(0, 1, 0), Vec3(0, 0, 1)};
    s.scene.Sync();
    AETHER_CHECK(!s.scene.BodyOf(broken).IsInvalid() && s.scene.Problems().empty());

    // Removing the collider, or the entity, removes the body.
    const usize bodies = s.scene.BodyCount();
    s.world.RemoveComponent<ConvexCollider>(broken);
    s.scene.Sync();
    AETHER_CHECK(s.scene.BodyCount() == bodies - 1 && s.world.GetComponent<RigidBody>(broken)->body_id.IsInvalid());
    const JPH::BodyID gone = s.scene.BodyOf(pillar);
    s.world.DestroyEntity(pillar);
    s.scene.Sync();
    AETHER_CHECK(s.scene.BodyCount() == bodies - 2 && s.scene.EntityOf(gone).IsNull());
}

AETHER_TEST(PhysicsScene_LayersFollowTheCollisionMatrix) {
    Sim s;
    ProjectSettings project;
    project.layers = {"Default", "Player", "Ghost"};
    SetLayersCollide(project, 2, 0, false); // ghosts pass through the Default floor
    s.physics.SetCollisionMatrix(MakeCollisionMatrix(project));
    AETHER_CHECK(!s.physics.GetCollisionMatrix().ShouldCollide(2, 0));

    s.Floor(); // Default
    const Entity player = s.Body(Vec3(-2, 3, 0), Sphere(0.5f));
    s.world.AddComponent(player, Layer{1});
    const Entity ghost = s.Body(Vec3(2, 3, 0), Sphere(0.5f));
    s.world.AddComponent(ghost, Layer{2});
    s.scene.Sync();
    AETHER_CHECK(PhysicsWorld::GameLayerOf(s.physics.BodyInterface().GetObjectLayer(s.scene.BodyOf(ghost))) == 2);
    s.Run(90);
    AETHER_CHECK_NEAR(s.Y(player), 1.0f, 0.03);
    AETHER_CHECK(s.Y(ghost) < -2.0f); // fell through

    // Changing an entity's layer moves its body to that layer.
    s.world.GetComponent<Transform>(ghost)->position = Vec3(2, 3, 0);
    s.world.GetComponent<Layer>(ghost)->index = 1;
    s.Run(90);
    AETHER_CHECK(PhysicsWorld::GameLayerOf(s.physics.BodyInterface().GetObjectLayer(s.scene.BodyOf(ghost))) == 1);
    AETHER_CHECK_NEAR(s.Y(ghost), 1.0f, 0.03);

    // Ghost vs Player still collide: a ghost dropped on the player rests on it.
    const Entity haunt = s.Body(Vec3(-2, 4, 0), Sphere(0.5f));
    s.world.AddComponent(haunt, Layer{2});
    s.Run(120);
    AETHER_CHECK(s.Y(haunt) > 1.5f); // stacked on the player (it may roll off later, but not through)
}
