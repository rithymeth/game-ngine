#include "aether/ai/bt_world.h"
#include "aether/blueprint/compiler.h"
#include "aether/blueprint/vm.h"
#include "aether/nav/crowd.h"
#include "test_framework.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>

// Phase 20 step 4: Blackboards and Behavior Trees - the asset format and
// checks, composites, decorators (conditions that abort, cooldowns, loops,
// time limits), services, tasks (waits, blackboard edits, Move To with
// NavAgents, Blueprint and Luau tasks through hooks), the component and
// world, and the Blueprint nodes.

using namespace aether;
using namespace aether::ai;

#define CHECK(...) AETHER_CHECK((__VA_ARGS__))

namespace {

bool Near(f32 a, f32 b, f32 tol) { return std::fabs(a - b) <= tol; }

BtNode N(BtNodeType type, std::vector<BtNode> children = {}, const std::string& name = {}) {
    BtNode n;
    n.type = type;
    n.children = std::move(children);
    n.name = name;
    return n;
}
BtNode Wait(f32 seconds) {
    BtNode n = N(BtNodeType::Wait);
    n.seconds = seconds;
    return n;
}
BtNode Log(const std::string& text) {
    BtNode n = N(BtNodeType::Log);
    n.event = text;
    return n;
}
BtNode With(BtNode n, BtDecorator d) {
    n.decorators.push_back(std::move(d));
    return n;
}
BtDecorator Cond(const std::string& key, BtCompare op = BtCompare::IsSet, BtAbort abort = BtAbort::None, BlackboardValue value = {}) {
    BtDecorator d;
    d.key = key, d.op = op, d.abort = abort, d.value = std::move(value);
    return d;
}
BtDecorator Deco(BtDecoratorType type, f32 seconds = 1.0f, i32 count = 0) {
    BtDecorator d;
    d.type = type, d.seconds = seconds, d.count = count;
    return d;
}

// Runs one tree against its own blackboard (no world).
struct Runner {
    BehaviorTreeAsset tree;
    Blackboard bb;
    BtHooks hooks;
    std::vector<std::string> logs;
    std::unique_ptr<BehaviorTreeInstance> inst;

    explicit Runner(BehaviorTreeAsset t) : tree(std::move(t)) {
        bb.Adopt(tree.blackboard);
        hooks.log = [this](Entity, const std::string& text) { logs.push_back(text); };
        inst = std::make_unique<BehaviorTreeInstance>(tree);
    }
    BtStatus Tick(f32 dt = 0.1f) {
        BtContext ctx{nullptr, kNullEntity, &bb, &hooks};
        return inst->Tick(ctx, dt);
    }
    void Run(f32 seconds, f32 dt = 0.1f) {
        for (int i = 0, n = static_cast<int>(std::lround(seconds / dt)); i < n; ++i) Tick(dt);
    }
    usize Count(const std::string& text) const { return static_cast<usize>(std::count(logs.begin(), logs.end(), text)); }
    std::string Last() const { return logs.empty() ? "" : logs.back(); }
};

BehaviorTreeAsset Tree(BtNode root, BlackboardAsset bb = {}) {
    BehaviorTreeAsset t;
    t.name = "Test";
    t.root = std::move(root);
    t.blackboard = std::move(bb);
    return t;
}

bool HasCode(const std::vector<BtProblem>& ps, const std::string& code) {
    return std::any_of(ps.begin(), ps.end(), [&](const BtProblem& p) { return p.code == code; });
}

} // namespace

AETHER_TEST(AI_Blackboard) {
    BlackboardAsset asset;
    asset.Add("Alarmed", BlackboardType::Bool, false);
    asset.Add("Ammo", BlackboardType::Int, 6);
    asset.Add("Speed", BlackboardType::Float);
    asset.Add("Name", BlackboardType::String);
    asset.Add("Goal", BlackboardType::Vector);
    asset.Add("Target", BlackboardType::Entity);
    Blackboard bb;
    CHECK(!bb.HasKeys());
    CHECK(bb.Set("Anything", 3)); // ad hoc before a tree is known
    CHECK(bb.Set("Goal", Vec3(1, 2, 3)));
    CHECK(bb.Set("Ammo", std::string("lots"))); // the wrong type for the tree's key: dropped on Adopt
    bb.Adopt(asset);
    CHECK(bb.HasKeys());
    CHECK(!bb.IsSet("Anything"));
    CHECK(bb.GetAs<Vec3>("Goal").y == 2.0f); // kept
    CHECK(bb.GetAs<i32>("Ammo") == 6);       // the initial value
    CHECK(bb.IsSet("Alarmed") && !bb.GetAs<bool>("Alarmed", true));
    CHECK(!bb.IsSet("Speed"));
    // Types are checked; unknown keys are refused.
    CHECK(!bb.Set("Ammo", 1.5f));
    CHECK(!bb.Set("Nope", true));
    CHECK(bb.Set("Speed", 2.5f));
    f64 n = 0;
    CHECK(bb.GetNumber("Speed", n) && n == 2.5);
    CHECK(bb.GetNumber("Ammo", n) && n == 6.0);
    CHECK(!bb.GetNumber("Name", n));
    // Revisions: a real change moves them, the same value doesn't.
    const u64 r = bb.Revision("Speed"), all = bb.Revision();
    CHECK(bb.Set("Speed", 2.5f));
    CHECK(bb.Revision("Speed") == r && bb.Revision() == all);
    CHECK(bb.Set("Speed", 3.0f));
    CHECK(bb.Revision("Speed") > r);
    const u64 before_clear = bb.Revision("Speed");
    CHECK(bb.Clear("Speed"));
    CHECK(!bb.IsSet("Speed") && bb.Revision("Speed") > before_clear);
    CHECK(!bb.Clear("Nope"));
    // Setting unset clears.
    CHECK(bb.Set("Ammo", BlackboardValue{}));
    CHECK(!bb.IsSet("Ammo"));
    CHECK(bb.Names() == std::vector<std::string>{"Alarmed", "Goal"});
    // Values as text and JSON.
    CHECK(ValueToString(Vec3(1, 2, 3)) == "(1, 2, 3)");
    CHECK(ValueToString(BlackboardValue{}) == "(unset)");
    BlackboardValue v;
    std::string error;
    CHECK(ValueFromJson(nlohmann::json::array({1, 2, 3}), BlackboardType::Vector, v, &error));
    CHECK(ValuesEqual(v, Vec3(1, 2, 3)));
    CHECK(!ValueFromJson(1.5, BlackboardType::Int, v, &error));
    CHECK(!ValueFromJson("x", BlackboardType::Bool, v, &error));
    CHECK(ValueFromJson(nullptr, BlackboardType::Float, v, &error) && std::holds_alternative<std::monostate>(v));
    CHECK(ValuesEqual(3, 3) && !ValuesEqual(3, 3.0f) && !ValuesEqual(Vec3(1, 0, 0), Vec3(0, 1, 0)));
}

AETHER_TEST(AI_BtFilesAndChecks) {
    BlackboardAsset bb;
    bb.Add("Target", BlackboardType::Entity);
    bb.Add("Goal", BlackboardType::Vector, Vec3(1, 0, 2));
    bb.Add("Health", BlackboardType::Float, 100.0f);
    bb.Add("Mode", BlackboardType::String, std::string("patrol"));
    bb.Add("Distance", BlackboardType::Float);
    BtNode chase = N(BtNodeType::Sequence, {}, "Chase");
    BtNode move = N(BtNodeType::MoveTo);
    move.key = "Target";
    move.acceptance = 1.5f;
    BtNode set = N(BtNodeType::SetBlackboard);
    set.key = "Mode", set.value = std::string("chase");
    BtNode bp = N(BtNodeType::RunBlueprint);
    bp.event = "Taunt";
    chase.children = {set, move, bp};
    chase.decorators = {Cond("Target", BtCompare::IsSet, BtAbort::Both), Deco(BtDecoratorType::Cooldown, 2.0f)};
    BtService dist;
    dist.type = BtServiceType::DistanceTo, dist.key = "Target", dist.out_key = "Distance", dist.interval = 0.25f;
    chase.services = {dist};
    BtNode patrol = N(BtNodeType::Parallel, {Wait(2), With(Log("tick"), Deco(BtDecoratorType::Loop, 1, 3))}, "Patrol");
    patrol.succeed_on_one = true;
    patrol.children[0].deviation = 0.5f;
    BtNode clear = N(BtNodeType::ClearBlackboard);
    clear.key = "Target";
    BtNode luau = N(BtNodeType::RunLuau);
    luau.event = "Think";
    BtNode root = N(BtNodeType::Selector,
                    {chase, patrol, With(N(BtNodeType::Sequence, {clear, luau, N(BtNodeType::Succeed), N(BtNodeType::Fail)}),
                                         Cond("Health", BtCompare::Less, BtAbort::None, 25.0f))});
    root.decorators.push_back(Deco(BtDecoratorType::TimeLimit, 30));
    root.decorators.push_back(Deco(BtDecoratorType::ForceSuccess));
    const BehaviorTreeAsset tree = Tree(root, bb);
    CHECK(ValidateBehaviorTree(tree).empty());

    const nlohmann::json saved = SaveBehaviorTree(tree);
    BehaviorTreeAsset loaded;
    std::string error;
    CHECK(LoadBehaviorTree(saved, loaded, &error));
    if (!error.empty()) std::printf("    %s\n", error.c_str());
    CHECK(SaveBehaviorTree(loaded) == saved); // round trip
    CHECK(loaded.blackboard.keys.size() == 5);
    CHECK(ValuesEqual(loaded.blackboard.Find("Goal")->initial, Vec3(1, 0, 2)));
    CHECK(loaded.root.children[0].decorators[0].abort == BtAbort::Both);
    CHECK(loaded.root.children[0].services[0].out_key == "Distance");
    CHECK(ValuesEqual(loaded.root.children[0].children[0].value, std::string("chase")));
    CHECK(loaded.root.children[1].succeed_on_one);
    CHECK(Near(loaded.root.children[1].children[0].deviation, 0.5f, 1e-6f));

    // Load errors name where.
    nlohmann::json bad = saved;
    bad["root"]["children"][1]["type"] = "Dance";
    CHECK(!LoadBehaviorTree(bad, loaded, &error));
    CHECK(error.find("Dance") != std::string::npos && error.find("root/Selector[1]") != std::string::npos);
    bad = saved;
    bad["root"]["children"][0]["children"][0]["value"] = 5;
    CHECK(!LoadBehaviorTree(bad, loaded, &error));
    bad = saved;
    bad["blackboard"][0]["type"] = "Colour";
    CHECK(!LoadBehaviorTree(bad, loaded, &error));
    CHECK(!LoadBehaviorTree(nlohmann::json::object(), loaded, &error));
    bad = saved;
    bad["version"] = 2;
    CHECK(!LoadBehaviorTree(bad, loaded, &error));

    // The checks.
    BehaviorTreeAsset broken = Tree(N(BtNodeType::Sequence), bb);
    CHECK(HasCode(ValidateBehaviorTree(broken), "BT001"));
    BtNode wait_with_kids = Wait(1);
    wait_with_kids.children.push_back(Log("x"));
    broken.root = N(BtNodeType::Sequence, {wait_with_kids});
    CHECK(HasCode(ValidateBehaviorTree(broken), "BT002"));
    broken.root = N(BtNodeType::Sequence, {With(Log("x"), Cond("Nope"))});
    CHECK(HasCode(ValidateBehaviorTree(broken), "BT003"));
    broken.root = N(BtNodeType::Selector, {With(Log("x"), Cond("Mode", BtCompare::Greater, BtAbort::None, 1.0f))});
    CHECK(HasCode(ValidateBehaviorTree(broken), "BT004"));
    broken.root = N(BtNodeType::Selector, {With(Log("x"), Cond("Health", BtCompare::Equal))});
    CHECK(HasCode(ValidateBehaviorTree(broken), "BT004")); // nothing to compare with
    broken.root = N(BtNodeType::Parallel, {Log("x")});
    auto ps = ValidateBehaviorTree(broken);
    CHECK(HasCode(ps, "BT005") && ps[0].warning);
    broken.root = N(BtNodeType::Sequence, {With(Log("x"), Cond("Target", BtCompare::IsSet, BtAbort::LowerPriority))});
    CHECK(HasCode(ValidateBehaviorTree(broken), "BT006"));
    broken.root = N(BtNodeType::Sequence, {Wait(-1)});
    CHECK(HasCode(ValidateBehaviorTree(broken), "BT007"));
    broken.root = N(BtNodeType::Sequence, {N(BtNodeType::RunBlueprint)});
    CHECK(HasCode(ValidateBehaviorTree(broken), "BT008"));
    BtNode move_bad = N(BtNodeType::MoveTo);
    move_bad.key = "Health";
    broken.root = N(BtNodeType::Sequence, {move_bad});
    CHECK(HasCode(ValidateBehaviorTree(broken), "BT009"));
    broken.root = N(BtNodeType::Sequence, {Log("x")});
    broken.blackboard.Add("Health", BlackboardType::Int);
    ps = ValidateBehaviorTree(broken);
    CHECK(HasCode(ps, "BT010"));
    CHECK(!ps.empty() && ps[0].path == "blackboard");
}

AETHER_TEST(AI_BtCompositesAndWaits) {
    BlackboardAsset bb;
    bb.Add("Target", BlackboardType::Bool);
    Runner r(Tree(N(BtNodeType::Selector, {With(N(BtNodeType::Sequence, {Log("attack"), Wait(1)}), Cond("Target")),
                                           N(BtNodeType::Sequence, {Log("idle"), Wait(0.5f)})}),
                  bb));
    CHECK(r.Tick() == BtStatus::Running);
    CHECK(r.logs == std::vector<std::string>{"idle"});
    CHECK(r.inst->Running());
    CHECK(r.inst->ActiveLabel() == "Wait");
    r.Run(0.5f);
    CHECK(r.inst->RootRuns() == 1); // the idle branch ended (after its half second)
    CHECK(r.Count("idle") == 1);
    r.Tick();
    CHECK(r.Count("idle") == 2); // and began again on the next tick
    // Without an abort, the target is noticed when the idle branch ends.
    r.bb.Set("Target", true);
    r.Run(0.6f);
    CHECK(r.Last() == "attack");
    CHECK(r.inst->ActivePath().size() == 3); // selector, sequence, wait
    const auto& nodes = r.inst->Nodes();
    CHECK(nodes.size() == 7);
    CHECK(nodes[3].path == "Selector/Sequence[0]/Wait[1]");
    CHECK(r.inst->IsActive(3));
    CHECK(r.inst->LastStatus(5) == BtStatus::Success); // idle's Log
    // Sequence fails on a failing child; selector falls through.
    Runner f(Tree(N(BtNodeType::Selector, {N(BtNodeType::Sequence, {Log("a"), N(BtNodeType::Fail), Log("never")}), Log("b")})));
    CHECK(f.Tick() == BtStatus::Success);
    CHECK((f.logs == std::vector<std::string>{"a", "b"}));
    // Wait deviation stays within its bounds, and differs between seeds.
    BtNode w = Wait(1.0f);
    w.deviation = 0.5f;
    Runner dev(Tree(N(BtNodeType::Sequence, {w, Log("done")})));
    f32 t = 0;
    while (dev.logs.empty() && t < 3) dev.Tick(0.05f), t += 0.05f;
    CHECK(t >= 0.45f && t <= 1.6f);
}

AETHER_TEST(AI_BtConditionsAbort) {
    BlackboardAsset bb;
    bb.Add("Enemy", BlackboardType::Bool);
    const auto chase_cond = Cond("Enemy", BtCompare::IsSet, BtAbort::Both);
    Runner r(Tree(N(BtNodeType::Selector, {With(N(BtNodeType::Sequence, {Log("chase"), Wait(10)}), chase_cond),
                                           N(BtNodeType::Sequence, {Log("patrol"), Wait(10)})}),
                  bb));
    r.Run(0.3f);
    CHECK(r.logs == std::vector<std::string>{"patrol"});
    // Lower priority: the enemy appears; patrolling is cut short.
    r.bb.Set("Enemy", true);
    r.Tick();
    CHECK((r.logs == std::vector<std::string>{"patrol", "chase"}));
    CHECK(r.inst->LastStatus(4) == BtStatus::Failure); // the patrol sequence, aborted
    // Self: it goes; chasing stops and patrolling resumes.
    r.bb.Clear("Enemy");
    r.Tick();
    CHECK(r.Last() == "patrol");
    // Only Self: a lower-priority branch isn't interrupted.
    Runner s(Tree(N(BtNodeType::Selector, {With(N(BtNodeType::Sequence, {Log("chase"), Wait(10)}), Cond("Enemy", BtCompare::IsSet, BtAbort::Self)),
                                           N(BtNodeType::Sequence, {Log("patrol"), Wait(1)})}),
                  bb));
    s.Tick();
    s.bb.Set("Enemy", true);
    s.Run(0.5f);
    CHECK(s.Last() == "patrol");
    s.Run(1.0f);
    CHECK(s.Last() == "chase");
    // Comparisons.
    BlackboardAsset hp;
    hp.Add("Health", BlackboardType::Float, 100.0f);
    hp.Add("State", BlackboardType::String, std::string("calm"));
    Runner c(Tree(N(BtNodeType::Selector, {With(N(BtNodeType::Sequence, {Log("flee"), Wait(10)}), Cond("Health", BtCompare::Less, BtAbort::LowerPriority, 25.0f)),
                                           With(N(BtNodeType::Sequence, {Log("rage"), Wait(10)}), Cond("State", BtCompare::Equal, BtAbort::LowerPriority, std::string("angry"))),
                                           N(BtNodeType::Sequence, {Log("calm"), Wait(10)})}),
                  hp));
    c.Tick();
    CHECK(c.Last() == "calm");
    c.bb.Set("State", std::string("angry"));
    c.Tick();
    CHECK(c.Last() == "rage");
    c.bb.Set("Health", 30.0f);
    c.Tick();
    CHECK(c.Last() == "rage");
    c.bb.Set("Health", 20.0f);
    c.Tick();
    CHECK(c.Last() == "flee");
}

AETHER_TEST(AI_BtDecorators) {
    // Cooldown: at most once a second.
    Runner cd(Tree(N(BtNodeType::Selector, {With(Log("shoot"), Deco(BtDecoratorType::Cooldown, 1.0f)), Log("reload")})));
    cd.Run(3.0f);
    CHECK(cd.Count("shoot") == 3);
    CHECK(cd.Count("reload") > 20);
    // Loop: three times, then on.
    Runner loop(Tree(N(BtNodeType::Sequence, {With(N(BtNodeType::Sequence, {Log("step"), Wait(0.2f)}), Deco(BtDecoratorType::Loop, 0, 3)), Log("done"), Wait(100)})));
    loop.Run(1.5f);
    CHECK(loop.Count("step") == 3);
    CHECK(loop.Count("done") == 1);
    // Forever.
    Runner ever(Tree(N(BtNodeType::Sequence, {With(Log("again"), Deco(BtDecoratorType::Loop, 0, 0)), Log("never")})));
    ever.Run(1.0f);
    CHECK(ever.Count("again") >= 5 && ever.Count("never") == 0);
    // Time limit: a long wait fails after a second.
    Runner tl(Tree(N(BtNodeType::Selector, {With(N(BtNodeType::Sequence, {Wait(5), Log("waited")}), Deco(BtDecoratorType::TimeLimit, 1.0f)), Log("gave up")})));
    f32 t = 0;
    while (tl.logs.empty() && t < 3) tl.Tick(), t += 0.1f;
    CHECK(tl.logs == std::vector<std::string>{"gave up"});
    CHECK(Near(t, 1.1f, 0.15f));
    // Inverter, ForceSuccess, ForceFailure.
    Runner inv(Tree(N(BtNodeType::Sequence, {With(N(BtNodeType::Fail), Deco(BtDecoratorType::Inverter)), With(N(BtNodeType::Fail), Deco(BtDecoratorType::ForceSuccess)), Log("ok"),
                                             With(N(BtNodeType::Succeed), Deco(BtDecoratorType::ForceFailure)), Log("never")})));
    CHECK(inv.Tick() == BtStatus::Failure);
    CHECK(inv.logs == std::vector<std::string>{"ok"});
}

AETHER_TEST(AI_BtParallel) {
    BtNode one = N(BtNodeType::Parallel, {N(BtNodeType::Sequence, {Wait(1), Log("fast")}), N(BtNodeType::Sequence, {Wait(3), Log("slow")})});
    one.succeed_on_one = true;
    Runner r(Tree(N(BtNodeType::Sequence, {one, Log("after"), Wait(100)})));
    r.Run(4.0f);
    CHECK((r.logs == std::vector<std::string>{"fast", "after"})); // the slow one was cut off
    // All: one failing fails it, the others are aborted.
    Runner all(Tree(N(BtNodeType::Selector, {N(BtNodeType::Parallel, {N(BtNodeType::Sequence, {Wait(1), Log("late")}), N(BtNodeType::Sequence, {Wait(0.5f), N(BtNodeType::Fail)})}),
                                             N(BtNodeType::Sequence, {Log("fallback"), Wait(100)})})));
    all.Run(2.0f);
    CHECK(all.logs == std::vector<std::string>{"fallback"});
    Runner both(Tree(N(BtNodeType::Sequence, {N(BtNodeType::Parallel, {Wait(0.5f), Wait(1.0f)}), Log("both"), Wait(100)})));
    both.Run(0.8f);
    CHECK(both.logs.empty());
    both.Run(0.5f);
    CHECK(both.logs == std::vector<std::string>{"both"});
}

AETHER_TEST(AI_BtBlackboardTasksAndServices) {
    World world;
    BlackboardAsset bb;
    bb.Add("Target", BlackboardType::Entity);
    bb.Add("Distance", BlackboardType::Float);
    bb.Add("Mode", BlackboardType::String);
    const Entity me = world.CreateEntity(Transform{Vec3(0, 0, 0), Quaternion::Identity()});
    const Entity foe = world.CreateEntity(Transform{Vec3(10, 0, 0), Quaternion::Identity()});
    BtService dist;
    dist.type = BtServiceType::DistanceTo, dist.key = "Target", dist.out_key = "Distance", dist.interval = 0.2f;
    BtNode set = N(BtNodeType::SetBlackboard);
    set.key = "Mode", set.value = std::string("hunting");
    BtNode clear = N(BtNodeType::ClearBlackboard);
    clear.key = "Mode";
    BtNode root = N(BtNodeType::Selector, {With(N(BtNodeType::Sequence, {set, Log("near"), Wait(100)}), Cond("Distance", BtCompare::Less, BtAbort::LowerPriority, 5.0f)),
                                           N(BtNodeType::Sequence, {clear, Wait(100)})});
    root.services = {dist};
    BehaviorTreeAsset tree = Tree(root, bb);
    CHECK(ValidateBehaviorTree(tree).empty());
    Blackboard board(tree.blackboard);
    board.Set("Target", foe);
    BehaviorTreeInstance inst(tree);
    BtHooks hooks;
    std::vector<std::string> logs;
    hooks.log = [&](Entity, const std::string& t) { logs.push_back(t); };
    BtContext ctx{&world, me, &board, &hooks};
    inst.Tick(ctx, 0.1f);
    CHECK(Near(board.GetAs<f32>("Distance"), 10.0f, 1e-4f));
    CHECK(!board.IsSet("Mode"));
    // It comes close: the service notices on its next run, and the near branch takes over.
    world.GetComponent<Transform>(foe)->position = Vec3(3, 0, 0);
    inst.Tick(ctx, 0.1f);
    CHECK(Near(board.GetAs<f32>("Distance"), 10.0f, 1e-4f)); // not yet (every 0.2 s)
    inst.Tick(ctx, 0.1f);
    inst.Tick(ctx, 0.1f);
    CHECK(Near(board.GetAs<f32>("Distance"), 3.0f, 1e-4f));
    CHECK(logs == std::vector<std::string>{"near"});
    CHECK(board.GetAs<std::string>("Mode") == "hunting");
    // The target goes: the distance is cleared.
    world.DestroyEntity(foe);
    for (int i = 0; i < 3; ++i) inst.Tick(ctx, 0.1f);
    CHECK(!board.IsSet("Distance"));
}

AETHER_TEST(AI_BtBlueprintAndLuauHooks) {
    BlackboardAsset bb;
    bb.Add("Stop", BlackboardType::Bool);
    BtNode bp = N(BtNodeType::RunBlueprint);
    bp.event = "Dance";
    BtNode luau = N(BtNodeType::RunLuau);
    luau.event = "Think";
    BtService ping;
    ping.type = BtServiceType::Blueprint, ping.event = "Ping", ping.interval = 0.5f;
    BtService lping;
    lping.type = BtServiceType::Luau, lping.event = "Scan", lping.interval = 1.0f;
    BtNode seq = N(BtNodeType::Sequence, {bp, luau, Log("done"), Wait(100)});
    seq.services = {ping, lping};
    seq.decorators = {Cond("Stop", BtCompare::IsNotSet, BtAbort::Self)};
    Runner r(Tree(N(BtNodeType::Selector, {seq, N(BtNodeType::Sequence, {Log("stopped"), Wait(100)})}), bb));
    std::vector<std::string> started, aborted, services;
    std::vector<bool> firsts;
    int thinks = 0;
    r.hooks.run_blueprint = [&](Entity, const std::string& e) { started.push_back(e); return true; };
    r.hooks.abort_blueprint = [&](Entity, const std::string& e) { aborted.push_back(e); };
    r.hooks.run_luau = [&](Entity, const std::string& f, f32, bool first) {
        CHECK(f == "Think");
        firsts.push_back(first);
        return ++thinks < 3 ? BtStatus::Running : BtStatus::Success;
    };
    r.hooks.blueprint_service = [&](Entity, const std::string& e) { services.push_back(e); };
    r.hooks.luau_service = [&](Entity, const std::string& e, f32) { services.push_back(e); };
    r.Run(1.0f);
    CHECK(started == std::vector<std::string>{"Dance"});
    CHECK(r.logs.empty()); // waiting on the Blueprint
    CHECK(std::count(services.begin(), services.end(), "Ping") == 2);
    CHECK(std::count(services.begin(), services.end(), "Scan") == 1);
    CHECK(!r.inst->FinishLatent(BtStatus::Success, "Other"));
    CHECK(r.inst->FinishLatent(BtStatus::Success, "Dance"));
    CHECK(!r.inst->FinishLatent(BtStatus::Success)); // already finished
    r.Run(0.5f);
    CHECK((firsts == std::vector<bool>{true, false, false}));
    CHECK(r.logs == std::vector<std::string>{"done"});
    // A failing Blueprint task, and one aborted by its condition.
    r.bb.Set("Stop", true);
    r.Tick();
    CHECK(r.Last() == "stopped");
    CHECK(aborted.empty()); // the sequence was waiting, not the Blueprint task
    r.bb.Clear("Stop");
    r.Run(0.1f); // back to the sequence? No: the stopped branch waits 100 s (no lower-priority abort)
    CHECK(r.Last() == "stopped");
    Runner a(Tree(N(BtNodeType::Selector, {With(bp, Cond("Stop", BtCompare::IsNotSet, BtAbort::Self)), Log("fallback")}), bb));
    a.hooks.run_blueprint = [&](Entity, const std::string&) { return true; };
    a.hooks.abort_blueprint = [&](Entity, const std::string& e) { aborted.push_back(e); };
    a.Tick();
    a.bb.Set("Stop", true);
    a.Tick();
    CHECK(aborted == std::vector<std::string>{"Dance"});
    CHECK(a.Last() == "fallback");
    // No hook: the task fails.
    Runner none(Tree(N(BtNodeType::Selector, {bp, luau, Log("neither")})));
    CHECK(none.Tick() == BtStatus::Success);
    CHECK(none.logs == std::vector<std::string>{"neither"});
}

namespace {

struct Library {
    std::map<std::string, BehaviorTreeAsset> trees;
    BehaviorTreeWorld::AssetLookup Lookup() {
        return [this](const std::string& name) -> const BehaviorTreeAsset* {
            auto it = trees.find(name);
            return it == trees.end() ? nullptr : &it->second;
        };
    }
};

bool FloorMesh(const ModelRenderer& m, std::vector<Vec3>& v, std::vector<u32>& i) {
    if (std::strcmp(m.asset_path, "floor") != 0) return false;
    nav::NavGeometry g;
    g.AddPlane(Vec3(0, 0, 0), 10, 10);
    v = g.vertices;
    i = g.indices;
    return true;
}

} // namespace

AETHER_TEST(AI_BtComponentAndWorld) {
    Library lib;
    BlackboardAsset bb;
    bb.Add("Goal", BlackboardType::Vector);
    bb.Add("Count", BlackboardType::Int, 0);
    lib.trees["Guard"] = Tree(N(BtNodeType::Sequence, {Log("guard"), Wait(1)}), bb);
    lib.trees["Other"] = Tree(N(BtNodeType::Sequence, {Log("other"), Wait(1)}));
    World world;
    BehaviorTreeWorld bt(world, lib.Lookup());
    CHECK(BehaviorTreeWorld::Active() == &bt);
    std::vector<std::pair<Entity, std::string>> logs;
    bt.hooks.log = [&](Entity e, const std::string& t) { logs.push_back({e, t}); };

    BehaviorTreeComponent c;
    c.tree = "Guard";
    const Entity e = world.CreateEntity(c);
    BehaviorTreeComponent& comp = *world.GetComponent<BehaviorTreeComponent>(e);
    CHECK(comp.SetVector("Goal", Vec3(1, 2, 3))); // before the tree is known: kept
    CHECK(comp.SetString("Junk", "x"));
    CHECK(!comp.IsRunning());
    bt.Update(0.1f);
    BehaviorTreeComponent& live = *world.GetComponent<BehaviorTreeComponent>(e);
    CHECK(live.IsRunning());
    CHECK(live.GetActiveNode() == "Wait");
    CHECK(logs.size() == 1 && logs[0].first == e);
    CHECK(live.GetVector("Goal").z == 3.0f);
    CHECK(!live.IsValueSet("Junk"));
    CHECK(live.GetInt("Count") == 0 && live.IsValueSet("Count"));
    CHECK(!live.SetFloat("Count", 1.0f)); // the wrong type
    CHECK(live.SetInt("Count", 4) && live.GetInt("Count") == 4);
    CHECK(live.ClearValue("Count") && !live.IsValueSet("Count"));
    CHECK(live.GetFloat("Nope") == 0.0f && live.GetString("Nope").empty() && live.GetEntity("Nope").IsNull() && !live.GetBool("Nope"));
    CHECK(bt.InstanceOf(e) != nullptr);
    // Stop, Start, Restart.
    live.Stop();
    bt.Update(0.1f);
    CHECK(!world.GetComponent<BehaviorTreeComponent>(e)->IsRunning());
    CHECK(bt.InstanceOf(e) == nullptr);
    world.GetComponent<BehaviorTreeComponent>(e)->Start();
    bt.Update(0.1f);
    CHECK(logs.size() == 2);
    world.GetComponent<BehaviorTreeComponent>(e)->Restart();
    bt.Update(0.1f);
    CHECK(logs.size() == 3);
    // A new tree takes over.
    world.GetComponent<BehaviorTreeComponent>(e)->tree = "Other";
    world.GetComponent<BehaviorTreeComponent>(e)->Start();
    bt.Update(0.1f);
    CHECK(logs.back().second == "other");
    // Ticking at an interval.
    BehaviorTreeComponent slow;
    slow.tree = "Other";
    slow.tick_interval = 0.5f;
    const Entity s = world.CreateEntity(slow);
    usize before = 0;
    for (int i = 0; i < 20; ++i) {
        bt.Update(0.1f);
        if (i == 0) before = bt.InstanceOf(s) != nullptr ? 1 : 0;
    }
    CHECK(before == 1);
    CHECK(Near(static_cast<f32>(bt.InstanceOf(s)->Time()), 2.0f, 0.11f)); // time still adds up
    // Not auto-starting, a missing tree, and removal.
    BehaviorTreeComponent manual;
    manual.tree = "Guard";
    manual.auto_start = false;
    const Entity m = world.CreateEntity(manual);
    BehaviorTreeComponent missing;
    missing.tree = "Nope";
    world.CreateEntity(missing);
    bt.Update(0.1f);
    CHECK(!world.GetComponent<BehaviorTreeComponent>(m)->IsRunning());
    CHECK(bt.Problems().size() == 1);
    bt.Update(0.1f);
    CHECK(bt.Problems().size() == 1); // said once
    world.DestroyEntity(e);
    bt.Update(0.1f);
    CHECK(bt.InstanceOf(e) == nullptr);
}

AETHER_TEST(AI_BtMoveToWithAgents) {
    World world;
    ModelRenderer floor;
    SetModelPath(floor, "floor");
    world.CreateEntity(Transform{}, floor);
    nav::NavWorld nav(world, FloorMesh);
    CHECK(nav.Bake({}));
    nav::NavCrowd crowd(world, nav);
    Library lib;
    BlackboardAsset bb;
    bb.Add("Goal", BlackboardType::Vector);
    bb.Add("Halt", BlackboardType::Bool);
    BtNode move = N(BtNodeType::MoveTo);
    move.key = "Goal";
    move.acceptance = 0.5f;
    lib.trees["Walker"] = Tree(N(BtNodeType::Selector, {With(N(BtNodeType::Sequence, {move, Log("there"), Wait(100)}), Cond("Halt", BtCompare::IsNotSet, BtAbort::Self)),
                                                         N(BtNodeType::Sequence, {Log("halted"), Wait(100)})}),
                               bb);
    CHECK(ValidateBehaviorTree(lib.trees["Walker"]).empty());
    BehaviorTreeWorld bt(world, lib.Lookup());
    std::vector<std::string> logs;
    bt.hooks.log = [&](Entity, const std::string& t) { logs.push_back(t); };
    BehaviorTreeComponent c;
    c.tree = "Walker";
    const Entity e = world.CreateEntity(Transform{Vec3(-6, 0, 0), Quaternion::Identity()}, NavAgent{}, c);
    world.GetComponent<BehaviorTreeComponent>(e)->SetVector("Goal", Vec3(6, 0, 0));
    const auto step = [&](int frames) {
        for (int i = 0; i < frames; ++i) {
            bt.Update(1.0f / 30.0f);
            crowd.Update(1.0f / 30.0f);
        }
    };
    step(30);
    CHECK(world.GetComponent<NavAgent>(e)->IsMoving());
    CHECK(Near(world.GetComponent<NavAgent>(e)->stopping_distance, 0.5f, 1e-6f));
    // The goal moves: the move is re-issued.
    world.GetComponent<BehaviorTreeComponent>(e)->SetVector("Goal", Vec3(-2, 0, 6));
    step(2);
    CHECK(Near(world.GetComponent<NavAgent>(e)->GetGoal().z, 6.0f, 0.1f));
    step(300);
    CHECK(logs == std::vector<std::string>{"there"});
    const Vec3 at = world.GetComponent<Transform>(e)->position;
    CHECK(std::hypot(at.x + 2.0f, at.z - 6.0f) < 0.6f);
    // Halting mid-move stops the agent.
    world.GetComponent<BehaviorTreeComponent>(e)->Restart();
    world.GetComponent<BehaviorTreeComponent>(e)->SetVector("Goal", Vec3(6, 0, -6));
    step(15);
    CHECK(world.GetComponent<NavAgent>(e)->IsMoving());
    world.GetComponent<BehaviorTreeComponent>(e)->SetBool("Halt", true);
    step(2);
    CHECK(logs.back() == "halted");
    CHECK(world.GetComponent<NavAgent>(e)->status == NavMoveStatus::Idle);
    // Without an agent, Move To fails.
    const Entity lonely = world.CreateEntity(Transform{}, c);
    world.GetComponent<BehaviorTreeComponent>(lonely)->SetVector("Goal", Vec3(1, 0, 1));
    step(1);
    CHECK(logs.back() == "halted");
}

AETHER_TEST(AI_BtBlueprintNodes) {
    RegisterBehaviorTreeComponents();
    // A Blueprint task: Event Patrol prints, then tells the tree it's done.
    bp::Blueprint blueprint;
    bp::Graph events;
    events.name = "EventGraph";
    blueprint.graphs.push_back(events);
    bp::GraphBuilder b(*blueprint.FindGraph("EventGraph"));
    const bp::NodeId begin = b.Add("Event.BeginPlay");
    const bp::NodeId set = b.Add("Call.Native:BehaviorTreeComponent.SetInt");
    b.Default(set, "key", "Laps").Default(set, "value", 2);
    b.Connect(begin, "then", set, "exec");
    const bp::NodeId patrol = b.Add("Event.Custom", {{"name", "Patrol"}});
    const bp::NodeId print = b.Add("Debug.Print");
    b.Default(print, "text", "patrolling");
    const bp::NodeId finish = b.Add("Call.Native:BehaviorTrees.FinishTask");
    const bp::NodeId self = b.Add("Entity.Self");
    b.Default(finish, "success", true);
    b.Connect(patrol, "then", print, "exec").Connect(print, "then", finish, "exec").Connect(self, "self", finish, "entity");
    bp::CompileResult compiled = bp::CompileBlueprint(blueprint);
    for (const bp::Diagnostic& d : compiled.diagnostics.diagnostics) std::printf("    %s: %s\n", d.code.c_str(), d.message.c_str());
    CHECK(compiled.Ok());

    Library lib;
    BlackboardAsset bb;
    bb.Add("Laps", BlackboardType::Int);
    BtNode task = N(BtNodeType::RunBlueprint);
    task.event = "Patrol";
    lib.trees["Patroller"] = Tree(N(BtNodeType::Sequence, {With(N(BtNodeType::Sequence, {task, Log("lap")}), Cond("Laps", BtCompare::Greater, BtAbort::None, 0)), Wait(100)}), bb);
    World world;
    BehaviorTreeWorld bt(world, lib.Lookup());
    bp::BlueprintVM vm(world);
    std::vector<std::string> printed, logs;
    vm.SetPrintHandler([&](Entity, const std::string& text) { printed.push_back(text); });
    bt.hooks.log = [&](Entity, const std::string& t) { logs.push_back(t); };
    bt.hooks.run_blueprint = [&](Entity e, const std::string& event) { return vm.Dispatch(e, "Event.Custom:" + event); };
    BehaviorTreeComponent c;
    c.tree = "Patroller";
    const Entity e = world.CreateEntity(c);
    CHECK(vm.Attach(e, compiled.blueprint));
    vm.BeginPlay();
    CHECK(world.GetComponent<BehaviorTreeComponent>(e)->GetInt("Laps") == 2);
    bt.Update(0.1f); // dispatches Patrol, which finishes the task while it runs
    CHECK(printed == std::vector<std::string>{"patrolling"});
    CHECK(logs == std::vector<std::string>{"lap"});
    bt.Update(0.1f);
    CHECK(logs == std::vector<std::string>{"lap"});
    CHECK(!BehaviorTrees::FinishTask(e, true)); // nothing waiting now
}
