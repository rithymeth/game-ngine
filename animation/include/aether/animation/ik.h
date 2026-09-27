#pragma once

#include "aether/animation/skeleton.h"

#include <functional>
#include <vector>

namespace aether::anim {

// Inverse kinematics (Phase 16 step 5, docs/design/PHASE_SPECS.md §16.6):
// solvers that bend a pose so a bone reaches or faces a target. Targets are
// in model space (the skeleton's root space); the solvers change the
// pose's local rotations (and the pelvis translation for foot placement).

// A bone's transform in model space.
struct ModelTransform {
    Vec3 position{0, 0, 0};
    Quaternion rotation = Quaternion::Identity();
    Vec3 scale{1, 1, 1};
};
void ComputeModelTransforms(const Skeleton& skeleton, const Pose& pose, std::vector<ModelTransform>& out);
// The shortest rotation taking direction `from` to direction `to`.
Quaternion FromTo(const Vec3& from, const Vec3& to);

// --- Two-bone IK (arms, legs) ---------------------------------------------------------
struct TwoBoneIK {
    i32 root = -1, mid = -1, end = -1; // shoulder, elbow, hand (a chain: end's parent is mid, mid's is root)
    Vec3 target{0, 0, 0};              // where the end bone should be
    Vec3 pole{0, 0, 0};                // the elbow/knee bends toward this point
    bool use_pole = true;
    f32 weight = 1.0f;                 // 0 leaves the pose alone
};
// False if the bones aren't a chain. A target out of reach straightens the limb toward it.
bool SolveTwoBoneIK(const Skeleton& skeleton, Pose& pose, const TwoBoneIK& ik);

// --- Look-at (heads, eyes, turrets) -----------------------------------------------------
struct LookAt {
    i32 bone = -1;
    Vec3 aim_axis{0, 0, 1}; // the bone's local axis that should point at the target
    Vec3 target{0, 0, 0};
    f32 max_angle = 3.2f;   // radians the bone may turn from the animated pose
    f32 weight = 1.0f;
};
bool SolveLookAt(const Skeleton& skeleton, Pose& pose, const LookAt& look);

// --- FABRIK (spines, tails, tentacles) --------------------------------------------------
struct FabrikChain {
    std::vector<i32> bones; // root first; each bone's parent is the one before it
    Vec3 target{0, 0, 0};
    u32 iterations = 16;
    f32 tolerance = 1e-4f; // metres
    f32 weight = 1.0f;
};
// Returns the distance left between the tip and the target (0 when reached).
f32 SolveFabrik(const Skeleton& skeleton, Pose& pose, const FabrikChain& chain);

// --- Foot placement ------------------------------------------------------------------------
// A ground trace in model space: from `from` down to `to`; the hit point and
// its normal, or false if there's no ground.
using GroundTrace = std::function<bool(const Vec3& from, const Vec3& to, Vec3& hit, Vec3& normal)>;

struct LegSetup {
    i32 hip = -1, knee = -1, ankle = -1;
    f32 foot_height = 0.1f; // the ankle's height above the sole
};
struct FootPlacementSettings {
    i32 pelvis = -1;
    std::vector<LegSetup> legs;
    f32 max_step_up = 0.5f;   // how far above the animated foot the ground may be
    f32 max_step_down = 0.5f; // and below (how far the pelvis may drop)
    f32 max_foot_angle = 0.8f; // radians a foot may tilt to match the ground
    f32 smoothing = 0.0f;      // seconds to settle (0 snaps)
};

// Keeps feet on uneven ground: traces under each foot, lowers the pelvis
// so the lowest foot can reach, bends each leg (two-bone IK) to put its
// foot on the ground and tilts the foot to its normal. The offsets are
// smoothed over time, so it keeps a little state per character.
class FootPlacer {
public:
    explicit FootPlacer(FootPlacementSettings settings) : settings_(std::move(settings)) {}
    void Apply(const Skeleton& skeleton, Pose& pose, const GroundTrace& trace, f32 dt);
    f32 PelvisOffset() const { return pelvis_offset_; }
    const std::vector<f32>& FootOffsets() const { return foot_offsets_; }

private:
    FootPlacementSettings settings_;
    f32 pelvis_offset_ = 0.0f;
    std::vector<f32> foot_offsets_;
    bool started_ = false;
};

} // namespace aether::anim
