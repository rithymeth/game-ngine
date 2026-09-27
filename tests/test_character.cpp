#include "aether/blueprint/compiler.h"
#include "aether/blueprint/vm.h"
#include "aether/job/job_system.h"
#include "aether/physics/character.h"
#include "test_framework.h"

#include <algorithm>
#include <cmath>

using namespace aether;

// Phase 13 step 5: CharacterMovement on Jolt's CharacterVirtual: walking,
// slopes, steps, jumping with coyote time and a jump buffer, triggers and
// moving platforms.

namespace {

constexpr f32 kDt = 1.0f / 60.0f;
constexpr f32 kPi = 3.14159265f;

struct Sim {
    JobSystem jobs{2};
    World world;
    PhysicsWorld physics{jobs};
    PhysicsScene scene{world, physics};
    CharacterSystem characters{world, physics, &scene};
    std::vector<CharacterEvent> log;

    Sim() {
        characters.SetEventHandler([this](const CharacterEvent& e) { log.push_back(e); });
    }
    Entity Box(const Vec3& center, const Vec3& half, const Quaternion& rotation = Quaternion::Identity()) {
        BoxCollider box;
        box.half_extents = half;
        return world.CreateEntity(Transform{center, rotation}, box);
    }
    Entity Floor() { return Box(Vec3(0, 0, 0), Vec3(20, 0.5f, 20)); } // top at y = 0.5
    Entity Character(const Vec3& feet) { return world.CreateEntity(Transform{feet, Quaternion::Identity()}, CharacterMovement{}); }
    void Step() {
        characters.Step(kDt);
        scene.Step(kDt);
    }
    void Run(int steps) {
        for (int i = 0; i < steps; ++i) Step();
    }
    // Holds `input` for `steps`.
    void Walk(Entity c, const Vec3& input, int steps, bool run = false) {
        for (int i = 0; i < steps; ++i) {
            CharacterMovement& m = *world.GetComponent<CharacterMovement>(c);
            m.AddInput(input);
            m.run = run;
            Step();
        }
    }
    CharacterMovement& Move(Entity c) { return *world.GetComponent<CharacterMovement>(c); }
    Vec3 Feet(Entity c) { return world.GetComponent<Transform>(c)->position; }
    usize Count(CharacterEventType type) const {
        return static_cast<usize>(std::count_if(log.begin(), log.end(), [&](const CharacterEvent& e) { return e.type == type; }));
    }
};

Quaternion AboutZ(f32 degrees) { return Quaternion::FromAxisAngle(Vec3(0, 0, 1), degrees * kPi / 180.0f); }

} // namespace

AETHER_TEST(Character_WalksRunsAndBrakes) {
    Sim s;
    s.Floor();
    const Entity c = s.Character(Vec3(0, 1.5f, 0));
    s.Run(60);
    AETHER_CHECK(s.Move(c).grounded && s.Move(c).mode == MovementMode::Walking);
    AETHER_CHECK(std::fabs(s.Feet(c).y - 0.5f) < 0.05f);
    AETHER_CHECK(s.Count(CharacterEventType::Landed) == 1 && s.Count(CharacterEventType::MovementModeChanged) == 1);
    AETHER_CHECK(s.characters.Count() == 1 && !s.characters.InnerBodyOf(c).IsInvalid());

    // Walking: 20 m/s² up to 4 m/s (0.2 s to get there), about 3.6 m in a second.
    const f32 x0 = s.Feet(c).x;
    s.Walk(c, Vec3(1, 0, 0), 60);
    AETHER_CHECK(std::fabs(s.Move(c).velocity.x - 4.0f) < 0.05f);
    AETHER_CHECK(std::fabs(s.Feet(c).x - x0 - 3.6f) < 0.15f);
    AETHER_CHECK(std::fabs(s.Feet(c).y - 0.5f) < 0.05f && std::fabs(s.Feet(c).z) < 1e-3f);
    // Running, and an over-long input is clamped to 1.
    s.Walk(c, Vec3(0, 0, 5), 60, /*run=*/true);
    AETHER_CHECK(std::fabs(s.Move(c).velocity.z - 7.0f) < 0.05f && std::fabs(s.Move(c).velocity.x) < 0.05f);
    // Braking at 25 m/s²: 7 m/s stops in 0.28 s.
    s.Run(20);
    AETHER_CHECK(std::fabs(s.Move(c).velocity.z) < 1e-3f);
    // Gameplay can teleport it by setting the Transform.
    s.world.GetComponent<Transform>(c)->position = Vec3(5, 3, 5);
    s.Run(60);
    AETHER_CHECK(std::fabs(s.Feet(c).x - 5) < 1e-3f && std::fabs(s.Feet(c).y - 0.5f) < 0.05f);
}

AETHER_TEST(Character_SlopesUpToTheLimit) {
    Sim s;
    s.Floor();
    // Two ramps rising toward +x, lying on the floor: 30° (walkable) and 60° (too steep).
    s.Box(Vec3(0, 0.5f, 0), Vec3(4, 0.25f, 1.5f), AboutZ(30));
    s.Box(Vec3(0, 0.5f, 8), Vec3(4, 0.25f, 1.5f), AboutZ(60));
    const Entity gentle = s.Character(Vec3(0, 3, 0));
    const Entity steep = s.Character(Vec3(0, 5, 8));
    s.Run(150);
    // On 30° it stands still, grounded.
    AETHER_CHECK(s.Move(gentle).grounded && std::fabs(s.Move(gentle).velocity.x) < 0.05f);
    const Vec3 rest = s.Feet(gentle);
    s.Run(120); // two seconds: no creeping down
    AETHER_CHECK(std::fabs(s.Feet(gentle).x - rest.x) < 0.01f && std::fabs(s.Feet(gentle).y - rest.y) < 0.02f);
    // It can walk up it.
    s.Walk(gentle, Vec3(1, 0, 0), 30);
    AETHER_CHECK(s.Feet(gentle).y > rest.y + 0.5f && s.Move(gentle).grounded);
    // On 60° it can't stand: it slid down to where the ramp meets the floor
    // (the ramp's surface crosses y = 0.5 at x ≈ -0.3).
    AETHER_CHECK(s.Feet(steep).x < -0.3f && std::fabs(s.Feet(steep).y - 0.5f) < 0.05f && s.Move(steep).grounded);
    // And walking up it gets nowhere.
    s.Walk(steep, Vec3(1, 0, 0), 60);
    AETHER_CHECK(s.Feet(steep).y < 1.0f);
}

AETHER_TEST(Character_ClimbsStepsButNotWalls) {
    Sim s;
    s.Floor();
    s.Box(Vec3(3, 0.65f, 0), Vec3(1, 0.15f, 2));  // a 0.3 m step, from x = 2
    s.Box(Vec3(3, 0.8f, 6), Vec3(1, 0.3f, 2));    // a 0.6 m wall, from x = 2
    const Entity climber = s.Character(Vec3(0, 0.6f, 0));
    const Entity blocked = s.Character(Vec3(0, 0.6f, 6));
    s.Run(30);
    for (int i = 0; i < 60; ++i) {
        s.Move(climber).AddInput(Vec3(1, 0, 0));
        s.Move(blocked).AddInput(Vec3(1, 0, 0));
        s.Step();
    }
    AETHER_CHECK(s.Feet(climber).x > 2.5f && std::fabs(s.Feet(climber).y - 0.8f) < 0.05f && s.Move(climber).grounded);
    AETHER_CHECK(s.Feet(blocked).x < 2.0f - 0.3f && std::fabs(s.Feet(blocked).y - 0.5f) < 0.05f);
}

AETHER_TEST(Character_JumpsWithCoyoteTimeAndABuffer) {
    {
        Sim s;
        s.Floor();
        const Entity c = s.Character(Vec3(0, 0.6f, 0));
        s.Run(30);
        s.log.clear();
        s.Move(c).Jump();
        f32 top = 0.0f;
        for (int i = 0; i < 90; ++i) {
            s.Step();
            top = std::max(top, s.Feet(c).y);
        }
        AETHER_CHECK(s.Count(CharacterEventType::Jumped) == 1 && s.Count(CharacterEventType::Landed) == 1);
        AETHER_CHECK(std::fabs(top - 0.5f - 25.0f / (2 * 9.81f)) < 0.1f); // v² / 2g ≈ 1.27 m
        auto landed = std::find_if(s.log.begin(), s.log.end(), [](const CharacterEvent& e) { return e.type == CharacterEventType::Landed; });
        AETHER_CHECK(landed != s.log.end() && landed->impact_speed > 4.0f && landed->impact_speed < 6.0f);
        AETHER_CHECK(s.Move(c).grounded);
        // Pressing jump in mid-air does nothing (no double jump).
        s.log.clear();
        s.Move(c).Jump();
        s.Step();
        s.Run(10);
        s.Move(c).Jump();
        s.Step();
        AETHER_CHECK(s.Count(CharacterEventType::Jumped) == 1);
    }

    // Coyote time: walking off a ledge, a jump soon after still works; later it doesn't.
    auto off_the_ledge = [](int delay_steps) {
        Sim s;
        s.Box(Vec3(-5, 0, 0), Vec3(5, 0.5f, 5)); // ends at x = 0
        s.Box(Vec3(0, -20, 0), Vec3(50, 0.5f, 50)); // far below
        const Entity c = s.Character(Vec3(-2, 0.6f, 0));
        s.Run(30);
        s.log.clear();
        int guard = 0;
        while (s.Move(c).grounded && guard++ < 120) s.Walk(c, Vec3(1, 0, 0), 1);
        s.Walk(c, Vec3(1, 0, 0), delay_steps);
        s.Move(c).Jump();
        s.Step();
        return s.Count(CharacterEventType::Jumped);
    };
    AETHER_CHECK(off_the_ledge(2) == 1);  // 0.03 s after leaving
    AETHER_CHECK(off_the_ledge(12) == 0); // 0.2 s: too late

    // Jump buffer: pressed shortly before landing, it jumps on landing.
    auto drop = [](int press_at_step) {
        Sim s;
        s.Floor();
        const Entity c = s.Character(Vec3(0, 3, 0));
        int landed_at = -1;
        for (int i = 0; i < 120; ++i) {
            if (i == press_at_step) s.Move(c).Jump();
            s.Step();
            if (landed_at < 0 && s.Count(CharacterEventType::Landed) > 0) landed_at = i;
        }
        return std::make_pair(landed_at, s.Count(CharacterEventType::Jumped));
    };
    const int landing = drop(-1).first; // when it lands with no jump
    AETHER_CHECK(landing > 10);
    AETHER_CHECK(drop(landing - 3).second == 1);  // 0.05 s early: buffered
    AETHER_CHECK(drop(landing - 15).second == 0); // 0.25 s early: forgotten
}

AETHER_TEST(Character_TriggersAndMovingPlatforms) {
    Sim s;
    s.Floor();
    std::vector<PhysicsEvent> physics_events;
    s.scene.SetEventHandler([&](const PhysicsEvent& e) { physics_events.push_back(e); });
    BoxCollider zone;
    zone.half_extents = Vec3(0.5f, 1, 2);
    zone.is_trigger = true;
    const Entity trigger = s.world.CreateEntity(Transform{Vec3(2, 1.5f, 0), Quaternion::Identity()}, zone);
    const Entity walker = s.Character(Vec3(0, 0.6f, 0));
    s.Run(10);
    s.Walk(walker, Vec3(1, 0, 0), 90); // through the trigger and out
    auto count = [&](PhysicsEventType type) {
        return std::count_if(physics_events.begin(), physics_events.end(),
                             [&](const PhysicsEvent& e) { return e.self == trigger && e.other == walker && e.type == type; });
    };
    AETHER_CHECK(count(PhysicsEventType::TriggerEnter) == 1 && count(PhysicsEventType::TriggerExit) == 1);
    // Queries see the character too.
    AETHER_CHECK(s.scene.RayCast(s.Feet(walker) + Vec3(0, 5, 0), s.Feet(walker) + Vec3(0, 0.1f, 0)).entity == walker);

    // A kinematic platform moving at 1 m/s carries a character standing on it.
    BoxCollider deck;
    deck.half_extents = Vec3(2, 0.1f, 2);
    RigidBody kinematic;
    kinematic.motion = BodyMotion::Kinematic;
    const Entity platform = s.world.CreateEntity(Transform{Vec3(0, 1, 10), Quaternion::Identity()}, kinematic, deck);
    const Entity rider = s.Character(Vec3(0, 1.2f, 10));
    s.Run(30);
    AETHER_CHECK(s.Move(rider).grounded);
    const f32 start = s.Feet(rider).x;
    for (int i = 0; i < 60; ++i) {
        s.world.GetComponent<Transform>(platform)->position.x += 1.0f * kDt;
        s.Step();
    }
    AETHER_CHECK(std::fabs(s.Feet(rider).x - start - 1.0f) < 0.15f && s.Move(rider).grounded);

    // Removing the component removes the character (and its inner body).
    s.world.RemoveComponent<CharacterMovement>(rider);
    s.Step();
    AETHER_CHECK(s.characters.Count() == 1 && s.characters.InnerBodyOf(rider).IsInvalid());
    AETHER_CHECK(std::string(CharacterEventName(CharacterEventType::Landed)) == "Event.OnLanded");
}

AETHER_TEST(Character_EventsReachBlueprints) {
    Sim s;
    s.Floor();
    const Entity c = s.Character(Vec3(0, 3, 0));
    // OnLanded keeps the impact speed; OnMovementModeChanged the new mode.
    bp::Blueprint b;
    bp::Graph g;
    g.name = "EventGraph";
    b.graphs.push_back(g);
    for (const auto& [name, type] : {std::pair<const char*, const char*>{"Impact", "float"}, {"Mode", "int"}}) {
        bp::Variable v;
        v.name = name;
        v.type = *bp::ParseType(type);
        v.default_value = bp::DefaultValue(v.type);
        b.variables.push_back(v);
    }
    bp::GraphBuilder gb(b.graphs[0]);
    const bp::NodeId landed = gb.Add("Event.OnLanded"), set_impact = gb.Add("Var.Set:Impact");
    gb.Connect(landed, "then", set_impact, "exec").Connect(landed, "impact_speed", set_impact, "value");
    const bp::NodeId mode = gb.Add("Event.OnMovementModeChanged"), set_mode = gb.Add("Var.Set:Mode");
    gb.Connect(mode, "then", set_mode, "exec").Connect(mode, "new_mode", set_mode, "value");
    const bp::CompileResult compiled = bp::CompileBlueprint(b);
    AETHER_CHECK(compiled.Ok());
    if (!compiled.Ok()) return;
    bp::BlueprintVM vm(s.world);
    AETHER_CHECK(vm.Attach(c, compiled.blueprint));
    s.characters.SetEventHandler([&](const CharacterEvent& e) {
        if (e.type == CharacterEventType::Landed) {
            const bp::VmValue args[] = {e.impact_speed};
            vm.Dispatch(e.entity, CharacterEventName(e.type), args);
        } else if (e.type == CharacterEventType::MovementModeChanged) {
            const bp::VmValue args[] = {static_cast<i32>(e.to)};
            vm.Dispatch(e.entity, CharacterEventName(e.type), args);
        } else {
            vm.Dispatch(e.entity, CharacterEventName(e.type));
        }
    });
    s.Run(60);
    AETHER_CHECK(std::get<f32>(vm.GetVariable(c, "Impact")) > 5.0f); // a 2.5 m fall: about 7 m/s
    AETHER_CHECK(std::get<i32>(vm.GetVariable(c, "Mode")) == static_cast<i32>(MovementMode::Walking));
    AETHER_CHECK(vm.Errors().empty());
}
