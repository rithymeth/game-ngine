#include "aether/job/job_system.h"
#include "aether/physics/physics_world.h"
#include "test_framework.h"

using namespace aether;

AETHER_TEST(PhysicsWorld_SphereFallsAndRestsOnFloor) {
    JobSystem jobs(2);
    PhysicsWorld physics(jobs);

    // Floor: box centered at y=0, half-extents (10, 0.5, 10) -> top surface at y=0.5.
    physics.CreateBox(Vec3(0, 0, 0), Vec3(10, 0.5f, 10));

    JPH::BodyID sphere = physics.CreateSphere(Vec3(0, 5, 0), /*radius=*/0.5f, /*mass=*/1.0f, /*is_static=*/false);

    f32 initial_y = physics.GetPosition(sphere).y;
    AETHER_CHECK_NEAR(initial_y, 5.0f, 1e-3);

    constexpr f32 kDt = 1.0f / 60.0f;
    for (int i = 0; i < 240; ++i) { // 4 seconds, comfortably enough to fall and settle
        physics.Step(kDt);
    }

    f32 final_y = physics.GetPosition(sphere).y;
    AETHER_CHECK(final_y < initial_y);      // it fell
    AETHER_CHECK_NEAR(final_y, 1.0f, 0.1);  // settled resting on the floor (top 0.5 + radius 0.5)
}

AETHER_TEST(PhysicsWorld_StaticBodyDoesNotMove) {
    JobSystem jobs(2);
    PhysicsWorld physics(jobs);
    JPH::BodyID floor = physics.CreateBox(Vec3(1, 2, 3), Vec3(10, 0.5f, 10));

    for (int i = 0; i < 60; ++i) {
        physics.Step(1.0f / 60.0f);
    }

    Vec3 pos = physics.GetPosition(floor);
    AETHER_CHECK_NEAR(pos.x, 1.0f, 1e-4);
    AETHER_CHECK_NEAR(pos.y, 2.0f, 1e-4);
    AETHER_CHECK_NEAR(pos.z, 3.0f, 1e-4);
}

AETHER_TEST(SyncPhysicsToTransforms_UpdatesEcsFromSimulation) {
    World world;
    JobSystem jobs(2);
    PhysicsWorld physics(jobs);

    physics.CreateBox(Vec3(0, 0, 0), Vec3(10, 0.5f, 10));
    JPH::BodyID sphere_id = physics.CreateSphere(Vec3(0, 5, 0), 0.5f, 1.0f, false);

    RigidBody body;
    body.body_id = sphere_id;
    body.radius = 0.5f;
    body.mass = 1.0f;
    Entity entity = world.CreateEntity(Transform{Vec3(0, 5, 0), Quaternion::Identity()}, body);

    constexpr f32 kDt = 1.0f / 60.0f;
    for (int i = 0; i < 240; ++i) {
        SyncPhysicsToTransforms(world, physics, kDt);
    }

    Transform* transform = world.GetComponent<Transform>(entity);
    AETHER_CHECK(transform != nullptr);
    AETHER_CHECK_NEAR(transform->position.y, 1.0f, 0.1);
}

// Exercises the exact mechanism the editor's live-edit inspector drives for
// a position edit (drag the Position widget -> PhysicsWorld::SetPosition),
// without needing to actually click an ImGui widget.
AETHER_TEST(PhysicsWorld_SetPositionTeleportsBodyImmediately) {
    JobSystem jobs(2);
    PhysicsWorld physics(jobs);
    JPH::BodyID sphere = physics.CreateSphere(Vec3(0, 5, 0), 0.5f, 1.0f, /*is_static=*/false);

    physics.SetPosition(sphere, Vec3(3, 7, -2));

    Vec3 pos = physics.GetPosition(sphere);
    AETHER_CHECK_NEAR(pos.x, 3.0f, 1e-4);
    AETHER_CHECK_NEAR(pos.y, 7.0f, 1e-4);
    AETHER_CHECK_NEAR(pos.z, -2.0f, 1e-4);
}

// Exercises the exact mechanism the editor's live-edit inspector drives for
// a radius/mass/static edit: since Jolt's shape and motion type are
// effectively immutable once a body exists, the editor destroys the old
// body and recreates it at the same position with the new radius. This
// checks that recreation actually took effect physically: a sphere resting
// on a floor settles higher after its radius grows (resting center = floor
// top + radius), not just that the API calls didn't crash.
AETHER_TEST(Editor_LiveEditRadiusRecreatesBodyAndChangesRestingHeight) {
    JobSystem jobs(2);
    PhysicsWorld physics(jobs);

    // Floor: box centered at y=0, half-extents (10, 0.5, 10) -> top surface at y=0.5.
    physics.CreateBox(Vec3(0, 0, 0), Vec3(10, 0.5f, 10));

    JPH::BodyID sphere = physics.CreateSphere(Vec3(0, 5, 0), /*radius=*/0.5f, /*mass=*/1.0f, /*is_static=*/false);

    constexpr f32 kDt = 1.0f / 60.0f;
    for (int i = 0; i < 240; ++i) {
        physics.Step(kDt);
    }
    f32 settled_small = physics.GetPosition(sphere).y;
    AETHER_CHECK_NEAR(settled_small, 1.0f, 0.1); // floor top 0.5 + radius 0.5

    // Simulate the editor's "Radius" drag widget changing 0.5 -> 1.5: same
    // recreate-in-place sequence as the RigidBody edit branch in editor/main.cpp.
    Vec3 position_at_edit = physics.GetPosition(sphere);
    physics.DestroyBody(sphere);
    JPH::BodyID resized = physics.CreateSphere(position_at_edit, /*radius=*/1.5f, /*mass=*/1.0f, /*is_static=*/false);
    AETHER_CHECK(resized != sphere); // genuinely a new body, not a mutated old one

    for (int i = 0; i < 240; ++i) {
        physics.Step(kDt);
    }
    f32 settled_large = physics.GetPosition(resized).y;
    AETHER_CHECK_NEAR(settled_large, 2.0f, 0.1); // floor top 0.5 + new radius 1.5
    AETHER_CHECK(settled_large > settled_small + 0.5f); // unambiguously higher, not noise
}

AETHER_TEST(RigidBody_SerializerRoundTripsShapeDataNotHandle) {
    RegisterPhysicsComponentSerializers();

    RigidBody original;
    original.body_id = JPH::BodyID(42);
    original.radius = 2.5f;
    original.mass = 3.5f;
    original.is_static = true;

    std::vector<u8> bytes;
    const ComponentInfo& info = GetComponentInfo(GetComponentId<RigidBody>());
    info.serialize(&original, bytes);

    RigidBody restored;
    info.deserialize(&restored, bytes.data(), bytes.size());

    AETHER_CHECK_NEAR(restored.radius, 2.5f, 1e-6);
    AETHER_CHECK_NEAR(restored.mass, 3.5f, 1e-6);
    AETHER_CHECK(restored.is_static == true);
    AETHER_CHECK(restored.body_id.IsInvalid()); // handle deliberately NOT preserved
}

#include "aether/physics/components.h"
#include "aether/scene/serialization.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

AETHER_TEST(PhysicsComponents_AreReflectedAndRoundTripThroughBothSceneFormats) {
    RegisterPhysicsComponentSerializers();

    const reflect::TypeInfo& transform = reflect::Reflect<Transform>();
    AETHER_CHECK(transform.FindField("position") && transform.FindField("rotation"));
    const reflect::TypeInfo& body = reflect::Reflect<RigidBody>();
    AETHER_CHECK(body.fields.size() == 3);            // radius, mass, is_static
    AETHER_CHECK(body.FindField("body_id") == nullptr); // live handle: never reflected

    const ComponentInfo& body_info = GetComponentInfo(GetComponentId<RigidBody>());
    AETHER_CHECK(body_info.encoding == ComponentEncoding::Custom); // binary format unchanged
    AETHER_CHECK(body_info.reflected == &body);
    AETHER_CHECK(std::string(GetComponentInfo(GetComponentId<Transform>()).name) == "Transform");

    World world;
    RigidBody rb;
    rb.radius = 0.75f;
    rb.mass = 3.0f;
    rb.is_static = true;
    rb.body_id = JPH::BodyID(12345);
    world.CreateEntity(Transform{Vec3(1, 2, 3), Quaternion::Identity()}, rb);

    auto check = [](World& loaded) {
        int count = 0;
        loaded.ForEach<Transform, RigidBody>([&](Transform& t, RigidBody& b) {
            ++count;
            AETHER_CHECK(t.position.y == 2.0f && t.rotation.w == 1.0f);
            AETHER_CHECK(b.radius == 0.75f && b.mass == 3.0f && b.is_static);
            AETHER_CHECK(b.body_id.IsInvalid()); // must be recreated, never restored
        });
        AETHER_CHECK(count == 1);
    };

    std::string binary_path = (std::filesystem::temp_directory_path() / "aether_test_physics.aesc").string();
    AETHER_CHECK(SaveScene(world, binary_path));
    World from_binary;
    AETHER_CHECK(LoadScene(from_binary, binary_path));
    check(from_binary);

    std::string json_path = (std::filesystem::temp_directory_path() / "aether_test_physics.ascene").string();
    AETHER_CHECK(SaveSceneJson(world, json_path));
    std::ifstream file(json_path);
    std::string text((std::istreambuf_iterator<char>(file)), {});
    AETHER_CHECK(text.find("\"RigidBody\"") != std::string::npos);
    AETHER_CHECK(text.find("body_id") == std::string::npos);
    World from_json;
    AETHER_CHECK(LoadSceneJson(from_json, json_path));
    check(from_json);

    std::filesystem::remove(binary_path);
    std::filesystem::remove(json_path);
}
