#pragma once

#include "aether/animation/skeleton.h"

#include <string>
#include <vector>

namespace aether::anim {

// Animation clips (Phase 16 step 1, docs/design/PHASE_SPECS.md §16.1): one
// track per bone, sampled into a Pose.

struct Vec3Track {
    std::vector<f32> times; // increasing, seconds
    std::vector<Vec3> values;
    bool step = false; // hold each key instead of interpolating
    bool Empty() const { return times.empty(); }
};
struct RotationTrack {
    std::vector<f32> times;
    std::vector<Quaternion> values;
    bool step = false;
    bool Empty() const { return times.empty(); }
};
// An empty sub-track leaves that part of the bone at its rest pose.
struct BoneTrack {
    Vec3Track translation;
    RotationTrack rotation;
    Vec3Track scale;
};

struct AnimationClip {
    std::string name;
    f32 duration = 0.0f; // seconds
    std::vector<BoneTrack> tracks; // one per bone
    usize KeyCount() const;
};

// Maps an imported animation's channels onto a skeleton (`bone_nodes` from
// BuildSkeleton). Channels for nodes that aren't bones are skipped.
AnimationClip BuildClip(const assets::AnimationData& animation, const Skeleton& skeleton, const std::vector<u32>& bone_nodes);

// The clip's time for a playback time: wrapped when looping, clamped otherwise.
f32 ClipTime(const AnimationClip& clip, f32 time, bool loop);
void SampleClip(const AnimationClip& clip, const Skeleton& skeleton, f32 time, bool loop, Pose& out);
Vec3 SampleTrack(const Vec3Track& track, f32 time, const Vec3& fallback);
Quaternion SampleTrack(const RotationTrack& track, f32 time, const Quaternion& fallback);

// Keyframe reduction: drops keys that interpolating their kept neighbours
// reproduces within these tolerances, and collapses constant tracks to one
// key. Returns how many keys went. Run it once on the source clip: a second
// pass measures against the reduced keys, so its error adds up.
struct ReductionSettings {
    f32 translation = 1e-4f; // metres
    f32 rotation = 1e-4f;    // radians
    f32 scale = 1e-4f;       // ratio
};
usize ReduceKeys(AnimationClip& clip, const ReductionSettings& settings = {});

} // namespace aether::anim
