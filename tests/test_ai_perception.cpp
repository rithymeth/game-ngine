#include "aether/ai/bt_world.h"
#include "aether/ai/perception.h"
#include "aether/blueprint/compiler.h"
#include "aether/blueprint/vm.h"
#include "aether/nav/components.h"
#include "test_framework.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

// Phase 20 step 5: AI perception - sight (radius, keeping track, the view
// cone, line of sight by callback or over the navigation mesh), hearing,
// damage, teams, forgetting, what the AI is most aware of, writing it to
// a Behavior Tree's blackboard, and the Blueprint events and nodes.

using namespace aether;
using namespace aether::ai;

#define CHECK(...) AETHER_CHECK((__VA_ARGS__))

namespace {

constexpr f32 kDt = 0.1f;

Transform At(const Vec3& p, f32 yaw_degrees = 0.0f) {
    return Transform{p, Quaternion::FromAxisAngle(Vec3(0, 1, 0), yaw_degrees * 3.14159265f / 180.0f)};
}

struct Scene {
    World world;
    PerceptionWorld perception{world};
    std::vector<PerceptionEvent> events;

    Entity Guard(const Vec3& at = {}, AIPerception p = {}, f32 yaw = 0.0f) { return world.CreateEntity(At(at, yaw), p); }
    Entity Thing(const Vec3& at, AIStimuliSource s = {}) { return world.CreateEntity(At(at), s); }
    AIPerception& Of(Entity e) { return *world.GetComponent<AIPerception>(e); }
    void Move(Entity e, const Vec3& p) { world.GetComponent<Transform>(e)->position = p; }
    void Step(int frames = 1) {
        for (int i = 0; i < frames; ++i) {
            perception.Update(kDt);
            events.insert(events.end(), perception.Events().begin(), perception.Events().end());
        }
    }
    usize Count(PerceptionSense sense, bool sensed, bool forgotten = false) const {
        return static_cast<usize>(std::count_if(events.begin(), events.end(), [&](const PerceptionEvent& e) {
            return e.sense == sense && e.sensed == sensed && e.forgotten == forgotten;
        }));
    }
};

bool Near(const Vec3& a, const Vec3& b, f32 tol = 1e-3f) { return (a - b).Length() <= tol; }

} // namespace

AETHER_TEST(AIPerception_Sight) {
    Scene s;
    const Entity guard = s.Guard();
    const Entity thief = s.Thing({0, 0, 10});
    s.Step();
    CHECK(s.Of(guard).CanSee(thief));
    CHECK(s.Count(PerceptionSense::Sight, true) == 1);
    CHECK(!s.events.empty() && s.events[0].perceiver == guard && s.events[0].actor == thief);
    s.Step(5);
    CHECK(s.Count(PerceptionSense::Sight, true) == 1); // only when first seen
    // Beyond the sight radius but within the lose-sight radius: still tracked.
    s.Move(thief, {0, 0, 16});
    s.Step();
    CHECK(s.Of(guard).CanSee(thief));
    s.Move(thief, {0, 0, 19});
    s.Step();
    CHECK(!s.Of(guard).CanSee(thief));
    CHECK(s.Of(guard).IsAwareOf(thief));
    CHECK(s.Count(PerceptionSense::Sight, false) == 1);
    CHECK(Near(s.Of(guard).GetLastKnownLocation(thief), Vec3(0, 0, 16)));
    // Coming back to 16 isn't enough to notice again (the sight radius is 15).
    s.Move(thief, {0, 0, 16});
    s.Step();
    CHECK(!s.Of(guard).CanSee(thief));
    s.Move(thief, {0, 0, 14});
    s.Step();
    CHECK(s.Of(guard).CanSee(thief));
    CHECK(s.Count(PerceptionSense::Sight, true) == 2);

    // The cone: 90 degrees about +Z.
    const f32 r = 5.0f, a40 = 40.0f * 3.14159265f / 180.0f, a50 = 50.0f * 3.14159265f / 180.0f;
    const Entity inside = s.Thing({r * std::sin(a40), 0, r * std::cos(a40)});
    const Entity outside = s.Thing({r * std::sin(a50), 0, r * std::cos(a50)});
    const Entity behind = s.Thing({0, 0, -5});
    s.Step();
    CHECK(s.Of(guard).CanSee(inside));
    CHECK(!s.Of(guard).CanSee(outside));
    CHECK(!s.Of(guard).CanSee(behind));
    // Turning round.
    s.world.GetComponent<Transform>(guard)->rotation = Quaternion::FromAxisAngle(Vec3(0, 1, 0), 3.14159265f);
    s.Step();
    CHECK(s.Of(guard).CanSee(behind));
    CHECK(!s.Of(guard).CanSee(inside));
    // All round.
    s.Of(guard).fov_degrees = 360.0f;
    s.Step();
    CHECK(s.Of(guard).CanSee(inside) && s.Of(guard).CanSee(outside) && s.Of(guard).CanSee(behind));
    // Hidden sources, and not seeing itself.
    s.world.GetComponent<AIStimuliSource>(inside)->visible = false;
    s.world.AddComponent(guard, AIStimuliSource{});
    s.Step();
    CHECK(!s.Of(guard).CanSee(inside));
    CHECK(!s.Of(guard).IsAwareOf(guard));
}

AETHER_TEST(AIPerception_LineOfSight) {
    Scene s;
    // A wall across z = 5 for |x| < 2.
    s.perception.SetLineOfSight([](const Vec3& from, const Vec3& to, Entity, Entity) {
        if ((from.z - 5.0f) * (to.z - 5.0f) > 0.0f) return true;
        const f32 t = (5.0f - from.z) / (to.z - from.z);
        return std::fabs(from.x + (to.x - from.x) * t) >= 2.0f;
    });
    const Entity guard = s.Guard();
    const Entity thief = s.Thing({0, 0, 10});
    s.Step();
    CHECK(!s.Of(guard).CanSee(thief));
    s.Move(thief, {4, 0, 10});
    s.Step();
    CHECK(s.Of(guard).CanSee(thief));
    s.Move(thief, {0, 0, 3});
    s.Step();
    CHECK(s.Of(guard).CanSee(thief));

    // Over the navigation mesh: a wall on the floor blocks.
    World& w = s.world;
    ModelRenderer floor;
    SetModelPath(floor, "floor");
    ModelRenderer wall;
    SetModelPath(wall, "wall");
    w.CreateEntity(At({0, 0, 0}), floor);
    w.CreateEntity(At({0, 0, 20}), wall);
    nav::NavWorld nav(w, [](const ModelRenderer& m, std::vector<Vec3>& v, std::vector<u32>& i) {
        nav::NavGeometry g;
        if (std::strcmp(m.asset_path, "floor") == 0) g.AddPlane(Vec3(0, 0, 20), 10, 10);
        else if (std::strcmp(m.asset_path, "wall") == 0) g.AddBox(Vec3(0, 1.5f, 0), Vec3(2, 1.5f, 0.5f));
        else return false;
        v = g.vertices;
        i = g.indices;
        return true;
    });
    CHECK(nav.Bake({}));
    s.perception.UseNavMeshLineOfSight(nav.Mesh());
    const Entity sentry = s.Guard({0, 0, 15});
    const Entity sneak = s.Thing({0, 0, 25});
    s.Step();
    CHECK(!s.Of(sentry).CanSee(sneak));
    s.Move(sneak, {9, 0, 25}); // passing the wall's end (widened by the agent radius)
    s.Step();
    CHECK(s.Of(sentry).CanSee(sneak));
}

AETHER_TEST(AIPerception_HearingAndDamage) {
    Scene s;
    const Entity guard = s.Guard();
    const Entity thief = s.world.CreateEntity(At({0, 0, -30})); // not visible to anyone: no stimuli source
    // Loud enough behind it: heard, at the noise.
    Perception::ReportNoise(Vec3(0, 0, -15), 1.0f, thief);
    s.Step();
    CHECK(s.Of(guard).IsAwareOf(thief));
    CHECK(!s.Of(guard).CanSee(thief));
    CHECK(Near(s.Of(guard).GetLastKnownLocation(thief), Vec3(0, 0, -15)));
    CHECK(s.Count(PerceptionSense::Hearing, true) == 1);
    const PerceivedActor* k = s.Of(guard).Find(thief);
    CHECK(k != nullptr && k->last_sense == PerceptionSense::Hearing && k->strength == 1.0f);
    // Too quiet for the distance.
    Perception::ReportNoise(Vec3(0, 0, -12), 0.5f, thief);
    s.Step();
    CHECK(s.Count(PerceptionSense::Hearing, true) == 1);
    CHECK(Near(s.Of(guard).GetLastKnownLocation(thief), Vec3(0, 0, -15)));
    // Its own noises, and noises nobody made, aren't heard.
    Perception::ReportNoise(Vec3(0, 0, 1), 1.0f, guard);
    Perception::ReportNoise(Vec3(0, 0, 1), 1.0f, kNullEntity);
    s.Step();
    CHECK(s.Count(PerceptionSense::Hearing, true) == 1);
    // Damage: it learns who hit it, wherever they are.
    const Entity sniper = s.world.CreateEntity(At({80, 0, -80}));
    Perception::ReportDamage(guard, sniper, 25.0f);
    s.Step();
    CHECK(s.Of(guard).IsAwareOf(sniper));
    CHECK(Near(s.Of(guard).GetLastKnownLocation(sniper), Vec3(80, 0, -80)));
    CHECK(s.Count(PerceptionSense::Damage, true) == 1);
    CHECK(s.Of(guard).Find(sniper)->strength == 25.0f);
    Perception::ReportDamage(thief, sniper, 5.0f); // not a perceiver: nothing
    s.Step();
    CHECK(s.Count(PerceptionSense::Damage, true) == 1);
    CHECK(s.Of(guard).GetKnownCount() == 2);
}

AETHER_TEST(AIPerception_TeamsAndForgetting) {
    Scene s;
    AIPerception red;
    red.team = 1;
    red.forget_after = 2.0f;
    const Entity guard = s.Guard({}, red);
    AIStimuliSource friend_src;
    friend_src.team = 1;
    AIStimuliSource foe_src;
    foe_src.team = 2;
    const Entity pal = s.Thing({0, 0, 5}, friend_src);
    const Entity foe = s.Thing({1, 0, 5}, foe_src);
    const Entity stray = s.Thing({-1, 0, 5}); // no team
    s.Step();
    CHECK(!s.Of(guard).CanSee(pal));
    CHECK(s.Of(guard).CanSee(foe));
    CHECK(s.Of(guard).CanSee(stray));
    s.Of(guard).detect_friends = true;
    s.Step();
    CHECK(s.Of(guard).CanSee(pal));
    // Out of sight for more than two seconds: forgotten.
    s.Move(foe, {0, 0, -10});
    s.Step();
    CHECK(s.Of(guard).IsAwareOf(foe) && !s.Of(guard).CanSee(foe));
    s.Step(18);
    CHECK(s.Of(guard).IsAwareOf(foe));
    s.Step(3);
    CHECK(!s.Of(guard).IsAwareOf(foe));
    CHECK(s.Count(PerceptionSense::Sight, false, true) == 1);
    // Destroyed: forgotten at once.
    s.world.DestroyEntity(stray);
    s.Step();
    CHECK(!s.Of(guard).IsAwareOf(stray));
    CHECK(s.Count(PerceptionSense::Sight, false, true) == 2);
    // Never forgetting.
    s.Of(guard).forget_after = 0.0f;
    s.Move(pal, {0, 0, -10});
    s.Step(100);
    CHECK(s.Of(guard).IsAwareOf(pal));
    s.Of(guard).ForgetAll();
    CHECK(s.Of(guard).GetKnownCount() == 0);
}

AETHER_TEST(AIPerception_TargetPriority) {
    Scene s;
    const Entity guard = s.Guard();
    const Entity far_one = s.Thing({0, 0, 12});
    const Entity near_one = s.Thing({0, 0, 4});
    const Entity heard = s.world.CreateEntity(At({0, 0, -3}));
    Perception::ReportNoise(Vec3(0, 0, -3), 1.0f, heard);
    s.Step();
    CHECK(s.Of(guard).GetTarget() == near_one); // seen beats heard; nearer beats farther
    s.world.GetComponent<AIStimuliSource>(near_one)->visible = false;
    s.Step();
    CHECK(s.Of(guard).GetTarget() == far_one);
    s.world.GetComponent<AIStimuliSource>(far_one)->visible = false;
    s.Step();
    // Nothing in sight: what it sensed most recently (both were seen a frame ago; the far one later).
    CHECK(s.Of(guard).GetTarget() == far_one);
    Perception::ReportNoise(Vec3(0, 0, -3), 1.0f, heard);
    s.Step();
    CHECK(s.Of(guard).GetTarget() == heard);
    CHECK(Near(s.Of(guard).GetLastKnownLocation(Entity{}), Vec3()));
}

AETHER_TEST(AIPerception_BlackboardAndBehaviorTree) {
    World world;
    PerceptionWorld perception(world);
    BehaviorTreeAsset tree;
    tree.blackboard.Add("Enemy", BlackboardType::Entity);
    tree.blackboard.Add("EnemyAt", BlackboardType::Vector);
    BtNode chase;
    chase.type = BtNodeType::Sequence;
    BtNode log_chase;
    log_chase.type = BtNodeType::Log;
    log_chase.event = "chase";
    BtNode wait;
    wait.type = BtNodeType::Wait;
    wait.seconds = 100;
    chase.children = {log_chase, wait};
    BtDecorator seen;
    seen.key = "Enemy", seen.op = BtCompare::IsSet, seen.abort = BtAbort::Both;
    chase.decorators = {seen};
    BtNode patrol = chase;
    patrol.decorators.clear();
    patrol.children[0].event = "patrol";
    tree.root.type = BtNodeType::Selector;
    tree.root.children = {chase, patrol};
    CHECK(ValidateBehaviorTree(tree).empty());
    BehaviorTreeWorld bt(world, [&](const std::string&) { return &tree; });
    std::vector<std::string> logs;
    bt.hooks.log = [&](Entity, const std::string& t) { logs.push_back(t); };

    AIPerception p;
    p.target_key = "Enemy";
    p.location_key = "EnemyAt";
    p.forget_after = 1.0f;
    BehaviorTreeComponent brain;
    brain.tree = "Guard";
    const Entity guard = world.CreateEntity(At({}), p, brain);
    const Entity thief = world.CreateEntity(At({0, 0, 30}), AIStimuliSource{});
    const auto step = [&](int n) {
        for (int i = 0; i < n; ++i) {
            perception.Update(kDt);
            bt.Update(kDt);
        }
    };
    step(3);
    CHECK(logs == std::vector<std::string>{"patrol"});
    world.GetComponent<Transform>(thief)->position = Vec3(0, 0, 8);
    step(1);
    const BehaviorTreeComponent& b = *world.GetComponent<BehaviorTreeComponent>(guard);
    CHECK(b.GetEntity("Enemy") == thief);
    CHECK(Near(b.GetVector("EnemyAt"), Vec3(0, 0, 8)));
    CHECK(logs.back() == "chase");
    // It slips away: the last place stays until it's forgotten, then the guard patrols again.
    world.GetComponent<Transform>(thief)->position = Vec3(0, 0, -8);
    step(2);
    CHECK(b.GetEntity("Enemy") == thief);
    CHECK(Near(b.GetVector("EnemyAt"), Vec3(0, 0, 8)));
    step(10);
    CHECK(!b.IsValueSet("Enemy") && !b.IsValueSet("EnemyAt"));
    CHECK(logs.back() == "patrol");
}

AETHER_TEST(AIPerception_BlueprintNodes) {
    RegisterPerceptionComponents();
    bp::Blueprint blueprint;
    bp::Graph events;
    events.name = "EventGraph";
    blueprint.graphs.push_back(events);
    bp::GraphBuilder b(*blueprint.FindGraph("EventGraph"));
    // On perceiving: print the sense when sensed, "lost" when not.
    const bp::NodeId perceived = b.Add("Event.OnTargetPerceived");
    const bp::NodeId branch = b.Add("Flow.Branch");
    const bp::NodeId print_sense = b.Add("Debug.Print");
    const bp::NodeId print_lost = b.Add("Debug.Print");
    b.Default(print_lost, "text", "lost");
    b.Connect(perceived, "then", branch, "exec").Connect(perceived, "sensed", branch, "condition");
    b.Connect(branch, "true", print_sense, "exec").Connect(perceived, "sense", print_sense, "text");
    b.Connect(branch, "false", print_lost, "exec");
    const bp::NodeId forgotten = b.Add("Event.OnTargetForgotten");
    const bp::NodeId print_forgot = b.Add("Debug.Print");
    b.Default(print_forgot, "text", "forgot");
    b.Connect(forgotten, "then", print_forgot, "exec");
    bp::CompileResult compiled = bp::CompileBlueprint(blueprint);
    for (const bp::Diagnostic& d : compiled.diagnostics.diagnostics) std::printf("    %s: %s\n", d.code.c_str(), d.message.c_str());
    CHECK(compiled.Ok());
    // A noisy one: Begin Play makes a noise where it stands.
    bp::Blueprint noisy;
    bp::Graph ng;
    ng.name = "EventGraph";
    noisy.graphs.push_back(ng);
    bp::GraphBuilder nb(*noisy.FindGraph("EventGraph"));
    const bp::NodeId begin = nb.Add("Event.BeginPlay");
    const bp::NodeId noise = nb.Add("Call.Native:Perception.ReportNoise");
    const bp::NodeId self = nb.Add("Entity.Self");
    nb.Default(noise, "location", nlohmann::json::array({0, 0, -10})).Default(noise, "loudness", 1.0);
    nb.Connect(begin, "then", noise, "exec").Connect(self, "self", noise, "instigator");
    bp::CompileResult compiled_noisy = bp::CompileBlueprint(noisy);
    CHECK(compiled_noisy.Ok());

    World world;
    PerceptionWorld perception(world);
    bp::BlueprintVM vm(world);
    std::vector<std::string> printed;
    vm.SetPrintHandler([&](Entity, const std::string& text) { printed.push_back(text); });
    AIPerception p;
    p.forget_after = 0.5f;
    const Entity guard = world.CreateEntity(At({}), p);
    const Entity thief = world.CreateEntity(At({0, 0, 5}), AIStimuliSource{});
    const Entity drummer = world.CreateEntity(At({0, 0, -10}));
    CHECK(vm.Attach(guard, compiled.blueprint));
    CHECK(vm.Attach(drummer, compiled_noisy.blueprint));
    vm.BeginPlay();
    const auto step = [&] {
        perception.Update(kDt);
        for (const PerceptionEvent& ev : perception.Events()) {
            if (ev.forgotten) {
                const bp::VmValue args[] = {ev.actor};
                vm.Dispatch(ev.perceiver, PerceptionWorld::kForgottenEvent, args);
            } else {
                const bp::VmValue args[] = {ev.actor, std::string(PerceptionWorld::SenseName(ev.sense)), ev.sensed};
                vm.Dispatch(ev.perceiver, PerceptionWorld::kPerceivedEvent, args);
            }
        }
    };
    step();
    std::vector<std::string> sorted = printed;
    std::sort(sorted.begin(), sorted.end());
    CHECK((sorted == std::vector<std::string>{"Hearing", "Sight"}));
    world.GetComponent<AIStimuliSource>(thief)->visible = false;
    for (int i = 0; i < 10; ++i) step();
    CHECK(std::count(printed.begin(), printed.end(), "lost") == 1);
    CHECK(std::count(printed.begin(), printed.end(), "forgot") == 2); // the thief and the drummer
}
