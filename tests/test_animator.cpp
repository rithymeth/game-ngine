#include "aether/animation/animator.h"
#include "aether/animation/compression.h"
#include "aether/blueprint/compiler.h"
#include "aether/blueprint/vm.h"
#include "aether/scene/components.h"
#include "test_framework.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>

using namespace aether;
using namespace aether::anim;

// Phase 16 step 4: notifies, montages, root motion in graphs, and the
// Animator component with its system.

namespace {

bool Close(f32 a, f32 b, f32 eps = 1e-3f) { return std::fabs(a - b) < eps; }

Skeleton Rig() {
    Skeleton s;
    s.bones.push_back({"root", -1, {}, Mat4::Identity()});
    s.bones.push_back({"arm", 0, {Vec3(0, 1, 0), Quaternion::Identity(), Vec3(1, 1, 1)}, Mat4::Identity()});
    return s;
}
// The root at x = marker; with `speed`, it also walks +Z at that many m/s.
AnimationClip Clip(const std::string& name, f32 marker, f32 duration = 1.0f, f32 speed = 0.0f) {
    AnimationClip c;
    c.name = name;
    c.duration = duration;
    c.tracks.resize(2);
    c.tracks[0].translation.times = {0.0f, duration};
    c.tracks[0].translation.values = {Vec3(marker, 0, 0), Vec3(marker, 0, speed * duration)};
    return c;
}

usize Count(const std::vector<AnimEvent>& events, AnimEventType type, const std::string& name = {}) {
    return static_cast<usize>(std::count_if(events.begin(), events.end(), [&](const AnimEvent& e) {
        return e.type == type && (name.empty() || e.name == name);
    }));
}

struct Assets {
    std::map<std::string, AnimationClip> clips;
    std::map<std::string, BlendSpace> spaces;
    std::map<std::string, Montage> montages;
    std::map<std::string, AnimGraph> graphs;
    Skeleton rig = Rig();
    Assets() {
        clips["Idle"] = Clip("Idle", 0);
        AnimationClip walk = Clip("Walk", 1, 1.0f, 2.0f);
        walk.notifies = {{"FootL", 0.25f}, {"FootR", 0.75f}};
        clips["Walk"] = walk;
        AnimationClip run = Clip("Run", 2, 1.0f, 5.0f);
        run.notifies = {{"FootL", 0.25f}, {"FootR", 0.75f}};
        clips["Run"] = run;
        AnimationClip attack = Clip("Attack", 10, 1.0f, 1.0f);
        attack.notifies = {{"Hit", 0.5f}, {"Trail", 0.2f, 0.3f}};
        clips["Attack"] = attack;
        BlendSpace speed;
        speed.x = {"Speed", 0, 1};
        speed.samples = {{"Walk", 0}, {"Run", 1}};
        spaces["Move"] = speed;
        Montage m;
        m.name = "Combo";
        m.clip = "Attack";
        m.blend_in = 0.1f;
        m.blend_out = 0.2f;
        m.sections = {{"Start", 0.0f, "Loop"}, {"Loop", 0.4f, "Loop"}, {"End", 0.8f, ""}};
        montages["Combo"] = m;
        Montage once = m;
        once.name = "Once";
        once.sections.clear();
        montages["Once"] = once;
        graphs["Loco"] = Loco();
    }
    // Slot "Default" over a machine: Idle <-> Walk by Speed.
    static AnimGraph Loco() {
        AnimGraph g;
        g.variables = {{"Speed", VarType::Float, 0}};
        AnimNode slot, sm, idle, walk;
        slot.id = 1, slot.kind = AnimNodeKind::Slot, slot.slot = "Default", slot.inputs = {2};
        sm.id = 2, sm.kind = AnimNodeKind::StateMachine, sm.machine = "M";
        idle.id = 3, idle.asset = "Idle";
        walk.id = 4, walk.asset = "Walk";
        g.nodes = {slot, sm, idle, walk};
        g.output = 1;
        StateMachine m;
        m.name = "M";
        m.states = {{"Idle", 3}, {"Walk", 4}};
        m.transitions = {{0, 1, {{"Speed", CompareOp::Greater, 0.1f}}, false, 0.2f, 0}, {1, 0, {{"Speed", CompareOp::LessEqual, 0.1f}}, false, 0.2f, 0}};
        g.machines = {m};
        return g;
    }
    AnimAssets Anim() {
        return {[this](const std::string& n) -> const AnimationClip* {
                    auto it = clips.find(n);
                    return it == clips.end() ? nullptr : &it->second;
                },
                [this](const std::string& n) -> const BlendSpace* {
                    auto it = spaces.find(n);
                    return it == spaces.end() ? nullptr : &it->second;
                }};
    }
    AnimationLibrary Library() {
        AnimationLibrary lib;
        lib.graph = [this](const std::string& n) -> const AnimGraph* {
            auto it = graphs.find(n);
            return it == graphs.end() ? nullptr : &it->second;
        };
        lib.skeleton = [this](const std::string& n) -> const Skeleton* { return n == "Rig" ? &rig : nullptr; };
        lib.montage = [this](const std::string& n) -> const Montage* {
            auto it = montages.find(n);
            return it == montages.end() ? nullptr : &it->second;
        };
        lib.assets = Anim();
        return lib;
    }
};

AnimGraph OneNode(const AnimNode& n) {
    AnimGraph g;
    g.variables = {{"Speed", VarType::Float, 0.5f}};
    g.nodes = {n};
    g.output = n.id;
    return g;
}

} // namespace

AETHER_TEST(Animator_NotifyPointsAndCompression) {
    AnimationClip c = Clip("C", 0, 1.0f);
    c.notifies = {{"A", 0.25f}, {"W", 0.5f, 0.25f}, {"Start", 0.0f}};
    std::vector<NotifyPoint> p;
    CollectNotifies(c, 0.0f, 0.3f, true, p);
    AETHER_CHECK(p.size() == 1 && p[0].notify->name == "A"); // after `from`, so the start marker doesn't fire at 0
    p.clear();
    CollectNotifies(c, 0.3f, 0.8f, true, p);
    AETHER_CHECK(p.size() == 2 && p[0].window && !p[0].end && p[1].end && p[1].notify->name == "W");
    p.clear();
    CollectNotifies(c, 0.9f, 0.3f, true, p); // wrapped: the start marker counts this time
    AETHER_CHECK(p.size() == 2 && p[0].notify->name == "A" && p[1].notify->name == "Start");
    p.clear();
    CollectNotifies(c, 0.9f, 0.3f, false, p); // not looping: nothing going backwards
    CollectNotifies(c, 0.4f, 0.4f, true, p);
    AETHER_CHECK(p.empty());

    // Notifies survive compression; version 1 data (no notifies) still reads.
    std::vector<u8> bytes = CompressClip(c);
    AnimationClip back;
    AETHER_CHECK(DecompressClip(bytes, back) && back.notifies.size() == 3 && back.notifies[1].duration == 0.25f && back.notifies[1].name == "W");
    AnimationClip plain = Clip("P", 1);
    std::vector<u8> v1 = CompressClip(plain);
    v1.resize(v1.size() - 4); // no notify count
    const u16 one = 1;
    std::memcpy(v1.data() + 4, &one, 2);
    AETHER_CHECK(DecompressClip(v1, back) && back.notifies.empty() && back.tracks.size() == 2);
}

AETHER_TEST(Animator_GraphNotifiesAreWeighted) {
    Assets a;
    const Skeleton& s = a.rig;
    AnimNode walk;
    walk.id = 1, walk.asset = "Walk";
    const AnimGraph single = OneNode(walk);
    AnimGraphInstance anim(single, s, a.Anim());
    Pose pose;
    usize steps = 0;
    for (int i = 0; i < 10; ++i) {
        anim.Update(0.1f, pose);
        steps += Count(anim.Events(), AnimEventType::Notify);
        for (const AnimEvent& e : anim.Events()) AETHER_CHECK(e.weight == 1.0f && e.detail == "Walk");
    }
    AETHER_CHECK(steps == 2); // FootL and FootR once per 1 s cycle

    // A blend space fires the heaviest clip's notifies only: one step, not two.
    AnimNode space;
    space.id = 1, space.kind = AnimNodeKind::BlendSpace, space.asset = "Move", space.variable = "Speed";
    AnimGraph g = OneNode(space);
    g.variables[0].default_value = 0.3f;
    AnimGraphInstance blended(g, s, a.Anim());
    steps = 0;
    f32 weight = 0.0f;
    for (int i = 0; i < 10; ++i) {
        blended.Update(0.1f, pose);
        for (const AnimEvent& e : blended.Events()) {
            ++steps;
            weight = e.weight;
            AETHER_CHECK(e.detail == "Walk");
        }
    }
    AETHER_CHECK(steps == 2 && Close(weight, 0.7f));

    // During a state crossfade both states' notifies come with their weights.
    AnimGraph loco = Assets::Loco();
    loco.machines[0].transitions[0].blend_time = 1.0f;
    AnimGraphInstance cross(loco, s, a.Anim());
    cross.SetFloat("Speed", 1.0f);
    f32 seen = -1.0f;
    for (int i = 0; i < 4; ++i) {
        cross.Update(0.1f, pose);
        for (const AnimEvent& e : cross.Events()) {
            if (e.type == AnimEventType::Notify) seen = e.weight;
        }
    }
    AETHER_CHECK(Close(seen, 0.3f)); // FootL at 0.25 s into the walk: 0.3 of the way through the blend
    AETHER_CHECK(Count(cross.Events(), AnimEventType::StateChanged) == 0);
}

AETHER_TEST(Animator_MontagesSectionsAndSlots) {
    Assets a;
    const AnimGraph g = Assets::Loco();
    AnimGraphInstance anim(g, a.rig, a.Anim());
    Pose pose;
    anim.Update(0.05f, pose);
    AETHER_CHECK(!anim.IsPlayingMontage() && Close(pose.local[0].translation.x, 0));
    Montage bad = a.montages["Combo"];
    bad.clip = "Nope";
    AETHER_CHECK(!anim.PlayMontage(bad) && !anim.PlayMontage(a.montages["Combo"], 1.0f, "Middle") && !anim.StopMontage());
    AETHER_CHECK(anim.PlayMontage(a.montages["Combo"]));
    AETHER_CHECK(anim.IsPlayingMontage() && anim.CurrentSection() == "Start" && anim.MontageWeight() == 0.0f);
    // Blending in over 0.1 s: half way after 0.05 s; the started event comes with the next Update.
    anim.Update(0.05f, pose);
    AETHER_CHECK(Count(anim.Events(), AnimEventType::MontageStarted, "Combo") == 1 && Close(anim.MontageWeight(), 0.5f));
    AETHER_CHECK(Close(pose.local[0].translation.x, 5.0f));
    anim.Update(0.05f, pose);
    AETHER_CHECK(Close(pose.local[0].translation.x, 10.0f));
    // Start -> Loop at 0.4, and Loop repeats itself (0.4 .. 0.8) until told otherwise:
    // over the next 1 s (clip time 0.1 -> 1.1) it enters Loop at 0.4 and wraps at 0.8,
    // passing Hit (0.5) twice; the Trail window (0.2) is in Start, so it begins once.
    usize loops = 0, trails = 0, hits = 0;
    for (int i = 0; i < 20; ++i) {
        anim.Update(0.05f, pose);
        loops += Count(anim.Events(), AnimEventType::MontageSectionChanged);
        trails += Count(anim.Events(), AnimEventType::NotifyBegin, "Trail");
        hits += Count(anim.Events(), AnimEventType::Notify, "Hit");
    }
    AETHER_CHECK(anim.CurrentSection() == "Loop" && loops == 2 && trails == 1 && hits == 2 && anim.IsPlayingMontage());
    // Jump to End: it blends out over its last 0.2 s and finishes on its own.
    AETHER_CHECK(anim.JumpToSection("End") && !anim.JumpToSection("Nope"));
    bool blending_out = false, ended = false, interrupted = true;
    for (int i = 0; i < 10 && !ended; ++i) {
        anim.Update(0.05f, pose);
        for (const AnimEvent& e : anim.Events()) {
            if (e.type == AnimEventType::MontageBlendingOut) blending_out = true;
            if (e.type == AnimEventType::MontageEnded) ended = true, interrupted = e.interrupted;
        }
    }
    AETHER_CHECK(blending_out && ended && !interrupted && !anim.IsPlayingMontage());
    anim.Update(0.05f, pose);
    AETHER_CHECK(Close(pose.local[0].translation.x, 0)); // back to the graph

    // Stopping ends it interrupted; playing over one ends the old one interrupted, without a pop.
    AETHER_CHECK(anim.PlayMontage(a.montages["Once"]));
    for (int i = 0; i < 4; ++i) anim.Update(0.05f, pose);
    AETHER_CHECK(anim.StopMontage(Montage{}.slot) && !anim.StopMontage());
    ended = false;
    for (int i = 0; i < 6; ++i) {
        anim.Update(0.05f, pose);
        for (const AnimEvent& e : anim.Events()) {
            if (e.type == AnimEventType::MontageEnded) ended = true, interrupted = e.interrupted;
        }
    }
    AETHER_CHECK(ended && interrupted);
    AETHER_CHECK(anim.PlayMontage(a.montages["Once"]));
    for (int i = 0; i < 3; ++i) anim.Update(0.05f, pose);
    const f32 weight = anim.MontageWeight();
    AETHER_CHECK(anim.PlayMontage(a.montages["Combo"]) && anim.MontageWeight() == weight);
    anim.Update(0.0f, pose);
    AETHER_CHECK(Count(anim.Events(), AnimEventType::MontageEnded, "Once") == 1 && Count(anim.Events(), AnimEventType::MontageStarted, "Combo") == 1);

    // Files and checks.
    const nlohmann::json j = MontageToJson(a.montages["Combo"]);
    Montage back;
    std::string error;
    AETHER_CHECK(MontageFromJson(j, back, &error) && MontageToJson(back) == j && back.sections[1].next == "Loop");
    AETHER_CHECK(!MontageFromJson(nlohmann::json{{"$type", "Clip"}}, back, &error));
    AETHER_CHECK(ValidateMontage(a.montages["Combo"]).empty());
    Montage broken = a.montages["Combo"];
    broken.clip.clear();
    broken.sections[2].next = "Nowhere";
    broken.sections.push_back({"Start", 0.9f, ""});
    broken.blend_in = -1;
    const std::vector<std::string> problems = ValidateMontage(broken);
    auto has = [&](const char* code) {
        return std::any_of(problems.begin(), problems.end(), [&](const std::string& p) { return p.rfind(code, 0) == 0; });
    };
    AETHER_CHECK(has("MN001") && has("MN002") && has("MN003") && has("MN004"));
    AnimGraph noslot = Assets::Loco();
    noslot.nodes[0].slot.clear();
    const std::vector<AnimDiagnostic> slot_problems = ValidateAnimGraph(noslot);
    AETHER_CHECK(std::any_of(slot_problems.begin(), slot_problems.end(), [](const AnimDiagnostic& d) { return d.code == "AG002"; }));
}

AETHER_TEST(Animator_RootMotionFromGraphsAndMontages) {
    Assets a;
    AnimNode walk;
    walk.id = 1, walk.asset = "Walk";
    const AnimGraph single = OneNode(walk);
    AnimGraphInstance anim(single, a.rig, a.Anim());
    anim.SetRootMotion(true);
    Pose pose;
    anim.Update(0.1f, pose);
    AETHER_CHECK(Close(anim.RootMotion().translation.z, 0.2f) && Close(pose.local[0].translation.z, 0.0f)); // in place
    Vec3 total(0, 0, 0);
    for (int i = 0; i < 20; ++i) {
        anim.Update(0.1f, pose);
        total = total + anim.RootMotion().translation;
    }
    AETHER_CHECK(Close(total.z, 4.0f, 1e-2f)); // 2 m/s across two loop wraps
    // A montage's motion counts as much as it's blended in, the graph's for the rest.
    AnimGraph g = Assets::Loco();
    AnimGraphInstance loco(g, a.rig, a.Anim());
    loco.SetRootMotion(true);
    loco.SetFloat("Speed", 1.0f);
    for (int i = 0; i < 10; ++i) loco.Update(0.1f, pose); // walking, settled
    AETHER_CHECK(Close(loco.RootMotion().translation.z, 0.2f));
    loco.PlayMontage(a.montages["Once"]); // 1 m/s
    loco.Update(0.05f, pose);             // half blended in
    AETHER_CHECK(Close(loco.MontageWeight(), 0.5f) && Close(loco.RootMotion().translation.z, 0.5f * 0.05f + 0.5f * 0.1f, 2e-3f));
    // Without root motion, the pose moves instead and nothing is reported.
    AnimGraphInstance off(single, a.rig, a.Anim());
    off.Update(0.1f, pose);
    AETHER_CHECK(Close(pose.local[0].translation.z, 0.2f) && off.RootMotion().translation.z == 0.0f);
}

AETHER_TEST(Animator_ComponentSystemAndBlueprints) {
    Assets a;
    World world;
    AnimationSystem system(world, a.Library());
    std::vector<Entity> actors;
    for (int i = 0; i < 12; ++i) {
        Animator anim;
        anim.graph = "Loco";
        anim.skeleton = "Rig";
        anim.speed = 1.0f + 0.1f * static_cast<f32>(i);
        actors.push_back(world.CreateEntity(Transform{Vec3(), Quaternion::Identity()}, anim));
    }
    Animator broken;
    broken.graph = "Missing";
    broken.skeleton = "Rig";
    const Entity lost = world.CreateEntity(broken);
    system.Update(0.05f);
    AETHER_CHECK(system.Count() == 13 && system.PoseOf(actors[0]) != nullptr && system.PoseOf(lost) == nullptr);
    AETHER_CHECK(system.Problems().size() == 1 && system.Problems()[0].find("Missing") != std::string::npos);
    AETHER_CHECK(system.SkinMatricesOf(actors[3])->size() == 2);

    // Commands from gameplay: parameters and montages, applied before evaluation.
    world.GetComponent<Animator>(actors[0])->SetFloatParameter("Speed", 1.0f);
    world.GetComponent<Animator>(actors[1])->PlayMontage("Combo", 1.0f);
    world.GetComponent<Animator>(actors[2])->PlayMontage("Nope", 1.0f);
    system.Update(0.05f);
    AETHER_CHECK(world.GetComponent<Animator>(actors[0])->commands.empty());
    AETHER_CHECK(system.InstanceOf(actors[0])->CurrentState(2) == "Walk" && system.InstanceOf(actors[1])->IsPlayingMontage());
    AETHER_CHECK(system.Problems().size() == 2);
    bool changed = false, started = false;
    for (const EntityAnimEvent& e : system.Events()) {
        if (e.entity == actors[0] && e.event.type == AnimEventType::StateChanged && e.event.name == "Walk") changed = true;
        if (e.entity == actors[1] && e.event.type == AnimEventType::MontageStarted) started = true;
    }
    AETHER_CHECK(changed && started);

    // Parallel evaluation gives the same poses as serial.
    World twin;
    AnimationSystem serial(twin, a.Library());
    std::vector<Entity> twins;
    for (int i = 0; i < 12; ++i) twins.push_back(twin.CreateEntity(Transform{Vec3(), Quaternion::Identity()}, *world.GetComponent<Animator>(actors[static_cast<usize>(i)])));
    JobSystem jobs(3);
    AnimationSystem parallel(world, a.Library());
    for (int i = 0; i < 12; ++i) {
        world.GetComponent<Animator>(actors[static_cast<usize>(i)])->SetFloatParameter("Speed", i % 2 ? 1.0f : 0.0f);
        twin.GetComponent<Animator>(twins[static_cast<usize>(i)])->SetFloatParameter("Speed", i % 2 ? 1.0f : 0.0f);
    }
    for (int f = 0; f < 5; ++f) {
        parallel.Update(0.05f, &jobs);
        serial.Update(0.05f);
    }
    for (int i = 0; i < 12; ++i) {
        const Pose* p = parallel.PoseOf(actors[static_cast<usize>(i)]);
        const Pose* q = serial.PoseOf(twins[static_cast<usize>(i)]);
        AETHER_CHECK(p != nullptr && q != nullptr && p->local[0].translation.x == q->local[0].translation.x);
    }
    // Gone entities are dropped.
    world.DestroyEntity(actors[5]);
    parallel.Update(0.05f, &jobs);
    AETHER_CHECK(parallel.Count() == 12 && parallel.PoseOf(actors[5]) == nullptr);

    // Blueprints: Play Montage as a node, and Event OnMontageEnded dispatched from the system's events.
    RegisterAnimationComponents();
    bp::Blueprint blueprint;
    bp::Graph events;
    events.name = "EventGraph";
    blueprint.graphs.push_back(events);
    bp::GraphBuilder b(*blueprint.FindGraph("EventGraph"));
    const bp::NodeId begin = b.Add("Event.BeginPlay"), play = b.Add("Call.Native:Animator.PlayMontage");
    b.Default(play, "montage", "Once").Default(play, "rate", 2.0);
    b.Connect(begin, "then", play, "exec");
    const bp::NodeId on_end = b.Add("Event.OnMontageEnded"), print = b.Add("Debug.Print"), notify = b.Add("Event.OnAnimNotify");
    const bp::NodeId print2 = b.Add("Debug.Print");
    b.Connect(on_end, "then", print, "exec").Connect(on_end, "montage", print, "text");
    b.Connect(notify, "then", print2, "exec").Connect(notify, "name", print2, "text");
    bp::CompileResult compiled = bp::CompileBlueprint(blueprint);
    for (const bp::Diagnostic& d : compiled.diagnostics.diagnostics) std::printf("    %s: %s\n", d.code.c_str(), d.message.c_str());
    AETHER_CHECK(compiled.Ok());
    World game;
    AnimationSystem anims(game, a.Library());
    bp::BlueprintVM vm(game);
    std::vector<std::string> printed;
    vm.SetPrintHandler([&](Entity, const std::string& text) { printed.push_back(text); });
    Animator hero;
    hero.graph = "Loco";
    hero.skeleton = "Rig";
    const Entity e = game.CreateEntity(Transform{Vec3(), Quaternion::Identity()}, hero);
    AETHER_CHECK(vm.Attach(e, compiled.blueprint));
    vm.BeginPlay();
    for (int f = 0; f < 20; ++f) {
        anims.Update(0.05f);
        for (const EntityAnimEvent& ev : anims.Events()) {
            const char* name = AnimationSystem::BlueprintEventName(ev.event.type);
            if (ev.event.type == AnimEventType::MontageEnded) {
                const bp::VmValue args[] = {ev.event.name, ev.event.interrupted};
                vm.Dispatch(ev.entity, name, args);
            } else if (ev.event.type == AnimEventType::Notify) {
                const bp::VmValue args[] = {ev.event.name};
                vm.Dispatch(ev.entity, name, args);
            }
        }
    }
    // At rate 2 the 1 s montage takes 0.5 s: its Hit notify, then its end.
    AETHER_CHECK(printed == (std::vector<std::string>{"Hit", "Once"}));
    AETHER_CHECK(std::string(AnimationSystem::BlueprintEventName(AnimEventType::StateChanged)) == "Event.OnAnimStateChanged");
}
