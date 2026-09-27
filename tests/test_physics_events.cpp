#include "aether/blueprint/compiler.h"
#include "aether/blueprint/vm.h"
#include "aether/job/job_system.h"
#include "aether/physics/physics_scene.h"
#include "aether/scene/gameplay.h"
#include "test_framework.h"

#include <algorithm>
#include <cmath>
#include <sstream>

using namespace aether;

// Phase 13 step 3: contact and trigger events, delivered after the step on
// the main thread in a deterministic order.

namespace {

constexpr f32 kDt = 1.0f / 60.0f;

struct Sim {
    JobSystem jobs;
    World world;
    PhysicsWorld physics{jobs};
    PhysicsScene scene{world, physics};
    std::vector<PhysicsEvent> log;

    explicit Sim(u32 threads = 2) : jobs(threads) {
        scene.SetEventHandler([this](const PhysicsEvent& e) { log.push_back(e); });
    }
    Entity Floor() {
        BoxCollider box;
        box.half_extents = Vec3(10, 0.5f, 10);
        return world.CreateEntity(Transform{Vec3(0, 0, 0), Quaternion::Identity()}, box);
    }
    Entity Ball(const Vec3& at, f32 radius = 0.5f, bool stay = false) {
        SphereCollider s;
        s.radius = radius;
        s.report_stay = stay;
        return world.CreateEntity(Transform{at, Quaternion::Identity()}, RigidBody{}, s);
    }
    void Run(int steps) {
        for (int i = 0; i < steps; ++i) scene.Step(kDt);
    }
    usize Count(Entity self, PhysicsEventType type, Entity other = kNullEntity) const {
        return static_cast<usize>(std::count_if(log.begin(), log.end(), [&](const PhysicsEvent& e) {
            return e.self == self && e.type == type && (other.IsNull() || e.other == other);
        }));
    }
};

} // namespace

AETHER_TEST(PhysicsEvents_CollisionsBeginOnceAndEndOnlyWhenApart) {
    Sim s;
    const Entity floor = s.Floor();
    // A compound (two spheres side by side) lands on the floor: both parts
    // touch, but it's one Begin per entity pair.
    SphereCollider left, right;
    left.center = Vec3(-0.6f, 0, 0);
    right.center = Vec3(0.6f, 0, 0);
    const Entity dumbbell = s.world.CreateEntity(Transform{Vec3(0, 1.5f, 0), Quaternion::Identity()}, RigidBody{}, left);
    s.world.AddComponent(dumbbell, right);
    s.Run(90);
    AETHER_CHECK(s.Count(floor, PhysicsEventType::CollisionBegin, dumbbell) == 1);
    AETHER_CHECK(s.Count(dumbbell, PhysicsEventType::CollisionBegin, floor) == 1);
    AETHER_CHECK(s.Count(dumbbell, PhysicsEventType::CollisionStay) == 0); // not asked for

    // Each side's normal points at itself, and the impact speed is sensible
    // (a 1 m fall: about 4.4 m/s).
    auto begin = std::find_if(s.log.begin(), s.log.end(), [&](const PhysicsEvent& e) {
        return e.self == dumbbell && e.type == PhysicsEventType::CollisionBegin;
    });
    AETHER_CHECK(begin != s.log.end() && begin->normal.y > 0.9f && begin->approach_speed > 3.0f && begin->approach_speed < 5.5f);
    auto floor_begin = std::find_if(s.log.begin(), s.log.end(), [&](const PhysicsEvent& e) {
        return e.self == floor && e.type == PhysicsEventType::CollisionBegin;
    });
    AETHER_CHECK(floor_begin != s.log.end() && floor_begin->normal.y < -0.9f && std::fabs(floor_begin->point.y - 0.5f) < 0.1f);

    // Resting until it sleeps: no End (Jolt drops sleeping contacts; we don't).
    s.Run(300);
    AETHER_CHECK(!s.physics.BodyInterface().IsActive(s.scene.BodyOf(dumbbell)));
    AETHER_CHECK(s.Count(floor, PhysicsEventType::CollisionEnd) == 0 && s.Count(floor, PhysicsEventType::CollisionBegin) == 1);

    // Lifted away: End, once, on both sides.
    s.world.GetComponent<Transform>(dumbbell)->position = Vec3(0, 6, 0);
    s.Run(2);
    AETHER_CHECK(s.Count(floor, PhysicsEventType::CollisionEnd, dumbbell) == 1);
    AETHER_CHECK(s.Count(dumbbell, PhysicsEventType::CollisionEnd, floor) == 1);
    // And it lands again: a second Begin.
    s.Run(90);
    AETHER_CHECK(s.Count(dumbbell, PhysicsEventType::CollisionBegin, floor) == 2);
    AETHER_CHECK(s.physics.DroppedContacts() == 0);
}

AETHER_TEST(PhysicsEvents_StayIsOptInAndOncePerStep) {
    Sim s;
    const Entity floor = s.Floor();
    const Entity quiet = s.Ball(Vec3(-2, 1.2f, 0));
    const Entity chatty = s.Ball(Vec3(2, 1.2f, 0), 0.5f, /*stay=*/true);
    usize most_in_one_step = 0;
    for (int i = 0; i < 60; ++i) {
        const usize before = s.Count(chatty, PhysicsEventType::CollisionStay, floor);
        s.scene.Step(kDt);
        most_in_one_step = std::max(most_in_one_step, s.Count(chatty, PhysicsEventType::CollisionStay, floor) - before);
    }
    const usize stays = s.Count(chatty, PhysicsEventType::CollisionStay, floor);
    // Every step while it's awake and touching (about half a second before
    // it sleeps; sleeping bodies report nothing), and never twice in a step.
    AETHER_CHECK(stays >= 20 && most_in_one_step == 1);
    AETHER_CHECK(s.Count(floor, PhysicsEventType::CollisionStay, chatty) == stays);
    AETHER_CHECK(s.Count(quiet, PhysicsEventType::CollisionStay) == 0 && s.Count(floor, PhysicsEventType::CollisionStay, quiet) == 0);
}

AETHER_TEST(PhysicsEvents_TriggersEnterAndExit) {
    Sim s;
    BoxCollider zone;
    zone.half_extents = Vec3(2, 0.5f, 2);
    zone.is_trigger = true;
    zone.report_stay = true; // triggers never Stay, even when asked
    const Entity trigger = s.world.CreateEntity(Transform{Vec3(0, 3, 0), Quaternion::Identity()}, zone);
    const Entity ball = s.Ball(Vec3(0, 6, 0), 0.25f);
    s.Run(120); // falls through and away (there's no floor)
    AETHER_CHECK(s.Count(trigger, PhysicsEventType::TriggerEnter, ball) == 1 && s.Count(ball, PhysicsEventType::TriggerEnter, trigger) == 1);
    AETHER_CHECK(s.Count(trigger, PhysicsEventType::TriggerExit, ball) == 1 && s.Count(ball, PhysicsEventType::TriggerExit, trigger) == 1);
    AETHER_CHECK(s.Count(trigger, PhysicsEventType::CollisionBegin) == 0 && s.Count(trigger, PhysicsEventType::CollisionStay) == 0);
    // Enter comes before Exit.
    auto enter = std::find_if(s.log.begin(), s.log.end(), [&](const PhysicsEvent& e) { return e.type == PhysicsEventType::TriggerEnter; });
    auto exit = std::find_if(s.log.begin(), s.log.end(), [&](const PhysicsEvent& e) { return e.type == PhysicsEventType::TriggerExit; });
    AETHER_CHECK(enter < exit);
    AETHER_CHECK(std::string(PhysicsEventName(PhysicsEventType::TriggerEnter)) == "Event.OnTriggerEnter");
}

AETHER_TEST(PhysicsEvents_DestroyedEntitiesStillEndAndAreSkipped) {
    Sim s;
    const Entity floor = s.Floor();
    const Entity ball = s.Ball(Vec3(0, 1.2f, 0));
    s.Run(30);
    AETHER_CHECK(s.Count(floor, PhysicsEventType::CollisionBegin, ball) == 1);

    // Destroyed while touching: the floor still hears the End, with the gone entity as `other`.
    s.world.DestroyEntity(ball);
    s.Run(1);
    AETHER_CHECK(s.Count(floor, PhysicsEventType::CollisionEnd, ball) == 1 && s.Count(ball, PhysicsEventType::CollisionEnd) == 0);
    AETHER_CHECK(s.scene.BodyCount() == 1);

    // A handler that destroys an entity: that entity's own events later in
    // the same dispatch are skipped. The floor's event comes first (lower
    // body ID), and destroys the ball it hit.
    const Entity doomed = s.Ball(Vec3(3, 1.2f, 0));
    s.log.clear();
    s.scene.SetEventHandler([&](const PhysicsEvent& e) {
        s.log.push_back(e);
        if (e.self == floor && e.type == PhysicsEventType::CollisionBegin && s.world.IsAlive(e.other)) s.world.DestroyEntity(e.other);
    });
    s.Run(30);
    AETHER_CHECK(s.Count(floor, PhysicsEventType::CollisionBegin, doomed) == 1);
    AETHER_CHECK(s.Count(doomed, PhysicsEventType::CollisionBegin) == 0); // skipped: destroyed earlier in the dispatch
    AETHER_CHECK(!s.world.IsAlive(doomed) && s.scene.BodyCount() == 1);
    AETHER_CHECK(s.Count(floor, PhysicsEventType::CollisionEnd, doomed) == 1); // its body went at the next step
}

AETHER_TEST(PhysicsEvents_AreDeterministicAcrossRuns) {
    // The same pile of 40 balls, 10 times, on 4 worker threads: the same
    // events in the same order every time.
    auto run = [] {
        Sim s(4);
        s.Floor();
        BoxCollider zone;
        zone.half_extents = Vec3(3, 1, 3);
        zone.is_trigger = true;
        s.world.CreateEntity(Transform{Vec3(0, 2, 0), Quaternion::Identity()}, zone);
        for (int i = 0; i < 40; ++i) {
            const f32 x = static_cast<f32>(i % 5) * 0.45f - 0.9f, z = static_cast<f32>((i / 5) % 4) * 0.45f - 0.7f;
            s.Ball(Vec3(x, 3.0f + static_cast<f32>(i) * 0.3f, z), 0.2f);
        }
        std::ostringstream out;
        for (int step = 0; step < 240; ++step) {
            s.log.clear();
            s.scene.Step(kDt);
            for (const PhysicsEvent& e : s.log) out << step << ':' << static_cast<int>(e.type) << ':' << e.self.index << '>' << e.other.index << ' ';
        }
        return out.str();
    };
    const std::string first = run();
    AETHER_CHECK(first.size() > 1000); // plenty happened
    for (int i = 1; i < 10; ++i) AETHER_CHECK(run() == first);
}

AETHER_TEST(PhysicsEvents_DriveBlueprintEvents) {
    // BP_Coin (assets/blueprints) collected by a falling player ball through
    // a real trigger, with the events dispatched to Blueprints.
    Sim s;
    bp::Blueprint coin_bp;
    AETHER_CHECK(bp::LoadBlueprint(std::string(AETHER_REPO_ASSETS_DIR) + "/blueprints/BP_Coin.abp", coin_bp));
    const bp::CompileResult coin = bp::CompileBlueprint(coin_bp);
    AETHER_CHECK(coin.Ok());
    bp::BlueprintVM vm(s.world);
    s.scene.SetEventHandler([&](const PhysicsEvent& e) {
        const bp::VmValue other[] = {e.other};
        vm.Dispatch(e.self, PhysicsEventName(e.type), other); // entities without the event just ignore it
    });

    s.Floor();
    SphereCollider pickup;
    pickup.radius = 0.4f;
    pickup.is_trigger = true;
    const Entity c = s.world.CreateEntity(Transform{Vec3(0, 1.0f, 0), Quaternion::Identity()}, pickup);
    AETHER_CHECK(vm.Attach(c, coin.blueprint));
    const Entity player = s.Ball(Vec3(0, 4, 0), 0.3f);
    AddTag(s.world, player, "Player");
    const Entity rock = s.Ball(Vec3(3, 4, 0), 0.3f); // not the player: rolls on
    (void)rock;
    vm.BeginPlay();
    s.Run(90);
    AETHER_CHECK(!s.world.IsAlive(c) && !vm.IsAttached(c)); // collected, destroyed by its Blueprint
    AETHER_CHECK(s.world.IsAlive(player) && s.scene.BodyCount() == 3); // floor, player, rock
    AETHER_CHECK(vm.Errors().empty());
}
