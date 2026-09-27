#include "aether/animation/root_motion.h"

#include <algorithm>
#include <cmath>

namespace aether::anim {

RootMotionDelta& RootMotionDelta::operator+=(const RootMotionDelta& later) {
    translation = translation + Rotate(rotation, later.translation);
    rotation = (rotation * later.rotation).Normalized();
    return *this;
}

Quaternion YawOf(const Quaternion& q) {
    // The heading of the rotated forward axis (+Z), or of +X when forward
    // points straight up or down.
    Vec3 f = Rotate(q, Vec3(0, 0, 1));
    f32 heading = 0.0f;
    if (f.x * f.x + f.z * f.z > 1e-8f) {
        heading = std::atan2(f.x, f.z);
    } else {
        f = Rotate(q, Vec3(1, 0, 0));
        heading = std::atan2(f.x, f.z) - kPi / 2;
    }
    return Quaternion::FromAxisAngle(Vec3(0, 1, 0), heading);
}

namespace {

struct RootSample {
    Vec3 translation;
    Quaternion yaw;
};

RootSample SampleRoot(const AnimationClip& clip, const Skeleton& skeleton, f32 time, const RootMotionSettings& s) {
    const usize b = static_cast<usize>(s.bone);
    if (s.bone < 0 || b >= skeleton.bones.size()) return {Vec3(), Quaternion::Identity()};
    const BoneTransform& rest = skeleton.bones[b].rest;
    if (b >= clip.tracks.size()) return {rest.translation, YawOf(rest.rotation)};
    const BoneTrack& t = clip.tracks[b];
    return {SampleTrack(t.translation, time, rest.translation), YawOf(SampleTrack(t.rotation, time, rest.rotation))};
}

RootMotionDelta Segment(const AnimationClip& clip, const Skeleton& skeleton, f32 a, f32 b, const RootMotionSettings& s) {
    const RootSample from = SampleRoot(clip, skeleton, a, s), to = SampleRoot(clip, skeleton, b, s);
    Vec3 moved = to.translation - from.translation;
    if (!s.translation_xz) moved.x = moved.z = 0.0f;
    if (!s.translation_y) moved.y = 0.0f;
    RootMotionDelta d;
    d.translation = Rotate(Conjugate(from.yaw), moved);
    if (s.yaw) d.rotation = (Conjugate(from.yaw) * to.yaw).Normalized();
    return d;
}

} // namespace

RootMotionDelta ExtractRootMotion(const AnimationClip& clip, const Skeleton& skeleton, f32 from, f32 to, bool loop,
                                  const RootMotionSettings& settings) {
    if (clip.duration <= 0.0f) return {};
    if (!loop || to >= from) {
        const f32 a = std::clamp(from, 0.0f, clip.duration), b = std::clamp(to, 0.0f, clip.duration);
        return Segment(clip, skeleton, a, b, settings);
    }
    // Wrapped: to the end, then from the start.
    RootMotionDelta d = Segment(clip, skeleton, std::clamp(from, 0.0f, clip.duration), clip.duration, settings);
    d += Segment(clip, skeleton, 0.0f, std::clamp(to, 0.0f, clip.duration), settings);
    return d;
}

void StripRootMotion(Pose& pose, const AnimationClip& clip, const Skeleton& skeleton, const RootMotionSettings& s) {
    const usize b = static_cast<usize>(s.bone);
    if (s.bone < 0 || b >= pose.local.size()) return;
    const RootSample start = SampleRoot(clip, skeleton, 0.0f, s);
    BoneTransform& root = pose.local[b];
    if (s.translation_xz) {
        root.translation.x = start.translation.x;
        root.translation.z = start.translation.z;
    }
    if (s.translation_y) root.translation.y = start.translation.y;
    if (s.yaw) root.rotation = (start.yaw * Conjugate(YawOf(root.rotation)) * root.rotation).Normalized();
}

} // namespace aether::anim
