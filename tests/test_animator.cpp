#include "aether/animation/animator.h"
#include "aether/animation/compression.h"
#include "aether/blueprint/compiler.h"
#include "aether/blueprint/vm.h"
#include "aether/scene/components.h"
#include "aether/scene/serialization.h"
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

Skeleton FootRig() {
    Skeleton s;
    s.bones = {{"pelvis", -1, {Vec3(0, 1, 0), Quaternion::Identity(), Vec3(1, 1, 1)}, Mat4::Identity()},
               {"hip", 0, {Vec3(0, 0, 0), Quaternion::Identity(), Vec3(1, 1, 1)}, Mat4::Identity()},
               {"knee", 1, {Vec3(0, -0.45f, 0.05f), Quaternion::Identity(), Vec3(1, 1, 1)}, Mat4::Identity()},
               {"ankle", 2, {Vec3(0, -0.45f, -0.05f), Quaternion::Identity(), Vec3(1, 1, 1)}, Mat4::Identity()}};
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

// Two constant-speed clips let the crossfade's motion be checked analytically,
// without conflating frame weights with nonlinear motion inside a clip.
AnimGraph Crossfade(f32 duration) {
    AnimGraph g;
    g.variables = {{"Go", VarType::Trigger, 0.0f}};
    AnimNode machine, walk, run;
    machine.id = 1, machine.kind = AnimNodeKind::StateMachine, machine.machine = "Move";
    walk.id = 2, walk.asset = "Walk";
    run.id = 3, run.asset = "Run";
    g.nodes = {machine, walk, run};
    g.output = 1;
    StateMachine m;
    m.name = "Move";
    m.states = {{"Walk", 2}, {"Run", 3}};
    m.transitions = {{0, 1, {{"Go", CompareOp::Triggered, 0}}, false, duration, 0}};
    g.machines = {m};
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
    // FootL at 0.25 s into the walk, in the frame that takes the blend from 0.2 to 0.3: it counts by the frame's average (M8.6).
    AETHER_CHECK(Close(seen, 0.25f));
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

AETHER_TEST(Animator_CrossfadeHitchesIntegrateTheWholeFrame) {
    Assets a;
    a.clips["Walk"] = Clip("Walk", 1, 4.0f, 2.0f);
    a.clips["Run"] = Clip("Run", 2, 4.0f, 6.0f);
    const f32 duration = 0.25f;
    const AnimGraph graph = Crossfade(duration);
    AETHER_CHECK(ValidateAnimGraph(graph).empty());
    for (const f32 dt : {0.125f, 0.25f, 0.5f, 1.0f}) {
        AnimGraphInstance coarse(graph, a.rig, a.Anim()), fine(graph, a.rig, a.Anim());
        coarse.SetRootMotion(true);
        fine.SetRootMotion(true);
        Pose pose, fine_pose;
        coarse.Update(0.0f, pose);
        fine.Update(0.0f, fine_pose);
        AETHER_CHECK(coarse.SetTrigger("Go") && fine.SetTrigger("Go"));
        coarse.Update(dt, pose);
        f32 fine_distance = 0.0f;
        for (int i = 0; i < 16; ++i) {
            fine.Update(dt / 16.0f, fine_pose);
            fine_distance += fine.RootMotion().translation.z;
        }
        const f32 ramp = std::min(dt, duration);
        const f32 target_area = ramp * ramp / (2.0f * duration) + std::max(dt - duration, 0.0f);
        const f32 expected = 2.0f * (dt - target_area) + 6.0f * target_area;
        AETHER_CHECK(Close(coarse.RootMotion().translation.z, expected));
        AETHER_CHECK(Close(fine_distance, expected));
        AETHER_CHECK(Close(coarse.RootMotion().translation.z, fine_distance));
        AETHER_CHECK(coarse.CurrentState(1) == "Run" && coarse.InTransition(1) == (dt < duration));
        AETHER_CHECK(Count(coarse.Events(), AnimEventType::StateChanged) == 1);
        AETHER_CHECK(!pose.local.empty());
        if (!pose.local.empty()) {
            // The pose uses the end weight, not the motion's frame average.
            AETHER_CHECK(Close(pose.local[0].translation.x, 1.0f + std::min(dt / duration, 1.0f)));
            AETHER_CHECK(Close(pose.local[0].translation.z, 0.0f));
        }
        if (dt >= duration) {
            coarse.Update(0.125f, pose);
            AETHER_CHECK(Close(coarse.RootMotion().translation.z, 0.75f));
            AETHER_CHECK(!coarse.InTransition(1) && coarse.Events().empty());
        }
    }
}

AETHER_TEST(Animator_CrossfadeHitchesFinishPartialRampsUnderGraphWeights) {
    Assets a;
    a.clips["Walk"] = Clip("Walk", 1, 4.0f, 2.0f);
    a.clips["Run"] = Clip("Run", 2, 4.0f, 6.0f);
    for (const f32 weight : {1.0f, 0.25f}) {
        AnimGraph graph = Crossfade(0.5f);
        graph.variables.push_back({"Weight", VarType::Float, weight});
        AnimNode idle, blend;
        idle.id = 4, idle.asset = "Idle";
        blend.id = 5, blend.kind = AnimNodeKind::Blend, blend.variable = "Weight", blend.inputs = {4, 1};
        graph.nodes.push_back(idle);
        graph.nodes.push_back(blend);
        graph.output = 5;
        AnimGraphInstance coarse(graph, a.rig, a.Anim()), fine(graph, a.rig, a.Anim());
        coarse.SetRootMotion(true);
        fine.SetRootMotion(true);
        Pose pose;
        coarse.Update(0.0f, pose);
        fine.Update(0.0f, pose);
        AETHER_CHECK(coarse.SetTrigger("Go") && fine.SetTrigger("Go"));
        coarse.Update(0.125f, pose);
        const f32 first = coarse.RootMotion().translation.z;
        AETHER_CHECK(Close(first, 0.3125f * weight) && coarse.InTransition(1));
        const Pose paused_pose = pose;
        coarse.Update(0.0f, pose); // pause an already-progressed ramp
        AETHER_CHECK(coarse.Events().empty() && Close(coarse.RootMotion().translation.z, 0.0f));
        AETHER_CHECK(Close(coarse.Blend(1).to_weight, 0.25f) && coarse.InTransition(1));
        AETHER_CHECK(!pose.local.empty() && pose.local.size() == paused_pose.local.size());
        if (!pose.local.empty() && !paused_pose.local.empty()) {
            AETHER_CHECK((pose.local[0].translation - paused_pose.local[0].translation).Length() < 1e-4f);
        }
        coarse.Update(0.875f, pose); // only 0.375 seconds of ramp remain
        // Target-weight area over the full second is 0.75: 2 * 0.25 + 6 * 0.75 = 5 m.
        AETHER_CHECK(Close(first + coarse.RootMotion().translation.z, 5.0f * weight));
        AETHER_CHECK(!coarse.InTransition(1));
        f32 fine_distance = 0.0f;
        for (int i = 0; i < 32; ++i) {
            fine.Update(1.0f / 32.0f, pose);
            fine_distance += fine.RootMotion().translation.z;
        }
        AETHER_CHECK(Close(fine_distance, 5.0f * weight));
        AETHER_CHECK(Close(first + coarse.RootMotion().translation.z, fine_distance));
    }
}

AETHER_TEST(Animator_CrossfadeHitchesUseFrameAverageNotifyWeights) {
    Assets a;
    a.clips["Walk"] = Clip("Walk", 1, 4.0f, 2.0f);
    a.clips["Run"] = Clip("Run", 2, 4.0f, 6.0f);
    a.clips["Walk"].notifies = {{"Source", 0.1875f}, {"LateSource", 0.75f}};
    a.clips["Run"].notifies = {{"Target", 0.1875f}, {"LateTarget", 0.75f}};
    for (const bool partial : {false, true}) {
        AnimGraph graph = Crossfade(partial ? 0.5f : 0.25f);
        graph.variables.push_back({"Weight", VarType::Float, 0.25f});
        AnimNode idle, blend;
        idle.id = 4, idle.asset = "Idle";
        blend.id = 5, blend.kind = AnimNodeKind::Blend, blend.variable = "Weight", blend.inputs = {4, 1};
        graph.nodes.push_back(idle);
        graph.nodes.push_back(blend);
        graph.output = 5;
        for (const bool root_motion : {false, true}) {
            AnimGraphInstance anim(graph, a.rig, a.Anim());
            anim.SetRootMotion(root_motion);
            Pose pose;
            anim.Update(0.0f, pose);
            AETHER_CHECK(anim.SetTrigger("Go"));
            if (partial) anim.Update(0.125f, pose);
            anim.Update(partial ? 0.875f : 1.0f, pose);
            // Notifications keep the established frame-average policy, not the
            // instantaneous blend weight at the marker's timestamp.
            const f32 target_weight = partial ? 47.0f / 56.0f : 0.875f;
            AETHER_CHECK(Count(anim.Events(), AnimEventType::Notify, "Source") == 1);
            AETHER_CHECK(Count(anim.Events(), AnimEventType::Notify, "Target") == 1);
            // Even markers beyond the ramp retain the whole-frame policy,
            // including the source marker, and compose with the outer weight.
            AETHER_CHECK(Count(anim.Events(), AnimEventType::Notify, "LateSource") == 1);
            AETHER_CHECK(Count(anim.Events(), AnimEventType::Notify, "LateTarget") == 1);
            for (const AnimEvent& e : anim.Events()) {
                if (e.type != AnimEventType::Notify) continue;
                AETHER_CHECK(Close(e.weight, 0.25f * (e.detail == "Walk" ? 1.0f - target_weight : target_weight)));
            }
            anim.Update(0.0f, pose);
            AETHER_CHECK(anim.Events().empty() && Close(anim.RootMotion().translation.z, 0.0f));
        }
    }
}

AETHER_TEST(Animator_CrossfadeHitchesPreserveZeroTimeAndInstantTransitions) {
    Assets a;
    a.clips["Walk"] = Clip("Walk", 1, 4.0f, 2.0f);
    a.clips["Run"] = Clip("Run", 2, 4.0f, 6.0f);
    for (const f32 duration : {0.0f, 0.5f}) {
        const AnimGraph graph = Crossfade(duration);
        AnimGraphInstance anim(graph, a.rig, a.Anim());
        anim.SetRootMotion(true);
        Pose pose;
        anim.Update(0.0f, pose);
        AETHER_CHECK(anim.SetTrigger("Go"));
        anim.Update(0.0f, pose);
        AETHER_CHECK(anim.CurrentState(1) == "Run" && anim.InTransition(1) == (duration > 0.0f));
        AETHER_CHECK(Close(anim.RootMotion().translation.z, 0.0f));
        anim.Update(0.0f, pose);
        AETHER_CHECK(anim.Events().empty() && Close(anim.RootMotion().translation.z, 0.0f));
        anim.Update(0.125f, pose);
        AETHER_CHECK(Close(anim.RootMotion().translation.z, duration > 0.0f ? 0.3125f : 0.75f));
    }
    // Disabling extraction still samples the target at the frame endpoint.
    const AnimGraph graph = Crossfade(0.25f);
    AnimGraphInstance off(graph, a.rig, a.Anim());
    Pose pose;
    off.Update(0.0f, pose);
    AETHER_CHECK(off.SetTrigger("Go"));
    off.Update(1.0f, pose);
    AETHER_CHECK(Close(off.RootMotion().translation.z, 0.0f) && !pose.local.empty());
    if (!pose.local.empty()) {
        AETHER_CHECK(Close(pose.local[0].translation.x, 2.0f) && Close(pose.local[0].translation.z, 6.0f));
    }
}

AETHER_TEST(Animator_ClipHitchesKeepEveryCycle) {
    Assets a;
    AnimNode walk;
    walk.id = 1, walk.asset = "Walk";
    const AnimGraph graph = OneNode(walk);
    for (const f32 dt : {1.0f, 2.25f}) {
        AnimGraphInstance anim(graph, a.rig, a.Anim());
        anim.SetRootMotion(true);
        Pose pose;
        anim.Update(dt, pose);
        AETHER_CHECK(Close(anim.RootMotion().translation.z, 2.0f * dt));
        AETHER_CHECK(Count(anim.Events(), AnimEventType::Notify, "FootL") == (dt == 1.0f ? 1 : 3));
        AETHER_CHECK(Count(anim.Events(), AnimEventType::Notify, "FootR") == (dt == 1.0f ? 1 : 2));
        AETHER_CHECK(!pose.local.empty());
        if (!pose.local.empty()) AETHER_CHECK(Close(pose.local[0].translation.z, 0.0f));
    }
    // A hitch starting late in a cycle must retain its tail, full cycles and head.
    AnimGraphInstance anim(graph, a.rig, a.Anim());
    anim.SetRootMotion(true);
    Pose pose;
    anim.Update(0.875f, pose);
    anim.Update(2.375f, pose); // 0.875 -> 3.25
    AETHER_CHECK(Close(anim.RootMotion().translation.z, 4.75f));
    AETHER_CHECK(Count(anim.Events(), AnimEventType::Notify, "FootL") == 3);
    AETHER_CHECK(Count(anim.Events(), AnimEventType::Notify, "FootR") == 2);
    anim.Update(0.0f, pose);
    AETHER_CHECK(anim.Events().empty() && Close(anim.RootMotion().translation.z, 0.0f));
}

AETHER_TEST(Animator_ClipHitchesKeepWindowAndBoundaryNotifies) {
    Assets a;
    a.clips["Walk"].notifies = {{"Start", 0.0f}, {"End", 1.0f}, {"Trail", 0.2f, 0.3f}, {"Full", 0.0f, 1.0f}};
    AnimNode walk;
    walk.id = 1, walk.asset = "Walk";
    const AnimGraph graph = OneNode(walk);
    AnimGraphInstance anim(graph, a.rig, a.Anim());
    Pose pose;
    anim.Update(0.0f, pose);
    AETHER_CHECK(anim.Events().empty()); // initial time zero is not a wrap
    anim.Update(2.5f, pose);
    AETHER_CHECK(Count(anim.Events(), AnimEventType::Notify, "Start") == 2);
    AETHER_CHECK(Count(anim.Events(), AnimEventType::Notify, "End") == 2);
    AETHER_CHECK(Count(anim.Events(), AnimEventType::NotifyBegin, "Trail") == 3);
    AETHER_CHECK(Count(anim.Events(), AnimEventType::NotifyEnd, "Trail") == 3);
    AETHER_CHECK(Count(anim.Events(), AnimEventType::NotifyBegin, "Full") == 2);
    AETHER_CHECK(Count(anim.Events(), AnimEventType::NotifyEnd, "Full") == 2);
    anim.Update(0.5f, pose); // lands exactly on the next boundary
    AETHER_CHECK(Count(anim.Events(), AnimEventType::Notify, "Start") == 1);
    AETHER_CHECK(Count(anim.Events(), AnimEventType::Notify, "End") == 1);
    AETHER_CHECK(Count(anim.Events(), AnimEventType::NotifyBegin, "Full") == 1);
    AETHER_CHECK(Count(anim.Events(), AnimEventType::NotifyEnd, "Full") == 1);
    anim.Update(0.125f, pose);
    AETHER_CHECK(anim.Events().empty()); // boundary markers must not fire twice
}

AETHER_TEST(Animator_ClipHitchesComposeTurningRootMotion) {
    Assets a;
    AnimationClip& clip = a.clips["Walk"];
    clip.tracks[0].rotation.times = {0.0f, 1.0f};
    clip.tracks[0].rotation.values = {Quaternion::Identity(), Quaternion::FromAxisAngle(Vec3(0, 1, 0), kPi * 0.5f)};
    AnimNode walk;
    walk.id = 1, walk.asset = "Walk";
    const AnimGraph graph = OneNode(walk);
    AnimGraphInstance anim(graph, a.rig, a.Anim());
    anim.SetRootMotion(true);
    Pose pose;
    anim.Update(3.25f, pose);
    RootMotionDelta expected;
    for (int i = 0; i < 3; ++i) expected += ExtractRootMotion(clip, a.rig, 0.0f, 1.0f, false);
    expected += ExtractRootMotion(clip, a.rig, 0.0f, 0.25f, false);
    AETHER_CHECK((anim.RootMotion().translation - expected.translation).Length() < 1e-3f);
    AETHER_CHECK((Rotate(anim.RootMotion().rotation, Vec3(0, 0, 1)) - Rotate(expected.rotation, Vec3(0, 0, 1))).Length() < 1e-3f);
    // Keep the full turn angle before weighting it with another graph branch.
    AnimNode idle, blend;
    idle.id = 2, idle.asset = "Idle";
    blend.id = 3, blend.kind = AnimNodeKind::Blend, blend.inputs = {2, 1}, blend.variable = "Speed";
    AnimGraph weighted = graph;
    weighted.nodes.push_back(idle);
    weighted.nodes.push_back(blend);
    weighted.output = 3; // OneNode's Speed defaults to 0.5
    AnimGraphInstance half(weighted, a.rig, a.Anim());
    half.SetRootMotion(true);
    half.Update(5.0f, pose);
    const Quaternion turn = Quaternion::FromAxisAngle(Vec3(0, 1, 0), kPi * 1.25f);
    AETHER_CHECK((Rotate(half.RootMotion().rotation, Vec3(0, 0, 1)) - Rotate(turn, Vec3(0, 0, 1))).Length() < 1e-3f);
    // The same quarter turn must not become -270 degrees when the imported
    // root heading crosses the +/-pi representation boundary.
    clip.tracks[0].rotation.values = {Quaternion::FromAxisAngle(Vec3(0, 1, 0), kPi * 17.0f / 18.0f),
                                     Quaternion::FromAxisAngle(Vec3(0, 1, 0), kPi * 26.0f / 18.0f)};
    AnimGraphInstance heading_boundary(weighted, a.rig, a.Anim());
    heading_boundary.SetRootMotion(true);
    heading_boundary.Update(5.0f, pose);
    AETHER_CHECK((Rotate(heading_boundary.RootMotion().rotation, Vec3(0, 0, 1)) - Rotate(turn, Vec3(0, 0, 1))).Length() < 1e-3f);
    // Equal endpoint headings can still contain a full turn along the key path.
    clip.tracks[0].rotation.times = {0.0f, 0.25f, 0.5f, 0.75f, 1.0f};
    clip.tracks[0].rotation.values.clear();
    for (int key = 0; key < 5; ++key) clip.tracks[0].rotation.values.push_back(Quaternion::FromAxisAngle(Vec3(0, 1, 0), key * kPi * 0.5f));
    AnimGraphInstance winding(weighted, a.rig, a.Anim());
    winding.SetRootMotion(true);
    winding.Update(3.0f, pose); // three full turns, half weighted -> 1.5 turns
    AETHER_CHECK((Rotate(winding.RootMotion().rotation, Vec3(0, 0, 1)) - Vec3(0, 0, -1)).Length() < 1e-3f);
}

AETHER_TEST(Animator_ClipHitchesRespectSharedNodeWeights) {
    Assets a;
    AnimNode walk, blend;
    walk.id = 1, walk.asset = "Walk";
    blend.id = 2, blend.kind = AnimNodeKind::Blend, blend.inputs = {1, 1}, blend.variable = "Speed";
    AnimGraph graph = OneNode(walk);
    graph.variables[0].default_value = 0.25f;
    graph.nodes.push_back(blend);
    graph.output = 2;
    AnimGraphInstance anim(graph, a.rig, a.Anim());
    anim.SetRootMotion(true);
    Pose pose;
    anim.Update(2.25f, pose);
    AETHER_CHECK(Close(anim.RootMotion().translation.z, 4.5f));
    AETHER_CHECK(Count(anim.Events(), AnimEventType::Notify) == 5);
    for (const AnimEvent& event : anim.Events()) AETHER_CHECK(Close(event.weight, 1.0f));
}

AETHER_TEST(Animator_ClipHitchesPreserveClampedAndPausedPlayback) {
    Assets a;
    AnimNode walk;
    walk.id = 1, walk.asset = "Walk", walk.loop = false;
    const AnimGraph once = OneNode(walk);
    AnimGraphInstance anim(once, a.rig, a.Anim());
    anim.SetRootMotion(true);
    Pose pose;
    anim.Update(2.25f, pose);
    AETHER_CHECK(Close(anim.RootMotion().translation.z, 2.0f));
    AETHER_CHECK(Count(anim.Events(), AnimEventType::Notify) == 2);
    anim.Update(1.0f, pose);
    AETHER_CHECK(anim.Events().empty() && Close(anim.RootMotion().translation.z, 0.0f));
    walk.loop = true, walk.rate = 0.0f;
    const AnimGraph paused_graph = OneNode(walk);
    AnimGraphInstance paused(paused_graph, a.rig, a.Anim());
    paused.SetRootMotion(true);
    paused.Update(2.25f, pose);
    AETHER_CHECK(paused.Events().empty() && Close(paused.RootMotion().translation.z, 0.0f));
    walk.rate = 1.0f;
    a.clips["Walk"].duration = 0.0f;
    const AnimGraph empty_graph = OneNode(walk);
    AnimGraphInstance empty(empty_graph, a.rig, a.Anim());
    empty.SetRootMotion(true);
    empty.Update(2.25f, pose);
    AETHER_CHECK(empty.Events().empty() && Close(empty.RootMotion().translation.z, 0.0f));
}

AETHER_TEST(Animator_ClipHitchesUseDurationAndPlaybackRate) {
    Assets a;
    a.clips["Walk"] = Clip("Walk", 1, 0.25f, 2.0f);
    a.clips["Walk"].notifies = {{"Step", 0.125f}};
    AnimNode walk;
    walk.id = 1, walk.asset = "Walk", walk.rate = 3.0f;
    const AnimGraph graph = OneNode(walk);
    AnimGraphInstance anim(graph, a.rig, a.Anim());
    anim.SetRootMotion(true);
    Pose pose;
    anim.Update(0.375f, pose); // 1.125 clip seconds: 4.5 cycles
    AETHER_CHECK(Close(anim.RootMotion().translation.z, 2.25f));
    AETHER_CHECK(Count(anim.Events(), AnimEventType::Notify, "Step") == 5);
    // With no notify output, motion accumulation should not iterate a million cycles.
    a.clips["Walk"].notifies.clear();
    AnimGraphInstance many(graph, a.rig, a.Anim());
    many.SetRootMotion(true);
    many.Update(1000000.0f, pose);
    AETHER_CHECK(Close(many.RootMotion().translation.z, 6000000.0f, 1.0f));
    AETHER_CHECK(many.Events().empty());
    // A rounded double quotient must not add a phantom cycle. The exact
    // binary-float interval crosses 1,463,630,740 cycles (a multiple of four).
    AnimationClip& turning = a.clips["Walk"];
    turning.duration = 1e-6f;
    turning.tracks[0].translation.values = {Vec3(), Vec3()};
    turning.tracks[0].translation.times = {0.0f, turning.duration};
    turning.tracks[0].rotation.times = {0.0f, turning.duration};
    turning.tracks[0].rotation.values = {Quaternion::Identity(), Quaternion::FromAxisAngle(Vec3(0, 1, 0), kPi * 0.5f)};
    walk.rate = 1.0f;
    const AnimGraph turning_graph = OneNode(walk);
    AnimGraphInstance rounded(turning_graph, a.rig, a.Anim());
    rounded.SetRootMotion(true);
    constexpr f32 interval = 1463.6307373046875f;
    rounded.Update(interval, pose);
    RootMotionDelta expected, cycle = ExtractRootMotion(turning, a.rig, 0.0f, turning.duration, false);
    // The sampled quaternion is only approximately a quarter turn. Compose it
    // using the known integer count rather than assuming exact cancellation.
    for (u64 count = 1463630740ull; count != 0; count >>= 1) {
        if (count & 1ull) expected += cycle;
        cycle += cycle;
    }
    expected += ExtractRootMotion(turning, a.rig, 0.0f, ClipTime(turning, interval, true), false);
    AETHER_CHECK((Rotate(rounded.RootMotion().rotation, Vec3(0, 0, 1)) - Rotate(expected.rotation, Vec3(0, 0, 1))).Length() < 1e-3f);
    turning.duration = 1e-20f;
    turning.tracks[0].translation.times = turning.tracks[0].rotation.times = {0.0f, turning.duration};
    AnimNode idle, blend;
    idle.id = 2, idle.asset = "Idle";
    blend.id = 3, blend.kind = AnimNodeKind::Blend, blend.inputs = {2, 1}, blend.variable = "Speed";
    AnimGraph enormous_graph = turning_graph;
    enormous_graph.nodes.push_back(idle);
    enormous_graph.nodes.push_back(blend);
    enormous_graph.output = 3;
    AnimGraphInstance enormous(enormous_graph, a.rig, a.Anim()); // half-weight the enormous turn before reduction
    enormous.SetRootMotion(true);
    enormous.Update(1e20f, pose);
    const Quaternion rotation = enormous.RootMotion().rotation;
    AETHER_CHECK(std::isfinite(rotation.x) && std::isfinite(rotation.y) && std::isfinite(rotation.z) && std::isfinite(rotation.w));
}

AETHER_TEST(Animator_ClipHitchesMatchLocalApiAtFractionalBoundaries) {
    Assets a;
    a.clips["Walk"] = Clip("Walk", 0, 0.3f, 2.0f);
    AnimationClip& clip = a.clips["Walk"];
    clip.notifies = {{"Late", 0.24f}, {"Zero", 0.0f}, {"Window", 0.06f, 0.03f}, {"Early", 0.03f}, {"End", clip.duration}};
    AnimNode walk;
    walk.id = 1, walk.asset = "Walk";
    const AnimGraph graph = OneNode(walk);
    auto compare = [&](f32 start, f32 dt) {
        AnimGraphInstance anim(graph, a.rig, a.Anim());
        anim.SetRootMotion(true);
        Pose pose;
        anim.Update(start, pose);
        anim.Update(dt, pose);
        const f32 end = start + dt;
        const f32 local_start = ClipTime(clip, start, true), local_end = ClipTime(clip, end, true);
        std::vector<NotifyPoint> expected;
        RootMotionDelta motion;
        if (end >= clip.duration && start == 0.0f) {
            CollectNotifies(clip, 0.0f, clip.duration, false, expected);
            CollectNotifies(clip, clip.duration, local_end, true, expected);
            motion = ExtractRootMotion(clip, a.rig, 0.0f, clip.duration, false);
            motion += ExtractRootMotion(clip, a.rig, 0.0f, local_end, false);
        } else {
            CollectNotifies(clip, local_start, local_end, true, expected);
            motion = ExtractRootMotion(clip, a.rig, local_start, local_end, true);
        }
        AETHER_CHECK((anim.RootMotion().translation - motion.translation).Length() < 1e-5f);
        AETHER_CHECK(anim.Events().size() == expected.size());
        for (usize i = 0; i < std::min(anim.Events().size(), expected.size()); ++i) {
            const NotifyPoint& point = expected[i];
            const AnimEventType type = !point.window ? AnimEventType::Notify : point.end ? AnimEventType::NotifyEnd : AnimEventType::NotifyBegin;
            AETHER_CHECK(anim.Events()[i].type == type && anim.Events()[i].name == point.notify->name);
        }
    };
    compare(0.0f, std::nextafter(clip.duration, 0.0f));
    compare(0.0f, clip.duration);
    compare(0.0f, std::nextafter(clip.duration, 1.0f));
    compare(0.27f, 0.15f); // ordinary single wrap, compared directly with the public APIs
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
    world.GetComponent<Animator>(actors[0])->SetBoolParameter("Speed", true); // name exists, but has the wrong type
    world.GetComponent<Animator>(actors[0])->SetFloatParameter("UnknownParameter", 1.0f);
    world.GetComponent<Animator>(actors[1])->PlayMontage("Combo", 1.0f);
    world.GetComponent<Animator>(actors[2])->PlayMontage("Nope", 1.0f);
    world.GetComponent<Animator>(actors[2])->JumpToSection("MissingSection");
    system.Update(0.05f);
    AETHER_CHECK(world.GetComponent<Animator>(actors[0])->commands.empty());
    AETHER_CHECK(system.InstanceOf(actors[0])->CurrentState(2) == "Walk" && system.InstanceOf(actors[1])->IsPlayingMontage());
    AETHER_CHECK(system.Problems().size() == 5);
    AETHER_CHECK(system.Problems()[1].find("Speed") != std::string::npos && system.Problems()[1].find("bool") != std::string::npos);
    AETHER_CHECK(system.Problems()[2].find("UnknownParameter") != std::string::npos);
    AETHER_CHECK(system.Problems()[3].find("can't play the montage 'Nope'") != std::string::npos);
    AETHER_CHECK(system.Problems()[4].find("MissingSection") != std::string::npos);
    bool changed = false, started = false;
    for (const EntityAnimEvent& e : system.Events()) {
        if (e.entity == actors[0] && e.event.type == AnimEventType::StateChanged && e.event.name == "Walk") changed = true;
        if (e.entity == actors[1] && e.event.type == AnimEventType::MontageStarted) started = true;
    }
    AETHER_CHECK(changed && started);
    world.GetComponent<Animator>(actors[2])->PlayMontage("Nope", 1.0f);
    world.GetComponent<Animator>(actors[2])->JumpToSection("MissingSection");
    system.Update(0.05f);
    AETHER_CHECK(system.Problems().size() == 5); // repeated bad commands stay deduplicated

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

AETHER_TEST(Animator_EcsLookAtRunsAfterTheGraphInEntitySpace) {
    Assets a;
    World world;
    AnimationSystem system(world, a.Library());
    Transform transform{Vec3(5, 0, 2), Quaternion::FromAxisAngle(Vec3(0, 1, 0), kPi * 0.5f)};
    AnimationLookAt look_at;
    look_at.bone = "arm";
    look_at.max_angle = kPi;
    // The arm should aim along model +X. Give the API that point in world space
    // to verify the entity transform is removed before the IK solver runs.
    look_at.target = transform.position + Rotate(transform.rotation, Vec3(3, 1, 0));
    Animator animator;
    animator.graph = "Loco";
    animator.skeleton = "Rig";
    const Entity entity = world.CreateEntity(transform, animator, look_at);

    system.Update(0.01f);
    const Pose* aimed_pose = system.PoseOf(entity);
    AETHER_CHECK(aimed_pose != nullptr && system.SkinMatricesOf(entity)->size() == 2);
    std::vector<ModelTransform> model;
    ComputeModelTransforms(a.rig, *aimed_pose, model);
    AETHER_CHECK((Rotate(model[1].rotation, Vec3(0, 0, 1)) - Vec3(1, 0, 0)).Length() < 1e-3f);

    // Disabling the ECS component restores the graph pose on the next frame.
    world.GetComponent<AnimationLookAt>(entity)->enabled = false;
    system.Update(0.01f);
    ComputeModelTransforms(a.rig, *system.PoseOf(entity), model);
    AETHER_CHECK((Rotate(model[1].rotation, Vec3(0, 0, 1)) - Vec3(0, 0, 1)).Length() < 1e-3f);

    // Bad authored data is reported once and does not break the animator.
    AnimationLookAt* invalid = world.GetComponent<AnimationLookAt>(entity);
    invalid->enabled = true;
    invalid->bone = "missing";
    system.Update(0.01f);
    const usize problems = system.Problems().size();
    system.Update(0.01f);
    AETHER_CHECK(system.PoseOf(entity) != nullptr && system.Problems().size() == problems);
    AETHER_CHECK(problems == 1 && system.Problems()[0].find("missing") != std::string::npos);

    // The component is reflected and registered before scene loading, so an
    // authored NPC or character keeps its aim constraint across save/load.
    World persisted;
    AnimationLookAt saved;
    saved.bone = "head";
    saved.target = Vec3(3, 2, 1);
    saved.weight = 0.4f;
    AnimationTwoBoneIK saved_reach;
    AnimationTwoBoneIKChain& saved_chain = saved_reach.chains.emplace_back();
    saved_chain.root = "hip";
    saved_chain.mid = "knee";
    saved_chain.end = "ankle";
    saved_chain.target = Vec3(1, 0, 2);
    saved_chain.pole = Vec3(0, 0, 1);
    saved_chain.weight = 0.6f;
    persisted.CreateEntity(saved, saved_reach);
    const std::string path = "/tmp/aether_test_animation_look_at.ascene";
    AETHER_CHECK(SaveSceneJson(persisted, path));
    World reloaded;
    RegisterAnimationComponents();
    AETHER_CHECK(LoadSceneJson(reloaded, path));
    usize constraints = 0;
    reloaded.ForEach<AnimationLookAt, AnimationTwoBoneIK>([&](AnimationLookAt& restored, AnimationTwoBoneIK& restored_reach) {
        ++constraints;
        AETHER_CHECK(restored.bone == "head" && restored.target.x == 3.0f && restored.target.y == 2.0f &&
                     restored.target.z == 1.0f && restored.weight == 0.4f);
        AETHER_CHECK(restored_reach.chains.size() == 1 && restored_reach.chains[0].root == "hip" &&
                     restored_reach.chains[0].mid == "knee" && restored_reach.chains[0].end == "ankle" &&
                     restored_reach.chains[0].target.z == 2.0f && restored_reach.chains[0].weight == 0.6f);
    });
    AETHER_CHECK(constraints == 1);
    std::remove(path.c_str());
}

AETHER_TEST(Animator_EcsTwoBoneIkUsesWorldTargetsAndReportsBadChains) {
    Assets a;
    a.rig.bones = {{"shoulder", -1, {}, Mat4::Identity()},
                   {"elbow", 0, {Vec3(1, 0, 0), Quaternion::Identity(), Vec3(1, 1, 1)}, Mat4::Identity()},
                   {"hand", 1, {Vec3(1, 0, 0), Quaternion::Identity(), Vec3(1, 1, 1)}, Mat4::Identity()}};
    World world;
    AnimationSystem system(world, a.Library());
    Transform transform{Vec3(5, 0, 2), Quaternion::FromAxisAngle(Vec3(0, 1, 0), kPi * 0.5f)};
    AnimationTwoBoneIK component;
    AnimationTwoBoneIKChain& chain = component.chains.emplace_back();
    chain.root = "shoulder";
    chain.mid = "elbow";
    chain.end = "hand";
    const Vec3 target(1, 1, 0), pole(0, 0, 1);
    chain.target = transform.position + Rotate(transform.rotation, target);
    chain.pole = transform.position + Rotate(transform.rotation, pole);
    Animator animator;
    animator.graph = "Loco";
    animator.skeleton = "Rig";
    const Entity entity = world.CreateEntity(transform, animator, component);

    system.Update(0.01f);
    std::vector<ModelTransform> model;
    ComputeModelTransforms(a.rig, *system.PoseOf(entity), model);
    AETHER_CHECK((model[2].position - target).Length() < 1e-3f && system.SkinMatricesOf(entity)->size() == 3);

    // An authored chain must be contiguous; invalid data is diagnosed only once.
    AnimationTwoBoneIK* invalid = world.GetComponent<AnimationTwoBoneIK>(entity);
    invalid->chains[0].end = "shoulder";
    system.Update(0.01f);
    const usize problems = system.Problems().size();
    system.Update(0.01f);
    AETHER_CHECK(system.PoseOf(entity) != nullptr && system.Problems().size() == problems);
    AETHER_CHECK(problems == 1 && system.Problems()[0].find("invalid two-bone") != std::string::npos);
}

AETHER_TEST(Animator_EcsFootPlacementUsesTheGroundProvider) {
    Assets a;
    a.rig = FootRig();
    // The generic idle clip writes a root marker at the origin. Keep this
    // fixture's pelvis at its authored 1 m bind height instead.
    a.clips["Idle"].tracks.resize(4);
    a.clips["Idle"].tracks[0].translation.values = {Vec3(0, 1, 0), Vec3(0, 1, 0)};
    AnimationLibrary library = a.Library();
    library.ground_trace = [](Entity, const Vec3& from, const Vec3& to, Vec3& hit, Vec3& normal) {
        constexpr f32 ground = -0.2f;
        if (ground > from.y || ground < to.y) return false;
        hit = Vec3(from.x, ground, from.z);
        normal = Vec3(0, 1, 0);
        return true;
    };
    World world;
    AnimationSystem system(world, std::move(library));
    AnimationFootPlacement placement;
    placement.pelvis = "pelvis";
    placement.legs.push_back({"hip", "knee", "ankle", 0.1f});
    Animator animator;
    animator.graph = "Loco";
    animator.skeleton = "Rig";
    const Entity entity = world.CreateEntity(Transform{Vec3(), Quaternion::Identity()}, animator, placement);

    system.Update(0.016f);
    std::vector<ModelTransform> model;
    ComputeModelTransforms(a.rig, *system.PoseOf(entity), model);
    AETHER_CHECK(std::fabs(model[0].position.y - 0.8f) < 1e-3f && std::fabs(model[3].position.y + 0.1f) < 1e-3f);
    AETHER_CHECK(system.SkinMatricesOf(entity)->size() == 4 && system.Problems().empty());
}
