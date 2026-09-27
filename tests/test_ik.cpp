#include "aether/animation/ik.h"
#include "aether/animation/retarget.h"
#include "test_framework.h"

#include <cmath>

using namespace aether;
using namespace aether::anim;

// Phase 16 step 5: two-bone IK, look-at, FABRIK, foot placement and retargeting.

namespace {

bool Near(const Vec3& a, const Vec3& b, f32 eps = 1e-3f) { return (a - b).Length() < eps; }

Bone B(const std::string& name, i32 parent, const Vec3& t, const Quaternion& r = Quaternion::Identity()) {
    return {name, parent, {t, r, Vec3(1, 1, 1)}, Mat4::Identity()};
}

// shoulder (origin) -> elbow (+1 X) -> hand (+1 X): a straight arm.
Skeleton Arm() {
    Skeleton s;
    s.bones = {B("shoulder", -1, Vec3(0, 0, 0)), B("elbow", 0, Vec3(1, 0, 0)), B("hand", 1, Vec3(1, 0, 0))};
    return s;
}

// pelvis at 1 m; each leg: hip, knee (bent slightly forward), ankle 0.1 m above the ground.
Skeleton Legs() {
    Skeleton s;
    s.bones = {B("pelvis", -1, Vec3(0, 1, 0)),
               B("hip_l", 0, Vec3(-0.2f, 0, 0)), B("knee_l", 1, Vec3(0, -0.45f, 0.05f)), B("ankle_l", 2, Vec3(0, -0.45f, -0.05f)),
               B("hip_r", 0, Vec3(0.2f, 0, 0)),  B("knee_r", 4, Vec3(0, -0.45f, 0.05f)), B("ankle_r", 5, Vec3(0, -0.45f, -0.05f))};
    return s;
}

Vec3 Position(const Skeleton& s, const Pose& p, const char* bone) {
    std::vector<ModelTransform> m;
    ComputeModelTransforms(s, p, m);
    return m[static_cast<usize>(s.Find(bone))].position;
}

} // namespace

AETHER_TEST(IK_TwoBone) {
    const Skeleton s = Arm();
    auto solve = [&](const Vec3& target, const Vec3& pole, f32 weight = 1.0f) {
        Pose p = RestPose(s);
        TwoBoneIK ik{0, 1, 2, target, pole, true, weight};
        AETHER_CHECK(SolveTwoBoneIK(s, p, ik));
        return p;
    };
    // Reachable: the hand on the target, the bones keep their lengths, the elbow bends toward the pole.
    for (const Vec3& pole : {Vec3(0, 0, 1), Vec3(0, 0, -1)}) {
        const Pose p = solve(Vec3(1, 1, 0), pole);
        const Vec3 elbow = Position(s, p, "elbow"), hand = Position(s, p, "hand");
        AETHER_CHECK(Near(hand, Vec3(1, 1, 0)) && std::fabs(elbow.Length() - 1) < 1e-3f && std::fabs((hand - elbow).Length() - 1) < 1e-3f);
        AETHER_CHECK(elbow.z * pole.z > 0.5f);
    }
    // From a bent start, too.
    Pose bent = RestPose(s);
    bent.local[1].rotation = Quaternion::FromAxisAngle(Vec3(0, 1, 0), 1.2f);
    TwoBoneIK again{0, 1, 2, Vec3(0.5f, -1.2f, 0.4f), Vec3(1, 0, 1), true, 1.0f};
    AETHER_CHECK(SolveTwoBoneIK(s, bent, again) && Near(Position(s, bent, "hand"), again.target));
    // Out of reach: straight toward it.
    const Pose far = solve(Vec3(5, 0, 5), Vec3(0, 1, 0));
    const Vec3 dir = Vec3(5, 0, 5).Normalized();
    AETHER_CHECK(Near(Position(s, far, "hand"), dir * 2.0f, 1e-2f));
    // Too close: as bent as it goes, still aimed at it.
    const Pose close = solve(Vec3(0.001f, 0.0f, 0.0f), Vec3(0, 1, 0));
    AETHER_CHECK(Position(s, close, "hand").Length() < 0.01f);
    // Half weight: part of the way.
    const f32 half = (Position(s, solve(Vec3(1, 1, 0), Vec3(0, 0, 1), 0.5f), "hand") - Vec3(1, 1, 0)).Length();
    AETHER_CHECK(half > 0.05f && half < (Vec3(2, 0, 0) - Vec3(1, 1, 0)).Length());
    // Not a chain.
    Pose p = RestPose(s);
    TwoBoneIK wrong{2, 1, 0, Vec3(1, 1, 0), Vec3(), true, 1.0f};
    AETHER_CHECK(!SolveTwoBoneIK(s, p, wrong));
    AETHER_CHECK(Near(Rotate(FromTo(Vec3(1, 0, 0), Vec3(-1, 0, 0)), Vec3(1, 0, 0)), Vec3(-1, 0, 0)));
}

AETHER_TEST(IK_LookAtAndFabrik) {
    Skeleton head;
    head.bones = {B("neck", -1, Vec3(0, 1.5f, 0)), B("head", 0, Vec3(0, 0.1f, 0))};
    Pose p = RestPose(head);
    LookAt look;
    look.bone = 1;
    look.target = Vec3(3, 1.6f, 0);
    AETHER_CHECK(SolveLookAt(head, p, look));
    std::vector<ModelTransform> m;
    ComputeModelTransforms(head, p, m);
    AETHER_CHECK(Near(Rotate(m[1].rotation, Vec3(0, 0, 1)), Vec3(1, 0, 0)));
    // Limited to 0.5 rad from where the animation had it.
    p = RestPose(head);
    look.max_angle = 0.5f;
    SolveLookAt(head, p, look);
    ComputeModelTransforms(head, p, m);
    AETHER_CHECK(std::fabs(AngleBetween(m[1].rotation, Quaternion::Identity()) - 0.5f) < 1e-3f);

    // A five-bone tail, 0.5 m per bone, straight up.
    Skeleton tail;
    tail.bones.push_back(B("t0", -1, Vec3(0, 0, 0)));
    for (int i = 1; i < 5; ++i) tail.bones.push_back(B("t" + std::to_string(i), i - 1, Vec3(0, 0.5f, 0)));
    FabrikChain chain;
    chain.bones = {0, 1, 2, 3, 4};
    chain.target = Vec3(1, 1, 0.5f);
    Pose t = RestPose(tail);
    AETHER_CHECK(SolveFabrik(tail, t, chain) < 1e-3f);
    ComputeModelTransforms(tail, t, m);
    AETHER_CHECK(Near(m[0].position, Vec3(0, 0, 0))); // the root stays put
    for (int i = 1; i < 5; ++i) AETHER_CHECK(std::fabs((m[static_cast<usize>(i)].position - m[static_cast<usize>(i - 1)].position).Length() - 0.5f) < 1e-3f);
    // Out of reach: straight at it, 1 m short.
    t = RestPose(tail);
    chain.target = Vec3(3, 0, 0);
    AETHER_CHECK(std::fabs(SolveFabrik(tail, t, chain) - 1.0f) < 1e-3f);
    ComputeModelTransforms(tail, t, m);
    AETHER_CHECK(Near(m[4].position, Vec3(2, 0, 0)));
    chain.bones = {0, 2};
    AETHER_CHECK(SolveFabrik(tail, t, chain) < 0.0f); // not a chain
}

AETHER_TEST(IK_FootPlacement) {
    const Skeleton s = Legs();
    FootPlacementSettings settings;
    settings.pelvis = 0;
    settings.legs = {{1, 2, 3, 0.1f}, {4, 5, 6, 0.1f}};
    // Ground heights: x < 0 (left foot) and x > 0 (right foot), with an optional slope under the left.
    f32 left = 0.0f, right = 0.0f;
    Vec3 left_normal(0, 1, 0);
    const GroundTrace trace = [&](const Vec3& from, const Vec3& to, Vec3& hit, Vec3& normal) {
        const f32 h = from.x < 0 ? left : right;
        if (h > from.y || h < to.y) return false;
        hit = Vec3(from.x, h, from.z);
        normal = from.x < 0 ? left_normal : Vec3(0, 1, 0);
        return true;
    };
    auto place = [&](FootPlacer& placer, f32 dt = 0.016f) {
        Pose p = RestPose(s);
        placer.Apply(s, p, trace, dt);
        return p;
    };
    FootPlacer flat(settings);
    Pose p = place(flat);
    AETHER_CHECK(Near(Position(s, p, "ankle_l"), Position(s, RestPose(s), "ankle_l")) && flat.PelvisOffset() == 0.0f);
    // A step under the left foot: it rises onto it; the pelvis stays.
    left = 0.3f;
    FootPlacer step(settings);
    p = place(step);
    AETHER_CHECK(std::fabs(Position(s, p, "ankle_l").y - 0.4f) < 1e-3f && std::fabs(Position(s, p, "ankle_r").y - 0.1f) < 1e-3f);
    AETHER_CHECK(std::fabs(Position(s, p, "pelvis").y - 1.0f) < 1e-4f);
    // A dip under the right foot: the pelvis drops so it can reach; the left stays on its ground.
    left = 0.0f;
    right = -0.2f;
    FootPlacer dip(settings);
    p = place(dip);
    AETHER_CHECK(std::fabs(dip.PelvisOffset() + 0.2f) < 1e-4f && std::fabs(Position(s, p, "pelvis").y - 0.8f) < 1e-4f);
    AETHER_CHECK(std::fabs(Position(s, p, "ankle_r").y + 0.1f) < 1e-3f && std::fabs(Position(s, p, "ankle_l").y - 0.1f) < 1e-3f);
    // Ground out of range is left alone (clamped to max_step_down).
    right = -2.0f;
    FootPlacer cliff(settings);
    place(cliff);
    AETHER_CHECK(cliff.PelvisOffset() == 0.0f);
    // A slope tilts the foot to its normal (up to the limit).
    right = 0.0f;
    left_normal = Vec3(0.3f, 1, 0).Normalized();
    FootPlacer slope(settings);
    p = place(slope);
    std::vector<ModelTransform> m;
    ComputeModelTransforms(s, p, m);
    AETHER_CHECK(Near(Rotate(m[3].rotation, Vec3(0, 1, 0)), left_normal, 1e-2f));
    // Smoothing: the first frame snaps, later changes ease in.
    settings.smoothing = 0.1f;
    left_normal = Vec3(0, 1, 0);
    left = 0.0f;
    FootPlacer smooth(settings);
    place(smooth, 0.05f);
    left = 0.3f;
    place(smooth, 0.05f);
    AETHER_CHECK(smooth.FootOffsets()[0] > 0.05f && smooth.FootOffsets()[0] < 0.25f);
    for (int i = 0; i < 40; ++i) place(smooth, 0.05f);
    AETHER_CHECK(std::fabs(smooth.FootOffsets()[0] - 0.3f) < 1e-3f);
}

AETHER_TEST(IK_Retargeting) {
    // Source: 1 m hips, a T-pose arm along +X.
    Skeleton src;
    src.bones = {B("mixamorig:Hips", -1, Vec3(0, 1, 0)), B("mixamorig:Spine", 0, Vec3(0, 0.5f, 0)), B("mixamorig:Arm", 1, Vec3(0.5f, 0, 0)),
                 B("mixamorig:Hand", 2, Vec3(0.5f, 0, 0))};
    // Target: twice as tall, the arm bone's axes turned 90° about X (a different rig), and an extra tail bone.
    Skeleton dst;
    dst.bones = {B("hips", -1, Vec3(0, 2, 0)), B("spine", 0, Vec3(0, 1, 0)),
                 B("arm", 1, Vec3(1, 0, 0), Quaternion::FromAxisAngle(Vec3(1, 0, 0), kPi / 2)), B("hand", 2, Vec3(1, 0, 0)),
                 B("tail", 0, Vec3(0, -0.2f, -0.3f))};
    const BoneMapping map = AutoMapBones(src, dst);
    AETHER_CHECK(map.bones.size() == 4);
    const Retargeter r(src, dst, map);
    AETHER_CHECK(r.MappedCount() == 4 && std::fabs(r.TranslationScale() - 2.0f) < 1e-5f);

    // The source bends its spine 30° about Z, raises the arm 45° and walks 0.5 m forward.
    Pose sp = RestPose(src);
    sp.local[0].translation = Vec3(0, 1, 0.5f);
    sp.local[1].rotation = Quaternion::FromAxisAngle(Vec3(0, 0, 1), 0.5236f);
    sp.local[2].rotation = Quaternion::FromAxisAngle(Vec3(0, 0, 1), 0.7854f);
    Pose tp;
    r.Retarget(sp, tp);
    auto dir = [](const Skeleton& s, const Pose& p, const char* a, const char* b) { return (Position(s, p, b) - Position(s, p, a)).Normalized(); };
    // Every mapped limb points the same way, whatever its rest axes.
    AETHER_CHECK(Near(dir(src, sp, "mixamorig:Spine", "mixamorig:Arm"), dir(dst, tp, "spine", "arm")));
    AETHER_CHECK(Near(dir(src, sp, "mixamorig:Arm", "mixamorig:Hand"), dir(dst, tp, "arm", "hand")));
    // The target keeps its own lengths; the root moves twice as far.
    AETHER_CHECK(std::fabs((Position(dst, tp, "hand") - Position(dst, tp, "arm")).Length() - 1.0f) < 1e-4f);
    AETHER_CHECK(Near(Position(dst, tp, "hips"), Vec3(0, 2, 1)));
    AETHER_CHECK(Near(tp.local[4].translation, dst.bones[4].rest.translation)); // unmapped: rest
    // The rest pose maps to the rest pose.
    r.Retarget(RestPose(src), tp);
    for (usize i = 0; i < dst.Size(); ++i) AETHER_CHECK(AngleBetween(tp.local[i].rotation, dst.bones[i].rest.rotation) < 1e-4f);

    // A clip, resampled.
    AnimationClip clip;
    clip.name = "Wave";
    clip.duration = 1.0f;
    clip.tracks.resize(src.Size());
    clip.tracks[2].rotation.times = {0.0f, 1.0f};
    clip.tracks[2].rotation.values = {Quaternion::Identity(), Quaternion::FromAxisAngle(Vec3(0, 0, 1), 1.2f)};
    clip.notifies = {{"Wave", 0.5f}};
    const AnimationClip out = r.RetargetClip(clip, 10.0f);
    AETHER_CHECK(out.tracks.size() == dst.Size() && out.tracks[2].rotation.times.size() == 11 && out.tracks[4].rotation.Empty());
    AETHER_CHECK(out.tracks[0].translation.times.size() == 11 && out.notifies.size() == 1);
    Pose a, b, c;
    SampleClip(clip, src, 0.3f, false, a);
    r.Retarget(a, b);
    SampleClip(out, dst, 0.3f, false, c);
    AETHER_CHECK(Near(dir(dst, b, "arm", "hand"), dir(dst, c, "arm", "hand")));
    // An explicit mapping and root.
    BoneMapping partial;
    partial.bones = {{"mixamorig:Spine", "spine"}};
    partial.root = "spine";
    const Retargeter r2(src, dst, partial);
    AETHER_CHECK(r2.MappedCount() == 1 && std::fabs(r2.TranslationScale() - 2.0f) < 1e-5f);
}
