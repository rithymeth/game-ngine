#include "aether/animation/compression.h"
#include "test_framework.h"

#include <cmath>

using namespace aether;
using namespace aether::anim;

// Phase 16 step 1: skeletons, poses, clips, compression and blending.

namespace {

bool Near(const Vec3& a, const Vec3& b, f32 eps = 1e-4f) { return (a - b).Length() < eps; }
Vec3 PointOf(const Mat4& m) { return Vec3(m.cols[3].x, m.cols[3].y, m.cols[3].z); }
Vec3 Apply(const Mat4& m, const Vec3& p) {
    const Vec4 v = m * Vec4(p.x, p.y, p.z, 1.0f);
    return Vec3(v.x, v.y, v.z);
}

assets::ModelNodeData Node(const Vec3& t, std::vector<u32> children = {}) {
    assets::ModelNodeData n;
    n.translation = t;
    n.children = std::move(children);
    return n;
}

// 0 root (not a joint) -> 1 hips -> 2 offset (not a joint) -> 3 spine -> 4 head; 1 -> 5 leg.
// The skin lists joints out of order; 6 is an animated prop that isn't a joint.
assets::ModelData Model() {
    assets::ModelData m;
    m.nodes = {Node(Vec3(0, 0, 5), {1}), Node(Vec3(0, 1, 0), {2, 5}), Node(Vec3(0, 0.5f, 0), {3}), Node(Vec3(0, 0.25f, 0), {4}),
               Node(Vec3(0, 0.5f, 0)),     Node(Vec3(0.2f, -0.1f, 0)), Node(Vec3(1, 0, 0))};
    m.nodes[4].rotation = Quaternion::FromAxisAngle(Vec3(0, 0, 1), 0.3f);
    m.root_nodes = {0, 6};
    assets::SkinData skin;
    skin.joints = {4, 1, 5, 3};
    m.skins.push_back(skin);
    return m;
}

Skeleton MakeSkeleton(std::vector<u32>* nodes = nullptr) {
    Skeleton s;
    const std::vector<std::string> names = {"root", "hips", "offset", "spine", "head", "leg", "prop"};
    std::string error;
    AETHER_CHECK(BuildSkeleton(Model(), 0, s, nodes, &names, &error));
    return s;
}

} // namespace

AETHER_TEST(Animation_RotationHelpers) {
    const Quaternion q = Quaternion::FromAxisAngle(Vec3(0.3f, 1, -0.2f), 1.1f);
    const Vec3 v(1, 2, 3);
    AETHER_CHECK(Near(Rotate(q, v), Apply(q.ToMat4(), v)));
    AETHER_CHECK(Near(Rotate(Conjugate(q), Rotate(q, v)), v));
    const Quaternion a = Quaternion::Identity(), b = Quaternion::FromAxisAngle(Vec3(0, 1, 0), 1.6f);
    AETHER_CHECK(AngleBetween(Slerp(a, b, 0.0f), a) < 1e-4f && AngleBetween(Slerp(a, b, 1.0f), b) < 1e-4f);
    AETHER_CHECK(std::fabs(AngleBetween(Slerp(a, b, 0.25f), a) - 0.4f) < 1e-4f); // constant angular speed
    AETHER_CHECK(std::fabs(AngleBetween(Nlerp(a, b, 0.5f), a) - 0.8f) < 1e-4f);  // symmetric at the middle
    // The shorter arc, even when a key is stored negated.
    const Quaternion nb(-b.x, -b.y, -b.z, -b.w);
    AETHER_CHECK(AngleBetween(Nlerp(a, nb, 0.5f), Nlerp(a, b, 0.5f)) < 1e-4f && AngleBetween(b, nb) < 1e-4f);
    AETHER_CHECK(std::fabs(AngleBetween(a, b) - 1.6f) < 1e-4f);
}

AETHER_TEST(Animation_SkeletonsFromSkins) {
    std::vector<u32> nodes;
    const Skeleton s = MakeSkeleton(&nodes);
    AETHER_CHECK(s.Size() == 4 && s.Valid());
    AETHER_CHECK(s.bones[0].name == "hips" && s.bones[0].parent == -1 && nodes[0] == 1);
    const i32 spine = s.Find("spine"), head = s.Find("head"), leg = s.Find("leg"), hips = s.Find("hips");
    AETHER_CHECK(spine > 0 && head > spine && leg > 0 && s.Find("offset") == -1 && s.Find("nope") == -1);
    AETHER_CHECK(s.bones[static_cast<usize>(spine)].parent == hips && s.bones[static_cast<usize>(head)].parent == spine);
    AETHER_CHECK(s.IsAncestor(hips, head) && s.IsAncestor(head, head) && !s.IsAncestor(leg, head));
    // The non-joint node between hips and spine is folded into the spine's rest pose.
    AETHER_CHECK(Near(s.bones[static_cast<usize>(spine)].rest.translation, Vec3(0, 0.75f, 0)));
    AETHER_CHECK(AngleBetween(s.bones[static_cast<usize>(head)].rest.rotation, Quaternion::FromAxisAngle(Vec3(0, 0, 1), 0.3f)) < 1e-4f);

    // Model space: the head sits at hips + 0.75 + 0.5, with the root node above hips folded in (z = 5).
    std::vector<Mat4> model;
    LocalToModel(s, RestPose(s), model);
    AETHER_CHECK(Near(PointOf(model[static_cast<usize>(head)]), Vec3(0, 2.25f, 5)));
    AETHER_CHECK(Near(PointOf(model[static_cast<usize>(leg)]), Vec3(0.2f, 0.9f, 5)));
    // A skin built with the rest pose's inverse binds gives identity skin matrices at rest.
    Skeleton bound = s;
    for (usize i = 0; i < bound.Size(); ++i) {
        const Mat4& m = model[i];
        // Rigid (rotation + translation): the inverse is the transposed rotation and -R^T t.
        Mat4 inv = Mat4::Identity();
        auto at = [](const Vec4& v, int c) { return c == 0 ? v.x : c == 1 ? v.y : v.z; };
        for (int c = 0; c < 3; ++c) inv.cols[c] = Vec4(at(m.cols[0], c), at(m.cols[1], c), at(m.cols[2], c), 0);
        const Vec3 t = PointOf(m);
        inv.cols[3] = Vec4(-(t.x * m.cols[0].x + t.y * m.cols[0].y + t.z * m.cols[0].z),
                           -(t.x * m.cols[1].x + t.y * m.cols[1].y + t.z * m.cols[1].z),
                           -(t.x * m.cols[2].x + t.y * m.cols[2].y + t.z * m.cols[2].z), 1);
        bound.bones[i].inverse_bind = inv;
    }
    std::vector<Mat4> skin;
    SkinMatrices(bound, model, skin);
    for (const Mat4& m : skin) AETHER_CHECK(Near(Apply(m, Vec3(1, 2, 3)), Vec3(1, 2, 3)));

    // A joint given as a matrix is decomposed.
    assets::ModelData with_matrix = Model();
    BoneTransform trs;
    trs.translation = Vec3(1, 2, 3);
    trs.rotation = Quaternion::FromAxisAngle(Vec3(1, 1, 0), 2.5f);
    trs.scale = Vec3(2, 0.5f, 1.5f);
    with_matrix.nodes[5].uses_matrix = true;
    with_matrix.nodes[5].matrix = trs.ToMat4();
    Skeleton m2;
    AETHER_CHECK(BuildSkeleton(with_matrix, 0, m2));
    const BoneTransform& back = m2.bones[static_cast<usize>(m2.Find("bone_5"))].rest;
    AETHER_CHECK(Near(back.translation, trs.translation) && Near(back.scale, trs.scale) && AngleBetween(back.rotation, trs.rotation) < 1e-3f);

    // Bad skins.
    Skeleton none;
    std::string error;
    AETHER_CHECK(!BuildSkeleton(Model(), 1, none, nullptr, nullptr, &error) && !error.empty());
    assets::ModelData bad = Model();
    bad.skins[0].joints.push_back(99);
    AETHER_CHECK(!BuildSkeleton(bad, 0, none, nullptr, nullptr, &error));
    bad = Model();
    bad.skins[0].joints.push_back(4);
    AETHER_CHECK(!BuildSkeleton(bad, 0, none, nullptr, nullptr, &error));
    bad.skins[0].joints.clear();
    AETHER_CHECK(!BuildSkeleton(bad, 0, none, nullptr, nullptr, &error));
    Skeleton dup = s;
    dup.bones[1].name = dup.bones[0].name;
    AETHER_CHECK(!dup.Valid(&error));
}

AETHER_TEST(Animation_ClipsSample) {
    std::vector<u32> nodes;
    const Skeleton s = MakeSkeleton(&nodes);
    assets::AnimationData anim;
    anim.name = "Walk";
    anim.duration = 2.0f;
    assets::AnimationChannelData move; // hips translation, linear
    move.node = 1;
    move.path = assets::AnimationPath::Translation;
    move.times = {0.0f, 1.0f, 2.0f};
    move.values = {0, 1, 0, 2, 1, 0, 2, 3, 0};
    assets::AnimationChannelData turn; // spine rotation, 0 -> 90° about Y
    turn.node = 3;
    turn.path = assets::AnimationPath::Rotation;
    turn.times = {0.0f, 2.0f};
    const Quaternion q90 = Quaternion::FromAxisAngle(Vec3(0, 1, 0), kPi / 2);
    turn.values = {0, 0, 0, 1, q90.x, q90.y, q90.z, q90.w};
    assets::AnimationChannelData blink; // head scale, held
    blink.node = 4;
    blink.path = assets::AnimationPath::Scale;
    blink.interpolation = assets::AnimationInterpolation::Step;
    blink.times = {0.0f, 1.5f};
    blink.values = {1, 1, 1, 1, 0.1f, 1};
    assets::AnimationChannelData prop; // not a bone: skipped
    prop.node = 6;
    prop.times = {0.0f, 3.0f};
    prop.values = {0, 0, 0, 1, 1, 1};
    anim.channels = {move, turn, blink, prop};

    const AnimationClip clip = BuildClip(anim, s, nodes);
    AETHER_CHECK(clip.name == "Walk" && clip.duration == 2.0f && clip.tracks.size() == 4 && clip.KeyCount() == 3 + 2 + 2);
    const usize hips = static_cast<usize>(s.Find("hips")), spine = static_cast<usize>(s.Find("spine")),
                head = static_cast<usize>(s.Find("head")), leg = static_cast<usize>(s.Find("leg"));
    Pose pose;
    SampleClip(clip, s, 0.5f, false, pose);
    AETHER_CHECK(Near(pose.local[hips].translation, Vec3(1, 1, 0)));
    AETHER_CHECK(AngleBetween(pose.local[spine].rotation, Nlerp(Quaternion::Identity(), q90, 0.25f)) < 1e-4f); // a quarter of the way
    AETHER_CHECK(std::fabs(AngleBetween(pose.local[spine].rotation, Quaternion::Identity()) - kPi / 8) < 0.03f);
    AETHER_CHECK(Near(pose.local[spine].translation, s.bones[spine].rest.translation)); // untouched part: the rest pose
    AETHER_CHECK(Near(pose.local[leg].translation, s.bones[leg].rest.translation));
    SampleClip(clip, s, 1.49f, false, pose);
    AETHER_CHECK(pose.local[head].scale.y == 1.0f); // held until its next key
    SampleClip(clip, s, 1.5f, false, pose);
    AETHER_CHECK(pose.local[head].scale.y == 0.1f);
    SampleClip(clip, s, 1.75f, false, pose);
    AETHER_CHECK(Near(pose.local[hips].translation, Vec3(2, 2.5f, 0)));
    // Clamped, or wrapped when looping.
    SampleClip(clip, s, 9.0f, false, pose);
    AETHER_CHECK(Near(pose.local[hips].translation, Vec3(2, 3, 0)));
    SampleClip(clip, s, 2.5f, true, pose);
    AETHER_CHECK(Near(pose.local[hips].translation, Vec3(1, 1, 0)));
    AETHER_CHECK(std::fabs(ClipTime(clip, -0.5f, true) - 1.5f) < 1e-5f && ClipTime(clip, -0.5f, false) == 0.0f);
    AETHER_CHECK(ClipTime(AnimationClip{}, 3.0f, true) == 0.0f);
}

AETHER_TEST(Animation_KeyReductionStaysWithinTolerance) {
    AnimationClip clip;
    clip.duration = 4.0f;
    clip.tracks.resize(4);
    for (int k = 0; k <= 240; ++k) {
        const f32 t = k / 60.0f;
        clip.tracks[0].translation.times.push_back(t); // a straight line: 2 keys
        clip.tracks[0].translation.values.push_back(Vec3(t, 2 * t, 0));
        clip.tracks[1].translation.times.push_back(t); // a sine wave
        clip.tracks[1].translation.values.push_back(Vec3(std::sin(t * 3.0f), 0, 0));
        clip.tracks[1].rotation.times.push_back(t); // a wobble
        clip.tracks[1].rotation.values.push_back(Quaternion::FromAxisAngle(Vec3(0, 1, 0), 0.5f * std::sin(t * 2.0f)));
        clip.tracks[2].scale.times.push_back(t); // constant: 1 key
        clip.tracks[2].scale.values.push_back(Vec3(1, 1, 1));
    }
    clip.tracks[3].translation.step = true; // held keys: equal neighbours merge
    for (int k = 0; k < 8; ++k) {
        clip.tracks[3].translation.times.push_back(static_cast<f32>(k) * 0.5f);
        clip.tracks[3].translation.values.push_back(Vec3(k < 4 ? 0.0f : 1.0f, 0, 0));
    }
    const AnimationClip original = clip;
    ReductionSettings settings;
    settings.translation = 1e-3f;
    settings.rotation = 1e-3f;
    const usize removed = ReduceKeys(clip, settings);
    AETHER_CHECK(removed == original.KeyCount() - clip.KeyCount() && clip.KeyCount() < original.KeyCount() / 4);
    AETHER_CHECK(clip.tracks[0].translation.times.size() == 2 && clip.tracks[2].scale.times.size() == 1);
    AETHER_CHECK(clip.tracks[3].translation.times.size() == 2 && clip.tracks[3].translation.times[1] == 2.0f);
    // Sampled anywhere, the reduced clip is within tolerance of the original.
    f32 worst_t = 0.0f, worst_r = 0.0f;
    for (int k = 0; k <= 1000; ++k) {
        const f32 t = 4.0f * static_cast<f32>(k) / 1000.0f;
        for (usize b = 0; b < 4; ++b) {
            worst_t = std::max(worst_t, (SampleTrack(clip.tracks[b].translation, t, Vec3()) - SampleTrack(original.tracks[b].translation, t, Vec3())).Length());
            worst_r = std::max(worst_r, AngleBetween(SampleTrack(clip.tracks[b].rotation, t, Quaternion()),
                                                     SampleTrack(original.tracks[b].rotation, t, Quaternion())));
        }
    }
    AETHER_CHECK(worst_t <= 1e-3f * 1.01f && worst_r <= 1e-3f * 1.05f);
    // Two keys that differ can't go further (reduce the source clip once; a
    // second pass would measure against the reduced keys and add error).
    AnimationClip pair;
    pair.tracks.resize(1);
    pair.tracks[0].translation.times = {0.0f, 1.0f};
    pair.tracks[0].translation.values = {Vec3(0, 0, 0), Vec3(1, 0, 0)};
    AETHER_CHECK(ReduceKeys(pair, settings) == 0 && pair.KeyCount() == 2);
}

AETHER_TEST(Animation_CompressionRoundTrips) {
    AnimationClip clip;
    clip.name = "Run_Fast";
    clip.duration = 3.0f;
    clip.tracks.resize(6);
    for (usize b = 0; b < 6; ++b) {
        for (int k = 0; k <= 90; ++k) {
            const f32 t = k / 30.0f, phase = static_cast<f32>(b);
            BoneTrack& tr = clip.tracks[b];
            tr.translation.times.push_back(t);
            tr.translation.values.push_back(Vec3(std::sin(t + phase) * 2.0f, std::cos(t * 2.0f) * 0.5f, t));
            tr.rotation.times.push_back(t);
            tr.rotation.values.push_back(Quaternion::FromAxisAngle(Vec3(std::sin(phase), 1, 0.3f), std::sin(t * 3.0f + phase) * 2.5f));
            if (b == 2) {
                tr.scale.times.push_back(t);
                tr.scale.values.push_back(Vec3(1, 1 + 0.2f * std::sin(t), 1));
            }
        }
    }
    clip.tracks[4] = {}; // a bone with no animation
    clip.tracks[5].rotation.step = true;

    const std::vector<u8> bytes = CompressClip(clip);
    AETHER_CHECK(bytes.size() * 2 < RawClipSize(clip)); // well over 2x smaller
    AnimationClip back;
    std::string error;
    AETHER_CHECK(DecompressClip(bytes, back, &error));
    AETHER_CHECK(back.name == "Run_Fast" && back.duration == 3.0f && back.tracks.size() == 6 && back.KeyCount() == clip.KeyCount());
    AETHER_CHECK(back.tracks[4].translation.Empty() && back.tracks[5].rotation.step && !back.tracks[0].rotation.step);
    f32 worst_t = 0.0f, worst_r = 0.0f, worst_s = 0.0f, worst_time = 0.0f;
    for (usize b = 0; b < 6; ++b) {
        const BoneTrack& a = clip.tracks[b];
        const BoneTrack& c = back.tracks[b];
        for (usize k = 0; k < a.translation.times.size(); ++k) {
            worst_time = std::max(worst_time, std::fabs(a.translation.times[k] - c.translation.times[k]));
            worst_t = std::max(worst_t, (a.translation.values[k] - c.translation.values[k]).Length());
            worst_r = std::max(worst_r, AngleBetween(a.rotation.values[k], c.rotation.values[k]));
        }
        for (usize k = 0; k < a.scale.times.size(); ++k) worst_s = std::max(worst_s, (a.scale.values[k] - c.scale.values[k]).Length());
    }
    AETHER_CHECK(worst_time < 1e-4f && worst_t < 1e-4f && worst_r < 2e-4f && worst_s < 1e-5f);
    AETHER_CHECK(CompressClip(back).size() == bytes.size());

    // Every truncation fails cleanly; so do a bad magic, a bad index and trailing bytes.
    for (usize n = 0; n < bytes.size(); ++n) {
        const std::vector<u8> cut(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(n));
        AETHER_CHECK(!DecompressClip(cut, back, &error));
    }
    std::vector<u8> bad = bytes;
    bad[0] ^= 0xFF;
    AETHER_CHECK(!DecompressClip(bad, back, &error) && error.find("not a") != std::string::npos);
    bad = bytes;
    bad.push_back(0);
    AETHER_CHECK(!DecompressClip(bad, back, &error));
    AnimationClip one;
    one.tracks.resize(1);
    one.tracks[0].rotation.times = {0.0f};
    one.tracks[0].rotation.values = {Quaternion::Identity()};
    std::vector<u8> index = CompressClip(one);
    index[index.size() - 7] = 9; // the dropped component's index
    AETHER_CHECK(!DecompressClip(index, back, &error));
    AETHER_CHECK(DecompressClip(CompressClip(AnimationClip{}), back) && back.tracks.empty());
}

AETHER_TEST(Animation_BlendingMasksAndAdditive) {
    const Skeleton s = MakeSkeleton();
    const Pose rest = RestPose(s);
    Pose moved = rest;
    for (BoneTransform& t : moved.local) {
        t.translation = t.translation + Vec3(1, 0, 0);
        t.rotation = Quaternion::FromAxisAngle(Vec3(0, 1, 0), 1.0f) * t.rotation;
        t.scale = Vec3(2, 2, 2);
    }
    Pose out;
    BlendPoses(rest, moved, 0.0f, out);
    AETHER_CHECK(Near(out.local[0].translation, rest.local[0].translation));
    BlendPoses(rest, moved, 1.0f, out);
    AETHER_CHECK(Near(out.local[0].translation, moved.local[0].translation) && AngleBetween(out.local[0].rotation, moved.local[0].rotation) < 1e-4f);
    BlendPoses(rest, moved, 0.5f, out);
    AETHER_CHECK(Near(out.local[0].translation, rest.local[0].translation + Vec3(0.5f, 0, 0)) && Near(out.local[0].scale, Vec3(1.5f, 1.5f, 1.5f)));
    AETHER_CHECK(std::fabs(AngleBetween(out.local[0].rotation, rest.local[0].rotation) - 0.5f) < 1e-3f);

    // Upper body from `moved`, the rest from `rest`.
    const BoneMask upper = MakeBoneMask(s, "spine");
    const usize spine = static_cast<usize>(s.Find("spine")), head = static_cast<usize>(s.Find("head")),
                leg = static_cast<usize>(s.Find("leg")), hips = static_cast<usize>(s.Find("hips"));
    AETHER_CHECK(upper.weights[spine] == 1.0f && upper.weights[head] == 1.0f && upper.weights[leg] == 0.0f && upper.weights[hips] == 0.0f);
    BlendPoses(rest, moved, 1.0f, out, &upper);
    AETHER_CHECK(Near(out.local[head].translation, moved.local[head].translation) && Near(out.local[leg].translation, rest.local[leg].translation));
    const BoneMask soft = MakeBoneMask(s, "spine", 2);
    AETHER_CHECK(soft.weights[spine] == 0.5f && soft.weights[head] == 1.0f && soft.weights[leg] == 0.0f);
    AETHER_CHECK(MakeBoneMask(s, "nope").weights == std::vector<f32>(4, 0.0f));

    // Additive: the difference from a reference, applied back, gives the pose again.
    const Pose delta = MakeAdditive(moved, rest);
    Pose rebuilt = rest;
    ApplyAdditive(rebuilt, delta, 1.0f);
    for (usize i = 0; i < s.Size(); ++i) {
        AETHER_CHECK(Near(rebuilt.local[i].translation, moved.local[i].translation) && Near(rebuilt.local[i].scale, moved.local[i].scale));
        AETHER_CHECK(AngleBetween(rebuilt.local[i].rotation, moved.local[i].rotation) < 1e-3f);
    }
    Pose half = rest;
    ApplyAdditive(half, delta, 0.5f, &upper);
    AETHER_CHECK(Near(half.local[head].translation, rest.local[head].translation + Vec3(0.5f, 0, 0)) &&
                 Near(half.local[leg].translation, rest.local[leg].translation));
    Pose none = rest;
    ApplyAdditive(none, delta, 0.0f);
    AETHER_CHECK(Near(none.local[head].scale, Vec3(1, 1, 1)));
}
