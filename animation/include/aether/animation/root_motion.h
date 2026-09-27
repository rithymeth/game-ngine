#pragma once

#include "aether/animation/clip.h"

namespace aether::anim {

// Root motion (Phase 16 step 2, docs/design/PHASE_SPECS.md §16.3): the root
// bone's movement over the ground and turn about the up axis (+Y), taken
// out of the animation and handed to the character, so walking clips
// move the capsule instead of sliding away from it.

struct RootMotionSettings {
    i32 bone = 0;               // the bone that carries the motion
    bool translation_xz = true; // over the ground
    bool translation_y = false; // jumps usually stay in the animation
    bool yaw = true;            // turning about +Y
};

struct RootMotionDelta {
    Vec3 translation{0, 0, 0}; // in the root's frame at the start (forward stays forward)
    Quaternion rotation = Quaternion::Identity(); // a yaw
    RootMotionDelta& operator+=(const RootMotionDelta& later); // this, then `later`
};

// The twist of `q` about +Y (the yaw part) and what's left (q = swing * twist).
Quaternion YawOf(const Quaternion& q);

// The motion from `from` to `to` (clip times). Looping clips that wrap
// past the end add the part up to the end and the part from the start.
RootMotionDelta ExtractRootMotion(const AnimationClip& clip, const Skeleton& skeleton, f32 from, f32 to, bool loop,
                                  const RootMotionSettings& settings = {});
// Removes the extracted parts from a sampled pose: the root keeps its
// start-of-clip position over the ground and loses its yaw relative to the
// start, so the character plays in place.
void StripRootMotion(Pose& pose, const AnimationClip& clip, const Skeleton& skeleton, const RootMotionSettings& settings = {});

} // namespace aether::anim
