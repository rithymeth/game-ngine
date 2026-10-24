// M8.6 Character gate: Kael's move set on the generated AETHER-01 rig
// (tools/generate_aether01_kael_rig.py), its graph stepped at varied frame
// rates, and retargeting onto a rig with other names and proportions.
#include "aether/animation/anim_graph.h"
#include "aether/animation/gltf_source.h"
#include "aether/animation/ik.h"
#include "aether/animation/retarget.h"
#include "aether/assets/gltf_loader.h"
#include "test_framework.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <set>
#include <string>
#include <vector>

using namespace aether;
using namespace aether::anim;

namespace {

std::filesystem::path KaelContent() { return std::filesystem::path(AETHER_REPO_ASSETS_DIR).parent_path() / "games/AETHER-01/Content/Characters"; }

bool LoadRig(const char* relative, GltfCharacter& out) {
    assets::GltfScene scene;
    std::string error;
    if (!assets::LoadGltf((KaelContent() / relative).string(), scene, &error)) return false;
    return BuildCharacterFromGltf(scene, 0, out, &error);
}

bool LoadGraph(AnimGraph& out) {
    std::ifstream file(KaelContent() / "Kael/Kael.aanim");
    const nlohmann::json j = nlohmann::json::parse(file, nullptr, false);
    return !j.is_discarded() && AnimGraphFromJson(j, out);
}

struct Kael {
    GltfCharacter rig;
    AnimGraph graph;
    bool ok = false;
    Kael() { ok = LoadRig("Kael/Kael_Rig.gltf", rig) && LoadGraph(graph); }
    AnimAssets Assets() const {
        AnimAssets a;
        a.clip = [this](const std::string& name) -> const AnimationClip* {
            for (const AnimationClip& c : rig.clips) {
                if (c.name == name) return &c;
            }
            return nullptr;
        };
        return a;
    }
};

const Kael& GetKael() {
    static const Kael kael;
    return kael;
}

constexpr u32 kMachine = 20; // the Locomotion state machine node in Kael.aanim

// The script a gameplay frame would run: parameter changes at simulated times, all multiples of 1/6 s so that
// every stepping below reaches them exactly.
struct Input {
    double time;
    std::function<void(AnimGraphInstance&)> apply;
};

std::vector<Input> MoveSetScript() {
    auto f = [](const char* n, f32 v) { return [=](AnimGraphInstance& g) { g.SetFloat(n, v); }; };
    auto b = [](const char* n, bool v) { return [=](AnimGraphInstance& g) { g.SetBool(n, v); }; };
    auto t = [](const char* n) { return [=](AnimGraphInstance& g) { g.SetTrigger(n); }; };
    return {
        {0.5, b("Aiming", true)}, {1.0, b("Aiming", false)}, {1.5, f("Speed", 2)},   {2.5, f("Speed", 6)},  {3.5, f("Speed", 8)},
        {4.5, f("Speed", 6)},     {5.0, f("Speed", 2)},      {5.5, f("Speed", 0)},   {6.0, t("Jump")},      {37.0 / 6, b("Grounded", false)},
        {41.0 / 6, b("Grounded", true)}, {7.5, t("Fire")},         {8.0, t("Reload")},     {9.5, t("Interact")},  {10.5, t("Hit")},
        {11.5, b("Dead", true)},  {13.0, b("Dead", false)},
    };
}

struct Change {
    double time; // the end of the frame it happened in
    std::string from, to;
};
struct Sample {
    double time;
    Pose pose;
    Vec3 root; // accumulated root motion
};
struct RunResult {
    std::vector<Change> changes;
    std::vector<Sample> samples; // every 1/6 s up to 6 s (only parameter-driven transitions before then)
    f32 worst_weight_error = 0.0f;
    bool weights_in_range = true;
};

// Steps the graph for 14 s with the frame times `next_dt` gives.
RunResult Run(const std::function<double(usize)>& next_dt) {
    const Kael& kael = GetKael();
    AnimGraphInstance g(kael.graph, kael.rig.skeleton, kael.Assets());
    g.SetRootMotion(true);
    const std::vector<Input> script = MoveSetScript();
    usize next = 0;
    RunResult r;
    Pose pose;
    Vec3 root(0, 0, 0);
    double now = 0.0;
    int grid = 1;
    for (usize frame = 0; now < 14.0; ++frame) {
        const double dt = next_dt(frame);
        while (next < script.size() && script[next].time <= now + 1e-6) script[next++].apply(g);
        g.Update(static_cast<f32>(dt), pose);
        now += dt;
        root = root + g.RootMotion().translation;
        for (const StateChange& c : g.Changes()) r.changes.push_back({now, c.from, c.to});
        const AnimGraphInstance::MachineBlend blend = g.Blend(kMachine);
        r.worst_weight_error = std::max(r.worst_weight_error, std::fabs(blend.from_weight + blend.to_weight - 1.0f));
        if (blend.from_weight < 0 || blend.from_weight > 1 || blend.to_weight < 0 || blend.to_weight > 1) r.weights_in_range = false;
        if (std::fabs(now - grid / 6.0) < 1e-6 && grid <= 36) {
            r.samples.push_back({now, pose, root});
            ++grid;
        }
    }
    return r;
}

f32 PoseError(const Pose& a, const Pose& b, f32& translation) {
    f32 worst = 0.0f;
    translation = 0.0f;
    for (usize i = 0; i < a.local.size() && i < b.local.size(); ++i) {
        worst = std::max(worst, AngleBetween(a.local[i].rotation, b.local[i].rotation));
        translation = std::max(translation, (a.local[i].translation - b.local[i].translation).Length());
    }
    return worst;
}

} // namespace

AETHER_TEST(Kael_GeneratedRigAndGraphAreValid) {
    const Kael& kael = GetKael();
    AETHER_CHECK(kael.ok);
    if (!kael.ok) return;
    AETHER_CHECK(kael.rig.skeleton.Size() == 13 && kael.rig.skeleton.Valid());
    AETHER_CHECK(kael.rig.skeleton.Find("Root") == 0 && kael.rig.skeleton.Find("Hips") == 1 && kael.rig.skeleton.Find("Shin_R") > 0);
    const std::vector<std::string> move_set = {"Idle", "Walk", "Run", "Sprint", "Jump", "Fall", "Land", "Aim", "Fire", "Reload", "Hit", "Death", "Interact"};
    AETHER_CHECK(kael.rig.clips.size() == move_set.size());
    const AnimAssets assets = kael.Assets();
    for (const std::string& name : move_set) {
        const AnimationClip* clip = assets.clip(name);
        AETHER_CHECK(clip != nullptr && clip->duration > 0.0f && clip->KeyCount() > 0);
    }
    // Walk/Run/Sprint carry root translation at their authored speeds (2, 6, 8 m/s).
    const struct { const char* clip; f32 speed; } moving[] = {{"Walk", 2}, {"Run", 6}, {"Sprint", 8}};
    for (const auto& m : moving) {
        const AnimationClip* clip = assets.clip(m.clip);
        if (!clip) continue;
        const Vec3 end = SampleTrack(clip->tracks[0].translation, clip->duration, Vec3(0, 0, 0));
        AETHER_CHECK(std::fabs(end.z / clip->duration - m.speed) < 1e-3f);
    }
    // No errors and no unreachable states; every state is one of the clips.
    for (const AnimDiagnostic& d : ValidateAnimGraph(kael.graph, &assets)) {
        AETHER_CHECK(false && "Kael.aanim has a diagnostic");
        std::printf("  %s %s\n", d.code.c_str(), d.message.c_str());
    }
    const StateMachine* m = kael.graph.FindMachine("Locomotion");
    AETHER_CHECK(m != nullptr && m->states.size() == move_set.size());
}

AETHER_TEST(Kael_MoveSetTransitionsMatchAt30_60_144HzAndVariableDt) {
    if (!GetKael().ok) { AETHER_CHECK(false); return; }
    // A variable frame time: 30, 60, 40 and 120 Hz frames repeating (each four sum to 1/12 s, so the 1/6 s grid is hit exactly).
    const double pattern[] = {1.0 / 30, 1.0 / 60, 1.0 / 40, 1.0 / 120};
    struct Rate {
        const char* name;
        std::function<double(usize)> dt;
        double frame; // the longest frame
    };
    const Rate rates[] = {{"144 Hz", [](usize) { return 1.0 / 144; }, 1.0 / 144},
                          {"30 Hz", [](usize) { return 1.0 / 30; }, 1.0 / 30},
                          {"60 Hz", [](usize) { return 1.0 / 60; }, 1.0 / 60},
                          {"variable", [&](usize f) { return pattern[f % 4]; }, 1.0 / 30}};
    std::vector<RunResult> results;
    for (const Rate& rate : rates) results.push_back(Run(rate.dt));
    const RunResult& ref = results[0];

    // Every state of the move set is entered, in the authored order.
    const std::vector<std::pair<std::string, std::string>> expected = {
        {"Idle", "Aim"}, {"Aim", "Idle"}, {"Idle", "Walk"}, {"Walk", "Run"}, {"Run", "Sprint"}, {"Sprint", "Run"}, {"Run", "Walk"},
        {"Walk", "Idle"}, {"Idle", "Jump"}, {"Jump", "Fall"}, {"Fall", "Land"}, {"Land", "Idle"}, {"Idle", "Fire"}, {"Fire", "Idle"},
        {"Idle", "Reload"}, {"Reload", "Idle"}, {"Idle", "Interact"}, {"Interact", "Idle"}, {"Idle", "Hit"}, {"Hit", "Idle"},
        {"Idle", "Death"}, {"Death", "Idle"}};
    // When each happens: parameter changes take effect in the next frame; clip-end exits at the end minus the blend.
    const double when[] = {0.5, 1.0, 1.5, 2.5, 3.5, 4.5, 5.0, 5.5, 6.0, 6.2, 41.0 / 6, 41.0 / 6 + 0.15, 7.5, 7.6, 8.0, 8.9, 9.5, 10.2, 10.5, 10.75, 11.5, 13.0};
    const bool param_driven[] = {true, true, true, true, true, true, true, true, true, false, true, false, true, false, true, false, true, false, true, false, true, true};
    for (usize r = 0; r < results.size(); ++r) {
        const RunResult& run = results[r];
        AETHER_CHECK(run.changes.size() == expected.size());
        if (run.changes.size() != expected.size()) {
            for (const Change& c : run.changes) std::printf("  %s %.4f %s -> %s\n", rates[r].name, c.time, c.from.c_str(), c.to.c_str());
            continue;
        }
        const double frame = rates[r].frame + 1e-6;
        for (usize i = 0; i < expected.size(); ++i) {
            const Change& c = run.changes[i];
            AETHER_CHECK(c.from == expected[i].first && c.to == expected[i].second);
            if (!(param_driven[i] ? c.time > when[i] && c.time <= when[i] + frame : std::fabs(c.time - when[i]) <= 2 * frame + 1e-4))
                std::printf("  %s %s->%s at %.4f (authored %.3f)\n", rates[r].name, c.from.c_str(), c.to.c_str(), c.time, when[i]);
            if (param_driven[i]) {
                // A parameter change takes effect during the first frame after it, at every rate ...
                AETHER_CHECK(c.time > when[i] && c.time <= when[i] + frame);
                AETHER_CHECK(std::fabs(c.time - ref.changes[i].time) <= frame + 1.0 / 144);
            } else {
                // ... and a clip-end exit comes when the state has run its authored time, within one frame (a clip's
                // time starts in the frame it is entered, so the exit is within two frames of the authored timeline).
                const double lasted = c.time - run.changes[i - 1].time, authored = when[i] - when[i - 1];
                const double ref_lasted = ref.changes[i].time - ref.changes[i - 1].time;
                AETHER_CHECK(std::fabs(lasted - authored) <= frame + 1e-4);
                AETHER_CHECK(std::fabs(lasted - ref_lasted) <= frame + 1.0 / 144 + 1e-4);
                AETHER_CHECK(std::fabs(c.time - when[i]) <= 2 * frame + 1e-4);
            }
        }
        // Crossfade weights always sum to one.
        AETHER_CHECK(run.weights_in_range && run.worst_weight_error < 1e-5f);
        f32 worst_root = 0.0f;
        // Up to 6 s every transition is parameter-driven, so pose and root motion agree with 144 Hz at the same times.
        AETHER_CHECK(run.samples.size() == ref.samples.size() && run.samples.size() == 36);
        for (usize s = 0; s < run.samples.size() && s < ref.samples.size(); ++s) {
            f32 translation = 0.0f;
            const f32 rotation = PoseError(run.samples[s].pose, ref.samples[s].pose, translation);
            AETHER_CHECK(rotation < 2e-3f && translation < 1e-3f);
            worst_root = std::max(worst_root, (run.samples[s].root - ref.samples[s].root).Length());
        }
        // Crossfades weight root motion by their average over each frame, so it integrates alike at every rate.
        AETHER_CHECK(worst_root < 5e-3f);
    }
    // Root motion moved Kael forward through walk, run and sprint.
    AETHER_CHECK(!ref.samples.empty() && ref.samples.back().root.z > 10.0f);
}

AETHER_TEST(Kael_RetargetsOntoTheWardenRigWithBoundedError) {
    const Kael& kael = GetKael();
    GltfCharacter warden;
    AETHER_CHECK(kael.ok && LoadRig("Warden/Warden_Rig.gltf", warden));
    if (!kael.ok || warden.skeleton.Size() == 0) return;
    const Skeleton& src = kael.rig.skeleton;
    const Skeleton& dst = warden.skeleton;
    // Names differ beyond case and prefix, so auto-mapping finds nothing and says why.
    const BoneMapping automatic = AutoMapBones(src, dst);
    const RetargetReport none = ValidateRetarget(src, dst, automatic);
    AETHER_CHECK(automatic.bones.empty() && !none.compatible && none.issues.size() == 1 && none.issues[0].code == "RT003");
    // The authored name map.
    BoneMapping map;
    const char* pairs[][2] = {{"Root", "base"}, {"Hips", "pelvis"}, {"Spine", "spine_01"}, {"Chest", "spine_02"}, {"Head", "skull"},
                              {"UpperArm_L", "arm_upper_l"}, {"Forearm_L", "arm_lower_l"}, {"UpperArm_R", "arm_upper_r"}, {"Forearm_R", "arm_lower_r"},
                              {"Thigh_L", "leg_upper_l"}, {"Shin_L", "leg_lower_l"}, {"Thigh_R", "leg_upper_r"}, {"Shin_R", "leg_lower_r"}};
    for (const auto& p : pairs) map.bones.push_back({p[0], std::string("warden:") + p[1]});
    const RetargetReport report = ValidateRetarget(src, dst, map, {"warden:pelvis", "warden:skull"});
    AETHER_CHECK(report.compatible && report.mapped == 13 && report.target_bones == 14);
    AETHER_CHECK(report.issues.size() == 1 && report.issues[0].code == "RT008" && !report.issues[0].error &&
                 report.issues[0].message.find("warden:jaw") != std::string::npos);
    const Retargeter retargeter(src, dst, map);
    AETHER_CHECK(retargeter.MappedCount() == 13 && std::fabs(retargeter.TranslationScale() - 1.35f) < 1e-4f);

    // Every mapped bone turns away from the Warden's rest pose as Kael's turns from his (model space), every frame of
    // every clip, though the Warden's arms rest in an A-pose; the Warden keeps its own bone lengths; its root moves 1.35x as far.
    auto target_of = [&](const std::string& source) {
        for (const auto& [from, to] : map.bones) {
            if (from == source) return to;
        }
        return std::string();
    };
    std::vector<ModelTransform> sm, tm, trest, srest;
    ComputeModelTransforms(dst, RestPose(dst), trest);
    ComputeModelTransforms(src, RestPose(src), srest);
    f32 worst_direction = 0.0f, worst_length = 0.0f, worst_root = 0.0f;
    for (const AnimationClip& clip : kael.rig.clips) {
        const AnimationClip moved = retargeter.RetargetClip(clip, 30.0f);
        for (int f = 0; f <= 20; ++f) {
            const f32 t = clip.duration * static_cast<f32>(f) / 20.0f;
            Pose sp, tp;
            SampleClip(clip, src, t, false, sp);
            SampleClip(moved, dst, t, false, tp);
            ComputeModelTransforms(src, sp, sm);
            ComputeModelTransforms(dst, tp, tm);
            for (usize b = 1; b < src.Size(); ++b) {
                const i32 parent = src.bones[b].parent;
                const usize tb = static_cast<usize>(dst.Find(target_of(src.bones[b].name)));
                const usize tparent = static_cast<usize>(dst.Find(target_of(src.bones[static_cast<usize>(parent)].name)));
                // The parent's turn away from its rest pose (model space) turns the Warden's rest bone direction.
                const usize sp_index = static_cast<usize>(parent);
                const Quaternion turn = sm[sp_index].rotation * Conjugate(srest[sp_index].rotation);
                const Vec3 expected = Rotate(turn, trest[tb].position - trest[tparent].position);
                const Vec3 tdir = tm[tb].position - tm[tparent].position;
                if (expected.Length() < 1e-4f) continue;
                const f32 cosine = std::clamp(expected.Normalized().Dot(tdir.Normalized()), -1.0f, 1.0f);
                worst_direction = std::max(worst_direction, std::acos(cosine));
                const f32 rest_length = (trest[tb].position - trest[tparent].position).Length();
                worst_length = std::max(worst_length, std::fabs(tdir.Length() - rest_length));
            }
            const Vec3 sroot = sm[static_cast<usize>(src.Find("Root"))].position, troot = tm[static_cast<usize>(dst.Find("warden:base"))].position;
            worst_root = std::max(worst_root, (troot - sroot * 1.35f).Length());
        }
    }
    std::printf("  retarget: direction %.5f rad, length %.6f m, root %.6f m\n", worst_direction, worst_length, worst_root);
    AETHER_CHECK(worst_direction < 0.02f); // radians, from resampling at 30 keys/s
    AETHER_CHECK(worst_length < 1e-4f);
    AETHER_CHECK(worst_root < 2e-3f);
}

AETHER_TEST(Kael_RetargetReportsIncompatibleSkeletonsClearly) {
    const Kael& kael = GetKael();
    AETHER_CHECK(kael.ok);
    if (!kael.ok) return;
    const Skeleton& src = kael.rig.skeleton;
    auto codes = [](const RetargetReport& r) {
        std::set<std::string> out;
        for (const RetargetIssue& i : r.issues) {
            if (i.error) out.insert(i.code);
        }
        return out;
    };
    // A mapping naming bones neither rig has, and two sources on one target.
    BoneMapping bad;
    bad.bones = {{"Hips", "Hips"}, {"Tail", "Spine"}, {"Spine", "Neck"}, {"Chest", "Spine"}, {"Head", "Spine"}};
    RetargetReport r = ValidateRetarget(src, src, bad);
    AETHER_CHECK(!r.compatible && codes(r) == std::set<std::string>({"RT002", "RT007"}));
    AETHER_CHECK(r.Describe().find("RT002 error [Tail]: The mapping names the source bone 'Tail'") != std::string::npos);
    // A crossed hierarchy: legs mapped onto arms the wrong way round (the shin's source isn't under the thigh's).
    BoneMapping crossed;
    crossed.bones = {{"Root", "Root"}, {"Hips", "Hips"}, {"Thigh_L", "Shin_L"}, {"Shin_L", "Thigh_L"}};
    r = ValidateRetarget(src, src, crossed);
    AETHER_CHECK(!r.compatible && codes(r) == std::set<std::string>({"RT005"}));
    // A root that doesn't move, and a required bone left out.
    BoneMapping rootless;
    rootless.bones = {{"Spine", "Spine"}};
    rootless.root = "Hips";
    r = ValidateRetarget(src, src, rootless, {"Head", "Wing"});
    AETHER_CHECK(!r.compatible && codes(r) == std::set<std::string>({"RT004", "RT006"}));
    // An invalid skeleton (a child before its parent).
    Skeleton broken = src;
    broken.bones[1].parent = 5;
    r = ValidateRetarget(src, broken, AutoMapBones(src, src));
    AETHER_CHECK(!r.compatible && codes(r) == std::set<std::string>({"RT001"}));
    // The identity mapping is compatible with no issues.
    r = ValidateRetarget(src, src, AutoMapBones(src, src));
    AETHER_CHECK(r.compatible && r.issues.empty() && r.Coverage() == 1.0f);
}
