#include "aether/animation/animator.h"
#include "aether/job/job_system.h"
#include "aether/physics/character.h"
#include "test_framework.h"

#include <cmath>

using namespace aether;
using namespace aether::anim;

// Phase 16 step 4: root motion from the Animator moving a CharacterMovement.

AETHER_TEST(AnimationRootMotion_MovesTheCharacter) {
    Skeleton rig;
    rig.bones.push_back({"root", -1, {}, Mat4::Identity()});
    AnimationClip walk;
    walk.name = "Walk";
    walk.duration = 1.0f;
    walk.tracks.resize(1);
    walk.tracks[0].translation.times = {0.0f, 1.0f};
    walk.tracks[0].translation.values = {Vec3(0, 0, 0), Vec3(0, 0, 1.5f)}; // 1.5 m/s forward (+Z)
    AnimGraph graph;
    AnimNode clip;
    clip.id = 1;
    clip.asset = "Walk";
    graph.nodes = {clip};
    graph.output = 1;
    AnimationLibrary lib;
    lib.graph = [&](const std::string&) { return &graph; };
    lib.skeleton = [&](const std::string&) { return &rig; };
    lib.assets.clip = [&](const std::string& n) -> const AnimationClip* { return n == "Walk" ? &walk : nullptr; };

    JobSystem jobs(2);
    World world;
    PhysicsWorld physics(jobs);
    PhysicsScene scene(world, physics);
    CharacterSystem characters(world, physics, &scene);
    AnimationSystem animation(world, lib);
    BoxCollider floor;
    floor.half_extents = Vec3(30, 0.5f, 30);
    world.CreateEntity(Transform{Vec3(0, 0, 0), Quaternion::Identity()}, floor);
    // Facing +X (turned 90° about Y): the clip's forward becomes +X in the world.
    Animator anim;
    anim.graph = "G";
    anim.skeleton = "Rig";
    anim.root_motion = true;
    const Entity hero = world.CreateEntity(Transform{Vec3(0, 0.5f, 0), Quaternion::FromAxisAngle(Vec3(0, 1, 0), 1.5707963f)},
                                           CharacterMovement{}, anim);
    constexpr f32 dt = 1.0f / 60.0f;
    for (int i = 0; i < 30; ++i) { // settle on the floor
        characters.Step(dt);
        scene.Step(dt);
    }
    const Vec3 start = world.GetComponent<Transform>(hero)->position;
    for (int i = 0; i < 120; ++i) {
        animation.Update(dt);
        Vec3 move;
        f32 yaw = 0.0f;
        AETHER_CHECK(animation.RootMotionOf(hero, move, yaw));
        world.GetComponent<CharacterMovement>(hero)->AddRootMotion(move, yaw);
        characters.Step(dt);
        scene.Step(dt);
    }
    const Vec3 end = world.GetComponent<Transform>(hero)->position;
    AETHER_CHECK(std::fabs((end.x - start.x) - 3.0f) < 0.1f && std::fabs(end.z - start.z) < 0.05f); // 2 s at 1.5 m/s along +X
    AETHER_CHECK(world.GetComponent<CharacterMovement>(hero)->grounded);
    // Root motion is consumed each step; without it the character brakes to a stop.
    for (int i = 0; i < 60; ++i) {
        characters.Step(dt);
        scene.Step(dt);
    }
    AETHER_CHECK(std::fabs(world.GetComponent<CharacterMovement>(hero)->velocity.x) < 0.05f);
}
