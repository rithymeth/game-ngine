#include "aether/animation/anim_graph.h"
#include "test_framework.h"

#include <algorithm>
#include <cmath>
#include <map>

using namespace aether;
using namespace aether::anim;

// Phase 16 step 3: animation graphs and state machines.

namespace {

bool Close(f32 a, f32 b, f32 eps = 1e-3f) { return std::fabs(a - b) < eps; }

// Two bones; every clip holds the root at x = `marker` (so a blended pose's
// x shows the weights) and the child at y = `marker`; "Ramp" moves x = t.
Skeleton TwoBones() {
    Skeleton s;
    s.bones.push_back({"root", -1, {}, Mat4::Identity()});
    s.bones.push_back({"arm", 0, {}, Mat4::Identity()});
    return s;
}
AnimationClip Hold(f32 marker, f32 duration = 1.0f) {
    AnimationClip c;
    c.duration = duration;
    c.tracks.resize(2);
    c.tracks[0].translation.times = {0.0f};
    c.tracks[0].translation.values = {Vec3(marker, 0, 0)};
    c.tracks[1].translation.times = {0.0f};
    c.tracks[1].translation.values = {Vec3(0, marker, 0)};
    return c;
}
AnimationClip Ramp() {
    AnimationClip c;
    c.duration = 10.0f;
    c.tracks.resize(2);
    c.tracks[0].translation.times = {0.0f, 10.0f};
    c.tracks[0].translation.values = {Vec3(0, 0, 0), Vec3(10, 0, 0)};
    return c;
}

struct Library {
    std::map<std::string, AnimationClip> clips;
    std::map<std::string, BlendSpace> spaces;
    Library() {
        clips["Idle"] = Hold(0);
        clips["Walk"] = Hold(1);
        clips["Run"] = Hold(2);
        clips["Jump"] = Hold(10, 1.0f);
        clips["Fall"] = Hold(20);
        clips["Rise"] = Hold(30);
        clips["Dead"] = Hold(-5);
        clips["Wave"] = Hold(7);
        clips["Ramp"] = Ramp();
        BlendSpace speed;
        speed.x = {"Speed", 0, 2};
        speed.samples = {{"Idle", 0}, {"Walk", 1}, {"Run", 2}};
        spaces["Locomotion"] = speed;
    }
    AnimAssets Assets() {
        return {[this](const std::string& n) -> const AnimationClip* {
                    auto it = clips.find(n);
                    return it == clips.end() ? nullptr : &it->second;
                },
                [this](const std::string& n) -> const BlendSpace* {
                    auto it = spaces.find(n);
                    return it == spaces.end() ? nullptr : &it->second;
                }};
    }
};

AnimNode ClipNode(u32 id, const std::string& clip, bool loop = true) {
    AnimNode n;
    n.id = id;
    n.asset = clip;
    n.loop = loop;
    return n;
}

Condition If(const std::string& var, CompareOp op, f32 value = 0.0f) { return {var, op, value}; }

// Locomotion: Idle <-> Walk -> Run by Speed; any state -> Jump on a trigger; Jump -> Idle when it ends.
AnimGraph Locomotion() {
    AnimGraph g;
    g.variables = {{"Speed", VarType::Float, 0}, {"Jump", VarType::Trigger, 0}, {"Grounded", VarType::Bool, 1}};
    AnimNode sm;
    sm.id = 1;
    sm.kind = AnimNodeKind::StateMachine;
    sm.machine = "Locomotion";
    g.nodes = {sm, ClipNode(2, "Idle"), ClipNode(3, "Walk"), ClipNode(4, "Run"), ClipNode(5, "Jump", false)};
    g.output = 1;
    StateMachine m;
    m.name = "Locomotion";
    m.states = {{"Idle", 2}, {"Walk", 3}, {"Run", 4}, {"Jump", 5}};
    m.transitions = {
        {0, 1, {If("Speed", CompareOp::Greater, 0.1f)}, false, 0.2f, 0},
        {1, 0, {If("Speed", CompareOp::LessEqual, 0.1f)}, false, 0.2f, 0},
        {1, 2, {If("Speed", CompareOp::Greater, 3.0f)}, false, 0.2f, 0},
        {2, 1, {If("Speed", CompareOp::LessEqual, 3.0f)}, false, 0.2f, 0},
        {kAnyState, 3, {If("Jump", CompareOp::Triggered)}, false, 0.1f, 5},
        {3, 0, {If("Grounded", CompareOp::IsTrue)}, true, 0.25f, 0},
    };
    g.machines = {m};
    return g;
}

usize Count(const std::vector<AnimDiagnostic>& d, const char* code) {
    return static_cast<usize>(std::count_if(d.begin(), d.end(), [&](const AnimDiagnostic& x) { return x.code == code; }));
}

} // namespace

AETHER_TEST(AnimGraph_StateMachineTransitionsAndBlends) {
    Library lib;
    const Skeleton s = TwoBones();
    const AnimGraph g = Locomotion();
    AETHER_CHECK(ValidateAnimGraph(g, &(const AnimAssets&)lib.Assets()).empty());
    AnimGraphInstance anim(g, s, lib.Assets());
    Pose pose;
    anim.Update(0.05f, pose);
    AETHER_CHECK(anim.CurrentState(1) == "Idle" && Close(pose.local[0].translation.x, 0) && anim.Changes().empty());

    // Start walking: a 0.2 s blend, a quarter per 0.05 s frame.
    AETHER_CHECK(anim.SetFloat("Speed", 1.0f) && !anim.SetBool("Speed", true) && !anim.SetFloat("Nope", 1));
    anim.Update(0.05f, pose);
    AETHER_CHECK(anim.CurrentState(1) == "Walk" && anim.InTransition(1) && anim.Changes().size() == 1);
    AETHER_CHECK(anim.Changes()[0].from == "Idle" && anim.Changes()[0].to == "Walk" && anim.Changes()[0].machine == "Locomotion");
    AETHER_CHECK(Close(pose.local[0].translation.x, 0.25f));
    anim.Update(0.05f, pose);
    AETHER_CHECK(Close(pose.local[0].translation.x, 0.5f) && anim.Changes().empty());
    anim.Update(0.05f, pose);
    anim.Update(0.05f, pose);
    AETHER_CHECK(Close(pose.local[0].translation.x, 1.0f) && !anim.InTransition(1));

    // Jump from any state (it outranks Walk -> Run), and the trigger is used up.
    anim.SetFloat("Speed", 5.0f);
    AETHER_CHECK(anim.SetTrigger("Jump"));
    anim.Update(0.05f, pose);
    AETHER_CHECK(anim.CurrentState(1) == "Jump" && anim.Get("Jump") == 0.0f);
    // Jump -> Idle only when Jump is within 0.25 s of its end (it's 1 s long).
    for (int i = 0; i < 12; ++i) anim.Update(0.05f, pose); // 0.65 s in
    AETHER_CHECK(anim.CurrentState(1) == "Jump");
    for (int i = 0; i < 3; ++i) anim.Update(0.05f, pose); // 0.8 s in
    AETHER_CHECK(anim.CurrentState(1) == "Idle");
    // Idle -> Walk at Speed 5, then straight on to Run next frame.
    anim.Update(0.05f, pose);
    AETHER_CHECK(anim.CurrentState(1) == "Walk");
    anim.Update(0.05f, pose);
    AETHER_CHECK(anim.CurrentState(1) == "Run");
    // A trigger nothing uses expires after one Update.
    anim.SetFloat("Speed", 5.0f);
    AETHER_CHECK(anim.Get("Jump") == 0.0f);
}

AETHER_TEST(AnimGraph_InterruptedBlendsDontPop) {
    Library lib;
    const Skeleton s = TwoBones();
    AnimGraph g = Locomotion();
    g.machines[0].transitions[0].blend_time = 1.0f; // a long Idle -> Walk blend
    g.machines[0].transitions[4].blend_time = 0.5f; // Jump
    AnimGraphInstance anim(g, s, lib.Assets());
    Pose pose;
    anim.Update(0.1f, pose);
    anim.SetFloat("Speed", 1.0f);
    for (int i = 0; i < 5; ++i) anim.Update(0.1f, pose);
    const f32 before = pose.local[0].translation.x;
    AETHER_CHECK(Close(before, 0.5f));
    // Jump mid-blend: it blends on from where the pose was, not from Walk or Idle.
    anim.SetTrigger("Jump");
    anim.Update(0.1f, pose);
    AETHER_CHECK(anim.CurrentState(1) == "Jump" && Close(pose.local[0].translation.x, before + (10 - before) * 0.2f));
    anim.Update(0.1f, pose);
    AETHER_CHECK(Close(pose.local[0].translation.x, before + (10 - before) * 0.4f));
}

AETHER_TEST(AnimGraph_ConduitsAndSubMachines) {
    Library lib;
    const Skeleton s = TwoBones();
    AnimGraph g;
    g.variables = {{"InAir", VarType::Bool, 0}, {"VelY", VarType::Float, 0}, {"Die", VarType::Trigger, 0}, {"Revive", VarType::Trigger, 0},
                   {"Speed", VarType::Float, 0}};
    AnimNode outer, inner;
    outer.id = 1;
    outer.kind = AnimNodeKind::StateMachine;
    outer.machine = "Life";
    inner.id = 2;
    inner.kind = AnimNodeKind::StateMachine;
    inner.machine = "Movement";
    g.nodes = {outer, inner, ClipNode(3, "Idle"), ClipNode(4, "Fall"), ClipNode(5, "Rise"), ClipNode(6, "Dead"), ClipNode(7, "Walk")};
    g.output = 1;
    StateMachine life;
    life.name = "Life";
    life.states = {{"Alive", 2}, {"Dead", 6}};
    life.transitions = {{0, 1, {If("Die", CompareOp::Triggered)}, false, 0.0f, 0}, {1, 0, {If("Revive", CompareOp::Triggered)}, false, 0.0f, 0}};
    StateMachine move;
    move.name = "Movement";
    move.states = {{"Ground", 3}, {"AirCheck", 0, true}, {"Fall", 4}, {"Rise", 5}, {"Walk", 7}};
    move.transitions = {
        {0, 1, {If("InAir", CompareOp::IsTrue)}, false, 0.0f, 0},
        {1, 2, {If("VelY", CompareOp::Less, 0)}, false, 0.0f, 0},
        {1, 3, {If("VelY", CompareOp::Greater, 0)}, false, 0.0f, 0},
        {kAnyState, 0, {If("InAir", CompareOp::IsFalse), If("Speed", CompareOp::LessEqual, 0)}, false, 0.0f, 0},
        {0, 4, {If("Speed", CompareOp::Greater, 0)}, false, 0.0f, 0},
    };
    g.machines = {life, move};
    AETHER_CHECK(ValidateAnimGraph(g).empty());
    AnimGraphInstance anim(g, s, lib.Assets());
    Pose pose;
    anim.Update(0.1f, pose);
    AETHER_CHECK(anim.CurrentState(1) == "Alive" && anim.CurrentState(2) == "Ground" && Close(pose.local[0].translation.x, 0));
    // In the air but VelY is 0: no conduit exit holds, so it stays on the ground.
    anim.SetBool("InAir", true);
    anim.Update(0.1f, pose);
    AETHER_CHECK(anim.CurrentState(2) == "Ground");
    // Falling: straight through the conduit to Fall, as one change.
    anim.SetFloat("VelY", -1);
    anim.Update(0.1f, pose);
    AETHER_CHECK(anim.CurrentState(2) == "Fall" && Close(pose.local[0].translation.x, 20));
    AETHER_CHECK(anim.Changes().size() == 1 && anim.Changes()[0].from == "Ground" && anim.Changes()[0].to == "Fall");
    // Land and walk; then die (the outer machine) and revive: the inner machine starts over at Ground.
    anim.SetBool("InAir", false);
    anim.Update(0.1f, pose);
    AETHER_CHECK(anim.CurrentState(2) == "Ground");
    anim.SetFloat("Speed", 1);
    anim.Update(0.1f, pose);
    AETHER_CHECK(anim.CurrentState(2) == "Walk");
    anim.SetTrigger("Die");
    anim.Update(0.1f, pose);
    AETHER_CHECK(anim.CurrentState(1) == "Dead" && Close(pose.local[0].translation.x, -5));
    // Reviving re-enters Movement at its entry (Ground), and its transitions
    // run in that same frame: at walking speed it moves on from Ground to Walk.
    anim.SetTrigger("Revive");
    anim.Update(0.1f, pose);
    AETHER_CHECK(anim.CurrentState(1) == "Alive" && anim.CurrentState(2) == "Walk");
    const auto& changes = anim.Changes();
    AETHER_CHECK(changes.size() == 2 && changes[0].to == "Alive" && changes[1].machine == "Movement" && changes[1].from == "Ground");
    AETHER_CHECK(anim.CurrentState(3).empty()); // not a machine node
}

AETHER_TEST(AnimGraph_BlendNodes) {
    Library lib;
    const Skeleton s = TwoBones();
    AnimGraph g;
    g.variables = {{"Alpha", VarType::Float, 0.25f}, {"Armed", VarType::Bool, 0}, {"Stance", VarType::Int, 0}, {"Speed", VarType::Float, 1.5f},
                   {"Lean", VarType::Float, 0.5f}};
    auto node = [](u32 id, AnimNodeKind kind, std::vector<u32> inputs, const std::string& var = {}) {
        AnimNode n;
        n.id = id;
        n.kind = kind;
        n.inputs = std::move(inputs);
        n.variable = var;
        return n;
    };
    AnimNode space = node(20, AnimNodeKind::BlendSpace, {}, "Speed");
    space.asset = "Locomotion";
    AnimNode layered = node(13, AnimNodeKind::Layered, {1, 2});
    layered.bone = "arm";
    AnimNode by_bool = node(11, AnimNodeKind::BlendByBool, {1, 2}, "Armed");
    by_bool.blend_time = 0.2f;
    AnimNode by_int = node(12, AnimNodeKind::BlendByInt, {1, 2, 3}, "Stance");
    by_int.blend_time = 0.0f;
    g.nodes = {ClipNode(1, "Walk"), ClipNode(2, "Wave"), ClipNode(3, "Run"), node(10, AnimNodeKind::Blend, {1, 2}, "Alpha"), by_bool, by_int,
               layered, node(14, AnimNodeKind::Additive, {1, 2}, "Lean"), space};
    AETHER_CHECK(ValidateAnimGraph([&] { AnimGraph v = g; v.output = 10; return v; }(), &(const AnimAssets&)lib.Assets()).empty());
    auto run = [&](u32 output, const std::function<void(AnimGraphInstance&)>& setup, int frames = 1) {
        AnimGraph copy = g;
        copy.output = output;
        AnimGraphInstance anim(copy, s, lib.Assets());
        setup(anim);
        Pose pose;
        for (int i = 0; i < frames; ++i) anim.Update(0.05f, pose);
        return pose;
    };
    // Blend by alpha: Walk (1) toward Wave (7).
    AETHER_CHECK(Close(run(10, [](AnimGraphInstance&) {}).local[0].translation.x, 1 + 6 * 0.25f));
    AETHER_CHECK(Close(run(10, [](AnimGraphInstance& a) { a.SetFloat("Alpha", 3); }).local[0].translation.x, 7)); // clamped
    // Blend by bool: settled at start, then 0.2 s crossfades.
    AETHER_CHECK(Close(run(11, [](AnimGraphInstance&) {}).local[0].translation.x, 1));
    {
        AnimGraph copy = g;
        copy.output = 11;
        AnimGraphInstance anim(copy, s, lib.Assets());
        Pose pose;
        anim.Update(0.05f, pose);
        anim.SetBool("Armed", true);
        anim.Update(0.05f, pose);
        AETHER_CHECK(Close(pose.local[0].translation.x, 1 + 6 * 0.25f));
        anim.Update(0.1f, pose);
        AETHER_CHECK(Close(pose.local[0].translation.x, 1 + 6 * 0.75f));
        anim.Update(0.1f, pose);
        AETHER_CHECK(Close(pose.local[0].translation.x, 7));
    }
    // Blend by int: an instant switch (blend_time 0), clamped to the inputs.
    AETHER_CHECK(Close(run(12, [](AnimGraphInstance& a) { a.SetInt("Stance", 2); }).local[0].translation.x, 2));
    AETHER_CHECK(Close(run(12, [](AnimGraphInstance& a) { a.SetInt("Stance", 9); }).local[0].translation.x, 2));
    // Layered: the arm from Wave, the root from Walk.
    const Pose layer = run(13, [](AnimGraphInstance&) {});
    AETHER_CHECK(Close(layer.local[0].translation.x, 1) && Close(layer.local[1].translation.y, 7));
    // Additive: Walk plus half of Wave's difference from the rest pose.
    AETHER_CHECK(Close(run(14, [](AnimGraphInstance&) {}).local[0].translation.x, 1 + 0.5f * 7));
    // A blend space node driven by a variable: Speed 1.5 is half way from Walk to Run.
    AETHER_CHECK(Close(run(20, [](AnimGraphInstance&) {}).local[0].translation.x, 1.5f));

    // A node shared by two parents advances once per frame.
    AnimGraph shared;
    shared.variables = {{"Alpha", VarType::Float, 0.5f}};
    shared.nodes = {ClipNode(1, "Ramp"), node(2, AnimNodeKind::Blend, {1, 1}, "Alpha")};
    shared.output = 2;
    AnimGraphInstance anim(shared, s, lib.Assets());
    Pose pose;
    for (int i = 0; i < 10; ++i) anim.Update(0.1f, pose);
    AETHER_CHECK(Close(pose.local[0].translation.x, 1.0f));
}

AETHER_TEST(AnimGraph_ValidationAndFiles) {
    Library lib;
    const AnimAssets assets = lib.Assets();
    const AnimGraph good = Locomotion();
    AETHER_CHECK(ValidateAnimGraph(good, &assets).empty());
    auto check = [&](const char* code, const std::function<void(AnimGraph&)>& spoil) {
        AnimGraph g = good;
        spoil(g);
        return Count(ValidateAnimGraph(g, &assets), code) > 0;
    };
    AETHER_CHECK(check("AG001", [](AnimGraph& g) { g.output = 99; }));
    AETHER_CHECK(check("AG002", [](AnimGraph& g) {
        AnimNode b;
        b.id = 9;
        b.kind = AnimNodeKind::Blend;
        b.variable = "Speed";
        b.inputs = {2, 77};
        g.nodes.push_back(b);
    }));
    AETHER_CHECK(check("AG002", [](AnimGraph& g) { g.nodes[1].inputs = {3}; })); // a clip takes no inputs
    AETHER_CHECK(check("AG003", [](AnimGraph& g) {
        AnimNode a, b;
        a.id = 20, a.kind = AnimNodeKind::Blend, a.variable = "Speed", a.inputs = {21, 2};
        b.id = 21, b.kind = AnimNodeKind::Blend, b.variable = "Speed", b.inputs = {20, 2};
        g.nodes.push_back(a);
        g.nodes.push_back(b);
    }));
    AETHER_CHECK(check("AG004", [](AnimGraph& g) { g.machines[0].transitions[0].conditions[0].variable = "Velocity"; }));
    AETHER_CHECK(check("AG005", [](AnimGraph& g) { g.machines[0].transitions[0].conditions[0].op = CompareOp::Triggered; }));
    AETHER_CHECK(check("AG005", [](AnimGraph& g) { g.machines[0].transitions[4].conditions[0].op = CompareOp::Greater; }));
    AETHER_CHECK(check("AG005", [](AnimGraph& g) {
        AnimNode b;
        b.id = 9, b.kind = AnimNodeKind::BlendByBool, b.variable = "Speed", b.inputs = {2, 3};
        g.nodes.push_back(b);
    }));
    AETHER_CHECK(check("AG006", [](AnimGraph& g) { g.nodes[0].machine = "Nope"; }));
    AETHER_CHECK(check("AG006", [](AnimGraph& g) { g.machines[0].entry = 9; }));
    AETHER_CHECK(check("AG006", [](AnimGraph& g) { g.machines[0].transitions[0].to = 9; }));
    AETHER_CHECK(check("AG006", [](AnimGraph& g) { g.machines[0].states.clear(); }));
    AETHER_CHECK(check("AG007", [](AnimGraph& g) { g.machines[0].states[0].conduit = true; }));
    AETHER_CHECK(check("AG007", [](AnimGraph& g) { g.machines[0].states.push_back({"Stuck", 0, true}); }));
    AETHER_CHECK(check("AG008", [](AnimGraph& g) { g.machines[0].states[1].pose = 1; })); // Walk plays the machine itself
    AETHER_CHECK(check("AG009", [](AnimGraph& g) { g.machines[0].states.push_back({"Orphan", 2}); }));
    AETHER_CHECK(check("AG010", [](AnimGraph& g) { g.nodes[2].asset = "Moonwalk"; }));
    AETHER_CHECK(check("AG011", [](AnimGraph& g) { g.variables.push_back({"Speed", VarType::Int, 0}); }));
    AETHER_CHECK(check("AG011", [](AnimGraph& g) { g.machines[0].states[1].name = "Idle"; }));
    AnimGraph orphan = good;
    orphan.machines[0].states.push_back({"Orphan", 2});
    for (const AnimDiagnostic& d : ValidateAnimGraph(orphan, &assets)) AETHER_CHECK(d.code != "AG009" || !d.error);

    // Files.
    const nlohmann::json j = AnimGraphToJson(good);
    AETHER_CHECK(j["machines"][0]["transitions"][4]["from"] == "*" && j["machines"][0]["entry"] == "Idle");
    AnimGraph back;
    std::string error;
    AETHER_CHECK(AnimGraphFromJson(j, back, &error) && AnimGraphToJson(back) == j);
    AETHER_CHECK(back.machines[0].transitions[5].when_finished && back.machines[0].transitions[4].priority == 5);
    AETHER_CHECK(ValidateAnimGraph(back, &assets).empty());
    for (const char* bad : {R"({"$type": "Material"})", R"({"$type": "AnimGraph", "$version": 5})",
                            R"({"$type": "AnimGraph", "nodes": [{"id": 1, "kind": "Teleport"}]})",
                            R"({"$type": "AnimGraph", "variables": [{"name": "x", "type": "vec3"}]})",
                            R"({"$type": "AnimGraph", "machines": [{"name": "M", "states": [{"name": "A"}], "transitions": [{"from": "A", "to": "B"}]}]})",
                            R"({"$type": "AnimGraph", "machines": [{"name": "M", "states": [{"name": "A"}], "transitions": [{"to": "A", "conditions": [{"variable": "x", "op": "~"}]}]}]})"}) {
        AETHER_CHECK(!AnimGraphFromJson(nlohmann::json::parse(bad), back, &error) && !error.empty());
    }
}
