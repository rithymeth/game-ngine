#include "aether/animation/ik.h"

#include <algorithm>
#include <cmath>

namespace aether::anim {

namespace {

constexpr f32 kEpsilon = 1e-6f;

f32 Angle(const Vec3& a, const Vec3& b) {
    const f32 la = a.Length(), lb = b.Length();
    if (la < kEpsilon || lb < kEpsilon) return 0.0f;
    return std::acos(std::clamp(a.Dot(b) / (la * lb), -1.0f, 1.0f));
}

// Sets a bone's model-space rotation (by changing its local one) and
// recomputes the model transforms.
void SetModelRotation(const Skeleton& s, Pose& pose, std::vector<ModelTransform>& model, i32 bone, const Quaternion& rotation) {
    const i32 parent = s.bones[static_cast<usize>(bone)].parent;
    const Quaternion parent_rotation = parent >= 0 ? model[static_cast<usize>(parent)].rotation : Quaternion::Identity();
    pose.local[static_cast<usize>(bone)].rotation = (Conjugate(parent_rotation) * rotation).Normalized();
    ComputeModelTransforms(s, pose, model);
}

// Turns a bone about its own position by a model-space rotation.
void RotateBone(const Skeleton& s, Pose& pose, std::vector<ModelTransform>& model, i32 bone, const Quaternion& delta) {
    SetModelRotation(s, pose, model, bone, delta * model[static_cast<usize>(bone)].rotation);
}

bool ValidBone(const Skeleton& s, const Pose& pose, i32 b) {
    return b >= 0 && static_cast<usize>(b) < s.bones.size() && static_cast<usize>(b) < pose.local.size();
}

Quaternion LimitAngle(const Quaternion& q, f32 max_angle) {
    const f32 angle = AngleBetween(Quaternion::Identity(), q);
    return angle > max_angle && angle > 0.0f ? Slerp(Quaternion::Identity(), q, max_angle / angle) : q;
}

} // namespace

void ComputeModelTransforms(const Skeleton& s, const Pose& pose, std::vector<ModelTransform>& out) {
    out.resize(s.bones.size());
    for (usize i = 0; i < s.bones.size(); ++i) {
        const BoneTransform& l = i < pose.local.size() ? pose.local[i] : s.bones[i].rest;
        const i32 parent = s.bones[i].parent;
        if (parent < 0) {
            out[i] = {l.translation, l.rotation, l.scale};
            continue;
        }
        const ModelTransform& p = out[static_cast<usize>(parent)];
        const Vec3 scaled(l.translation.x * p.scale.x, l.translation.y * p.scale.y, l.translation.z * p.scale.z);
        out[i] = {p.position + Rotate(p.rotation, scaled), (p.rotation * l.rotation).Normalized(),
                  Vec3(p.scale.x * l.scale.x, p.scale.y * l.scale.y, p.scale.z * l.scale.z)};
    }
}

Quaternion FromTo(const Vec3& from, const Vec3& to) {
    const Vec3 a = from.Normalized(), b = to.Normalized();
    if (a.LengthSq() < kEpsilon || b.LengthSq() < kEpsilon) return Quaternion::Identity();
    const f32 d = a.Dot(b);
    if (d < -0.999999f) {
        // Opposite: half a turn about any axis perpendicular to `from`.
        Vec3 axis = Vec3(1, 0, 0).Cross(a);
        if (axis.LengthSq() < 1e-6f) axis = Vec3(0, 1, 0).Cross(a);
        return Quaternion::FromAxisAngle(axis.Normalized(), kPi);
    }
    const Vec3 c = a.Cross(b);
    return Quaternion(c.x, c.y, c.z, 1.0f + d).Normalized();
}

// --- Two-bone IK --------------------------------------------------------------------------

bool SolveTwoBoneIK(const Skeleton& s, Pose& pose, const TwoBoneIK& ik) {
    if (!ValidBone(s, pose, ik.root) || !ValidBone(s, pose, ik.mid) || !ValidBone(s, pose, ik.end)) return false;
    if (s.bones[static_cast<usize>(ik.end)].parent != ik.mid || s.bones[static_cast<usize>(ik.mid)].parent != ik.root) return false;
    if (ik.weight <= 0.0f) return true;
    const Pose original = pose;
    std::vector<ModelTransform> m;
    ComputeModelTransforms(s, pose, m);
    auto pos = [&](i32 b) { return m[static_cast<usize>(b)].position; };
    const Vec3 a = pos(ik.root), b = pos(ik.mid), c = pos(ik.end);
    const f32 l1 = (b - a).Length(), l2 = (c - b).Length();
    if (l1 < kEpsilon || l2 < kEpsilon) return false;
    const f32 slack = 1e-4f * (l1 + l2);
    const f32 d = std::clamp((ik.target - a).Length(), std::fabs(l1 - l2) + slack, l1 + l2 - slack);

    // 1. Bend the middle joint so the limb spans `d`.
    // The bend axis must be perpendicular to the limb. A straight limb has
    // no bend plane of its own: take the plane through it and the pole
    // (or the target).
    Vec3 axis = (b - a).Cross(c - b);
    if (axis.LengthSq() < 1e-10f && ik.use_pole) axis = (b - a).Cross(ik.pole - a);
    if (axis.LengthSq() < 1e-10f) axis = (b - a).Cross(ik.target - a);
    if (axis.LengthSq() < 1e-10f) axis = (b - a).Cross(Vec3(0, 1, 0));
    if (axis.LengthSq() < 1e-10f) axis = (b - a).Cross(Vec3(1, 0, 0));
    axis = axis.Normalized();
    const f32 current = Angle(a - b, c - b);
    const f32 desired = std::acos(std::clamp((l1 * l1 + l2 * l2 - d * d) / (2.0f * l1 * l2), -1.0f, 1.0f));
    // The sign that opens (or closes) the joint depends on the winding: try one, keep the one that works.
    Quaternion bend = Quaternion::FromAxisAngle(axis, desired - current);
    if (std::fabs(Angle(a - b, Rotate(bend, c - b)) - desired) > 1e-3f) bend = Quaternion::FromAxisAngle(axis, current - desired);
    RotateBone(s, pose, m, ik.mid, bend);

    // 2. Swing the root so the end points at the target.
    RotateBone(s, pose, m, ik.root, FromTo(pos(ik.end) - a, ik.target - a));

    // 3. Twist about the root-to-target line so the middle joint faces the pole.
    if (ik.use_pole) {
        const Vec3 u = (ik.target - a).Normalized();
        Vec3 vb = pos(ik.mid) - a, vp = ik.pole - a;
        vb = vb - u * vb.Dot(u);
        vp = vp - u * vp.Dot(u);
        if (vb.LengthSq() > 1e-10f && vp.LengthSq() > 1e-10f && u.LengthSq() > 0.5f) {
            const Vec3 nb = vb.Normalized(), np = vp.Normalized();
            const Quaternion twist = nb.Dot(np) < -0.9999f ? Quaternion::FromAxisAngle(u, kPi) : FromTo(nb, np);
            RotateBone(s, pose, m, ik.root, twist);
        }
    }
    if (ik.weight < 1.0f) BlendPoses(original, pose, ik.weight, pose);
    return true;
}

// --- Look-at -------------------------------------------------------------------------------------

bool SolveLookAt(const Skeleton& s, Pose& pose, const LookAt& look) {
    if (!ValidBone(s, pose, look.bone)) return false;
    std::vector<ModelTransform> m;
    ComputeModelTransforms(s, pose, m);
    const ModelTransform& bone = m[static_cast<usize>(look.bone)];
    const Vec3 aim = Rotate(bone.rotation, look.aim_axis);
    Quaternion delta = LimitAngle(FromTo(aim, look.target - bone.position), std::max(look.max_angle, 0.0f));
    delta = Slerp(Quaternion::Identity(), delta, std::clamp(look.weight, 0.0f, 1.0f));
    RotateBone(s, pose, m, look.bone, delta);
    return true;
}

// --- FABRIK --------------------------------------------------------------------------------------

f32 SolveFabrik(const Skeleton& s, Pose& pose, const FabrikChain& chain) {
    const usize n = chain.bones.size();
    if (n < 2) return -1.0f;
    for (usize i = 0; i < n; ++i) {
        if (!ValidBone(s, pose, chain.bones[i])) return -1.0f;
        if (i > 0 && s.bones[static_cast<usize>(chain.bones[i])].parent != chain.bones[i - 1]) return -1.0f;
    }
    const Pose original = pose;
    std::vector<ModelTransform> m;
    ComputeModelTransforms(s, pose, m);
    std::vector<Vec3> p(n);
    for (usize i = 0; i < n; ++i) p[i] = m[static_cast<usize>(chain.bones[i])].position;
    std::vector<f32> len(n - 1);
    f32 total = 0.0f;
    for (usize i = 0; i + 1 < n; ++i) total += len[i] = (p[i + 1] - p[i]).Length();
    const Vec3 root = p[0];
    if ((chain.target - root).Length() >= total) {
        // Out of reach: straight at it.
        const Vec3 dir = (chain.target - root).Normalized();
        for (usize i = 0; i + 1 < n; ++i) p[i + 1] = p[i] + dir * len[i];
    } else {
        for (u32 it = 0; it < chain.iterations && (p[n - 1] - chain.target).Length() > chain.tolerance; ++it) {
            p[n - 1] = chain.target; // backward: from the tip
            for (usize i = n - 1; i-- > 0;) p[i] = p[i + 1] + (p[i] - p[i + 1]).Normalized() * len[i];
            p[0] = root; // forward: from the root
            for (usize i = 0; i + 1 < n; ++i) p[i + 1] = p[i] + (p[i + 1] - p[i]).Normalized() * len[i];
        }
    }
    // Rotate each bone so its child lands on the solved position.
    for (usize i = 0; i + 1 < n; ++i) {
        const Vec3 at = m[static_cast<usize>(chain.bones[i])].position;
        RotateBone(s, pose, m, chain.bones[i], FromTo(m[static_cast<usize>(chain.bones[i + 1])].position - at, p[i + 1] - at));
    }
    if (chain.weight < 1.0f) {
        BlendPoses(original, pose, std::max(chain.weight, 0.0f), pose);
        ComputeModelTransforms(s, pose, m);
    }
    return (m[static_cast<usize>(chain.bones[n - 1])].position - chain.target).Length();
}

// --- Foot placement ----------------------------------------------------------------------------

void FootPlacer::Apply(const Skeleton& s, Pose& pose, const GroundTrace& trace, f32 dt) {
    const FootPlacementSettings& st = settings_;
    if (!ValidBone(s, pose, st.pelvis) || !trace) return;
    std::vector<ModelTransform> m;
    ComputeModelTransforms(s, pose, m);
    const Vec3 up(0, 1, 0);
    // The ground under each foot, relative to the animation's ground (model y = 0).
    std::vector<f32> targets(st.legs.size(), 0.0f);
    std::vector<Vec3> normals(st.legs.size(), up);
    std::vector<Vec3> ankles(st.legs.size());
    for (usize i = 0; i < st.legs.size(); ++i) {
        const LegSetup& leg = st.legs[i];
        if (!ValidBone(s, pose, leg.ankle)) continue;
        ankles[i] = m[static_cast<usize>(leg.ankle)].position;
        const Vec3 from(ankles[i].x, st.max_step_up + leg.foot_height + std::max(ankles[i].y, 0.0f), ankles[i].z);
        const Vec3 to(ankles[i].x, -st.max_step_down - leg.foot_height, ankles[i].z);
        Vec3 hit, normal;
        if (trace(from, to, hit, normal)) {
            targets[i] = std::clamp(hit.y, -st.max_step_down, st.max_step_up);
            normals[i] = normal.LengthSq() > 0 ? normal.Normalized() : up;
        }
    }
    f32 pelvis_target = 0.0f;
    for (f32 t : targets) pelvis_target = std::min(pelvis_target, t); // drop so the lowest foot reaches

    // Smooth toward the targets (the first frame snaps).
    const f32 alpha = !started_ || st.smoothing <= 0.0f ? 1.0f : 1.0f - std::exp(-dt / st.smoothing);
    foot_offsets_.resize(targets.size(), 0.0f);
    pelvis_offset_ += (pelvis_target - pelvis_offset_) * alpha;
    for (usize i = 0; i < targets.size(); ++i) foot_offsets_[i] += (targets[i] - foot_offsets_[i]) * alpha;
    started_ = true;

    // Move the pelvis down in model space.
    const i32 parent = s.bones[static_cast<usize>(st.pelvis)].parent;
    const Quaternion parent_rotation = parent >= 0 ? m[static_cast<usize>(parent)].rotation : Quaternion::Identity();
    BoneTransform& pelvis = pose.local[static_cast<usize>(st.pelvis)];
    pelvis.translation = pelvis.translation + Rotate(Conjugate(parent_rotation), up * pelvis_offset_);
    ComputeModelTransforms(s, pose, m);

    for (usize i = 0; i < st.legs.size(); ++i) {
        const LegSetup& leg = st.legs[i];
        if (!ValidBone(s, pose, leg.hip) || !ValidBone(s, pose, leg.knee) || !ValidBone(s, pose, leg.ankle)) continue;
        // Keep the knee's bend plane: the pole is the knee pushed away from the hip-ankle line.
        const Vec3 hip = m[static_cast<usize>(leg.hip)].position, knee = m[static_cast<usize>(leg.knee)].position;
        const Vec3 ankle = m[static_cast<usize>(leg.ankle)].position;
        const Vec3 mid = (hip + ankle) * 0.5f;
        TwoBoneIK ik;
        ik.root = leg.hip;
        ik.mid = leg.knee;
        ik.end = leg.ankle;
        ik.target = ankles[i] + up * foot_offsets_[i];
        ik.pole = knee + (knee - mid) * 2.0f;
        ik.use_pole = (knee - mid).LengthSq() > 1e-8f;
        SolveTwoBoneIK(s, pose, ik);
        ComputeModelTransforms(s, pose, m);
        // Tilt the foot to the ground.
        const Quaternion tilt = LimitAngle(FromTo(up, normals[i]), st.max_foot_angle);
        RotateBone(s, pose, m, leg.ankle, tilt);
    }
}

} // namespace aether::anim
