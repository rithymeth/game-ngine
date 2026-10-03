#include "aether/animation/blend_space.h"
#include "test_framework.h"

#include <algorithm>
#include <cmath>
#include <random>

using namespace aether;
using namespace aether::anim;

// Phase 16 step 2: blend spaces, synced playback and root motion.

namespace {

bool Near(const Vec3& a, const Vec3& b, f32 eps = 1e-4f) { return (a - b).Length() < eps; }

f32 WeightOf(const std::vector<BlendWeight>& w, u32 sample) {
    for (const BlendWeight& b : w) {
        if (b.sample == sample) return b.weight;
    }
    return 0.0f;
}

bool SumsToOne(const std::vector<BlendWeight>& w) {
    f32 total = 0.0f;
    for (const BlendWeight& b : w) total += b.weight;
    return std::fabs(total - 1.0f) < 1e-5f;
}

// Where the weights put you: the weighted average of the samples' positions.
std::pair<f32, f32> Reconstruct(const BlendSpace& s, const std::vector<BlendWeight>& w) {
    f32 x = 0, y = 0;
    for (const BlendWeight& b : w) {
        x += s.samples[b.sample].x * b.weight;
        y += s.samples[b.sample].y * b.weight;
    }
    return {x, y};
}

// A one-bone skeleton and a clip whose root walks +Z (2 m per `duration`)
// while turning to `turn` radians, with a bob in Y.
Skeleton OneBone() {
    Skeleton s;
    s.bones.push_back({"root", -1, {}, Mat4::Identity()});
    s.bones.push_back({"spine", 0, {Vec3(0, 1, 0), Quaternion::Identity(), Vec3(1, 1, 1)}, Mat4::Identity()});
    return s;
}
AnimationClip Walk(f32 duration, f32 distance, f32 turn) {
    AnimationClip c;
    c.duration = duration;
    c.tracks.resize(2);
    for (int k = 0; k <= 40; ++k) {
        const f32 u = static_cast<f32>(k) / 40.0f;
        c.tracks[0].translation.times.push_back(u * duration);
        c.tracks[0].translation.values.push_back(Vec3(0, 0.1f * std::sin(u * 2 * kPi), distance * u));
        c.tracks[0].rotation.times.push_back(u * duration);
        c.tracks[0].rotation.values.push_back(Quaternion::FromAxisAngle(Vec3(0, 1, 0), turn * u));
        c.tracks[1].rotation.times.push_back(u * duration);
        c.tracks[1].rotation.values.push_back(Quaternion::FromAxisAngle(Vec3(1, 0, 0), 0.3f * std::sin(u * 2 * kPi)));
    }
    return c;
}

} // namespace

AETHER_TEST(AnimationBlending_Weights1D) {
    BlendSpace s;
    s.x = {"Speed", 0, 600};
    s.samples = {{"Run", 450}, {"Idle", 0}, {"Walk", 150}}; // any order
    AETHER_CHECK(ValidateBlendSpace(s).empty());
    std::vector<BlendWeight> w = ComputeWeights(s, 300);
    AETHER_CHECK(w.size() == 2 && std::fabs(WeightOf(w, 2) - 0.5f) < 1e-5f && std::fabs(WeightOf(w, 0) - 0.5f) < 1e-5f);
    w = ComputeWeights(s, 100);
    AETHER_CHECK(std::fabs(WeightOf(w, 2) - 2.0f / 3.0f) < 1e-5f && w[0].sample == 2 && SumsToOne(w)); // heaviest first
    AETHER_CHECK(ComputeWeights(s, 150).size() == 1 && ComputeWeights(s, 150)[0].sample == 2);
    AETHER_CHECK(ComputeWeights(s, -50)[0].sample == 1 && ComputeWeights(s, 9999)[0].sample == 0); // clamped
    AETHER_CHECK(ComputeWeights(s, 550)[0].sample == 0);                                            // past the last sample
    BlendSpace one;
    one.samples = {{"Idle", 0.5f}};
    AETHER_CHECK(ComputeWeights(one, 0.1f).size() == 1 && ComputeWeights(BlendSpace{}, 0.5f).empty());
}

AETHER_TEST(AnimationBlending_Weights2D) {
    // A cross: four corners and the middle.
    BlendSpace s;
    s.dimensions = 2;
    s.x = {"Right", -1, 1};
    s.y = {"Forward", -1, 1};
    s.samples = {{"BackLeft", -1, -1}, {"BackRight", 1, -1}, {"FwdLeft", -1, 1}, {"FwdRight", 1, 1}, {"Idle", 0, 0}};
    Triangulate(s);
    AETHER_CHECK(s.triangles.size() == 4 && ValidateBlendSpace(s).empty());
    AETHER_CHECK(ComputeWeights(s, 0, 0).size() == 1 && ComputeWeights(s, 0, 0)[0].sample == 4);
    std::mt19937 rng(3);
    std::uniform_real_distribution<f32> d(-1, 1);
    for (int i = 0; i < 200; ++i) {
        const f32 x = d(rng), y = d(rng);
        const std::vector<BlendWeight> w = ComputeWeights(s, x, y);
        const auto [rx, ry] = Reconstruct(s, w);
        AETHER_CHECK(SumsToOne(w) && w.size() <= 3 && std::fabs(rx - x) < 1e-4f && std::fabs(ry - y) < 1e-4f);
    }
    // Outside the axes: clamped first.
    const auto [cx, cy] = Reconstruct(s, ComputeWeights(s, 5, -3));
    AETHER_CHECK(std::fabs(cx - 1) < 1e-4f && std::fabs(cy + 1) < 1e-4f);

    // Outside the hull (but inside the axes): the nearest point on it.
    BlendSpace tri;
    tri.dimensions = 2;
    tri.samples = {{"A", 0, 0}, {"B", 1, 0}, {"C", 0, 1}};
    Triangulate(tri);
    std::vector<BlendWeight> w = ComputeWeights(tri, 1, 1);
    AETHER_CHECK(std::fabs(WeightOf(w, 1) - 0.5f) < 1e-5f && std::fabs(WeightOf(w, 2) - 0.5f) < 1e-5f && WeightOf(w, 0) == 0.0f);

    // A cloud of samples: a Delaunay triangulation (no sample inside any
    // triangle's circumcircle), and weights that reproduce the position.
    BlendSpace cloud;
    cloud.dimensions = 2;
    cloud.x = {"X", 0, 100};
    cloud.y = {"Y", 0, 1};
    std::uniform_real_distribution<f32> u(0, 1);
    for (int i = 0; i < 30; ++i) cloud.samples.push_back({"S" + std::to_string(i), u(rng) * 100, u(rng)});
    Triangulate(cloud);
    AETHER_CHECK(cloud.triangles.size() >= 30);
    for (const auto& t : cloud.triangles) {
        auto p = [&](u32 i) { return std::pair<f64, f64>(cloud.samples[i].x / 100.0, cloud.samples[i].y); };
        auto [ax, ay] = p(t[0]);
        auto [bx, by] = p(t[1]);
        auto [qx, qy] = p(t[2]);
        if ((bx - ax) * (qy - ay) - (by - ay) * (qx - ax) < 0) std::swap(bx, qx), std::swap(by, qy);
        for (u32 k = 0; k < cloud.samples.size(); ++k) {
            if (k == t[0] || k == t[1] || k == t[2]) continue;
            auto [px, py] = p(k);
            const f64 a1 = ax - px, a2 = ay - py, b1 = bx - px, b2 = by - py, c1 = qx - px, c2 = qy - py;
            const f64 det = (a1 * a1 + a2 * a2) * (b1 * c2 - c1 * b2) - (b1 * b1 + b2 * b2) * (a1 * c2 - c1 * a2) +
                            (c1 * c1 + c2 * c2) * (a1 * b2 - b1 * a2);
            AETHER_CHECK(det <= 1e-9);
        }
    }
    // Points inside the hull (mixes of three samples) are reproduced exactly.
    std::uniform_int_distribution<usize> pick(0, cloud.samples.size() - 1);
    for (int i = 0; i < 100; ++i) {
        const BlendSample &a = cloud.samples[pick(rng)], &b = cloud.samples[pick(rng)], &c = cloud.samples[pick(rng)];
        f32 wa = u(rng), wb = u(rng), wc = u(rng);
        const f32 total = wa + wb + wc;
        wa /= total, wb /= total, wc /= total;
        const f32 x = a.x * wa + b.x * wb + c.x * wc, y = a.y * wa + b.y * wb + c.y * wc;
        const std::vector<BlendWeight> w2 = ComputeWeights(cloud, x, y);
        const auto [rx, ry] = Reconstruct(cloud, w2);
        AETHER_CHECK(SumsToOne(w2) && w2.size() <= 3 && std::fabs(rx - x) < 1e-3f && std::fabs(ry - y) < 1e-5f);
    }
}

AETHER_TEST(AnimationBlending_ValidationAndFiles) {
    auto has = [](const std::vector<BlendDiagnostic>& d, const char* code, bool error = true) {
        return std::any_of(d.begin(), d.end(), [&](const BlendDiagnostic& x) { return x.code == code && x.error == error; });
    };
    BlendSpace s;
    AETHER_CHECK(has(ValidateBlendSpace(s), "BS001"));
    s.samples = {{"A", 0.5f}, {"B", 0.5f}};
    AETHER_CHECK(has(ValidateBlendSpace(s), "BS002"));
    s.samples = {{"A", 0.2f}, {"B", 1.5f}};
    AETHER_CHECK(has(ValidateBlendSpace(s), "BS005", false) && !has(ValidateBlendSpace(s), "BS005"));
    s.samples = {{"", 0.2f}, {"B", 0.5f, 0, 0.0f}};
    AETHER_CHECK(has(ValidateBlendSpace(s), "BS006"));
    s.x = {"Speed", 5, 5};
    AETHER_CHECK(has(ValidateBlendSpace(s), "BS004"));
    BlendSpace line;
    line.dimensions = 2;
    line.samples = {{"A", 0, 0}, {"B", 0.5f, 0.5f}, {"C", 1, 1}};
    Triangulate(line);
    AETHER_CHECK(has(ValidateBlendSpace(line), "BS003") && line.triangles.empty());
    const std::vector<BlendWeight> along = ComputeWeights(line, 0.75f, 0.75f); // still blends along the line
    AETHER_CHECK(std::fabs(WeightOf(along, 1) - 0.5f) < 1e-4f && std::fabs(WeightOf(along, 2) - 0.5f) < 1e-4f);
    line.dimensions = 3;
    AETHER_CHECK(has(ValidateBlendSpace(line), "BS004"));

    // Files.
    BlendSpace grid;
    grid.dimensions = 2;
    grid.x = {"Right", -300, 300};
    grid.y = {"Forward", -300, 300};
    grid.samples = {{"Idle", 0, 0}, {"Left", -300, 0, 0.9f}, {"Right", 300, 0}, {"Fwd", 0, 300}};
    Triangulate(grid);
    const nlohmann::json j = BlendSpaceToJson(grid);
    BlendSpace back;
    std::string error;
    AETHER_CHECK(BlendSpaceFromJson(j, back, &error) && back.dimensions == 2 && back.y.name == "Forward" && back.samples.size() == 4);
    AETHER_CHECK(back.samples[1].rate == 0.9f && back.triangles.size() == grid.triangles.size() && BlendSpaceToJson(back) == j);
    for (const char* bad : {R"({"$type": "Blueprint"})", R"({"$type": "BlendSpace", "$version": 7})",
                            R"({"$type": "BlendSpace", "dimensions": 4})", R"({"$type": "BlendSpace", "samples": [{"x": 1}]})",
                            R"({"$type": "BlendSpace", "x": {"min": "a"}})"}) {
        AETHER_CHECK(!BlendSpaceFromJson(nlohmann::json::parse(bad), back, &error) && !error.empty());
    }
}

AETHER_TEST(AnimationBlending_WeightedPosesAndSyncedPlayback) {
    const Skeleton s = OneBone();
    Pose a = RestPose(s), b = RestPose(s), c = RestPose(s);
    a.local[1].translation = Vec3(0, 0, 0);
    b.local[1].translation = Vec3(4, 0, 0);
    const Quaternion turn = Quaternion::FromAxisAngle(Vec3(0, 1, 0), 1.0f);
    b.local[1].rotation = turn;
    c.local[1].rotation = Quaternion(-turn.x, -turn.y, -turn.z, -turn.w); // the same rotation, negated
    Pose out;
    BlendWeighted({&a, &b}, {1.0f, 3.0f}, out); // normalized to 0.25 / 0.75
    AETHER_CHECK(Near(out.local[1].translation, Vec3(3, 0, 0)));
    BlendWeighted({&b, &c}, {0.5f, 0.5f}, out);
    AETHER_CHECK(AngleBetween(out.local[1].rotation, turn) < 1e-4f); // not cancelled out
    BlendWeighted({&a, &b}, {0.0f, 0.0f}, out);
    AETHER_CHECK(Near(out.local[1].translation, Vec3(0, 0, 0))); // no weight: the first

    // Walk (1 s cycle) and run (0.5 s) at 50/50: one cycle takes 0.75 s, both clips in step.
    const AnimationClip walk = Walk(1.0f, 1.5f, 0.0f), run = Walk(0.5f, 2.0f, 0.0f);
    BlendSpace space;
    space.x = {"Speed", 0, 400};
    space.samples = {{"Walk", 150}, {"Run", 350}};
    BlendSpacePlayer player(space, {&walk, &run}, s);
    player.SetParameters(250);
    AETHER_CHECK(std::fabs(player.CycleDuration() - 0.75f) < 1e-5f);
    player.Update(0.375f, out);
    AETHER_CHECK(std::fabs(player.Phase() - 0.5f) < 1e-5f);
    Pose walk_half, run_half, expected;
    SampleClip(walk, s, 0.5f, true, walk_half);
    SampleClip(run, s, 0.25f, true, run_half);
    BlendWeighted({&walk_half, &run_half}, {0.5f, 0.5f}, expected);
    AETHER_CHECK(Near(out.local[0].translation, expected.local[0].translation) && AngleBetween(out.local[1].rotation, expected.local[1].rotation) < 1e-4f);
    // Changing the parameters keeps the phase (no foot pop), only the speed changes.
    player.SetParameters(150);
    AETHER_CHECK(std::fabs(player.CycleDuration() - 1.0f) < 1e-5f && player.Phase() == 0.5f);
    player.Update(0.75f, out);
    AETHER_CHECK(std::fabs(player.Phase() - 0.25f) < 1e-4f); // wrapped
    AETHER_CHECK(player.Weights().size() == 1);
}

AETHER_TEST(AnimationBlending_RootMotion) {
    const Skeleton s = OneBone();
    const AnimationClip walk = Walk(1.0f, 2.0f, kPi / 2); // 2 m forward while turning 90°
    // The first half: 1 m in the starting frame, 45° of turn.
    RootMotionDelta d = ExtractRootMotion(walk, s, 0.0f, 0.5f, false);
    AETHER_CHECK(Near(d.translation, Vec3(0, 0, 1)) && std::fabs(AngleBetween(d.rotation, Quaternion::Identity()) - kPi / 4) < 1e-3f);
    // The second half moves +Z in clip space; expressed in the root's frame after 45° of turn.
    d = ExtractRootMotion(walk, s, 0.5f, 1.0f, false);
    const Vec3 expected = Rotate(Conjugate(Quaternion::FromAxisAngle(Vec3(0, 1, 0), kPi / 4)), Vec3(0, 0, 1));
    AETHER_CHECK(Near(d.translation, expected, 1e-3f));
    // Small steps compose to the whole.
    RootMotionDelta sum;
    for (int i = 0; i < 10; ++i) sum += ExtractRootMotion(walk, s, i * 0.1f, (i + 1) * 0.1f, false);
    const RootMotionDelta whole = ExtractRootMotion(walk, s, 0.0f, 1.0f, false);
    AETHER_CHECK(Near(sum.translation, whole.translation, 1e-3f) && AngleBetween(sum.rotation, whole.rotation) < 1e-3f);
    // Wrapping past the end of a looping clip: the end part, then the start part.
    RootMotionDelta wrapped = ExtractRootMotion(walk, s, 0.75f, 1.0f, false);
    wrapped += ExtractRootMotion(walk, s, 0.0f, 0.25f, false);
    const RootMotionDelta looped = ExtractRootMotion(walk, s, 0.75f, 0.25f, true);
    AETHER_CHECK(Near(looped.translation, wrapped.translation) && AngleBetween(looped.rotation, wrapped.rotation) < 1e-4f);
    // Settings: no yaw, and vertical motion only when asked.
    RootMotionSettings flat;
    flat.yaw = false;
    AETHER_CHECK(AngleBetween(ExtractRootMotion(walk, s, 0.0f, 0.5f, false, flat).rotation, Quaternion::Identity()) < 1e-5f);
    RootMotionSettings vertical;
    vertical.translation_xz = false;
    vertical.translation_y = true;
    const RootMotionDelta bob = ExtractRootMotion(walk, s, 0.0f, 0.25f, false, vertical);
    AETHER_CHECK(std::fabs(bob.translation.y - 0.1f) < 1e-3f && std::fabs(bob.translation.z) < 1e-5f);

    // Stripping: the root stays at its start over the ground, facing its start, and keeps its bob.
    Pose pose;
    SampleClip(walk, s, 0.25f, false, pose);
    StripRootMotion(pose, walk, s);
    AETHER_CHECK(std::fabs(pose.local[0].translation.z) < 1e-5f && std::fabs(pose.local[0].translation.y - 0.1f) < 1e-3f);
    AETHER_CHECK(AngleBetween(YawOf(pose.local[0].rotation), Quaternion::Identity()) < 1e-4f);
    // The yaw of a rotation that also pitches is just its heading.
    const Quaternion q = Quaternion::FromAxisAngle(Vec3(0, 1, 0), 0.7f) * Quaternion::FromAxisAngle(Vec3(1, 0, 0), 0.4f);
    AETHER_CHECK(AngleBetween(YawOf(q), Quaternion::FromAxisAngle(Vec3(0, 1, 0), 0.7f)) < 1e-4f);

    // A blend space player hands out the blended motion and plays in place.
    const AnimationClip slow = Walk(1.0f, 1.0f, 0.0f), fast = Walk(1.0f, 3.0f, 0.0f);
    BlendSpace space;
    space.x = {"Speed", 0, 1};
    space.samples = {{"Slow", 0}, {"Fast", 1}};
    BlendSpacePlayer player(space, {&slow, &fast}, s);
    player.SetParameters(0.5f);
    Vec3 travelled(0, 0, 0);
    Pose out;
    for (int i = 0; i < 20; ++i) {
        RootMotionDelta step;
        player.Update(0.05f, out, &step);
        travelled = travelled + step.translation;
        AETHER_CHECK(std::fabs(out.local[0].translation.z) < 1e-5f);
    }
    AETHER_CHECK(Near(travelled, Vec3(0, 0, 2), 1e-3f)); // the average of 1 m and 3 m per cycle
}
