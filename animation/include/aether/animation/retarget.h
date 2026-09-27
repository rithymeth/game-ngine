#pragma once

#include "aether/animation/clip.h"

#include <string>
#include <utility>
#include <vector>

namespace aether::anim {

// Retargeting (Phase 16 step 5, docs/design/PHASE_SPECS.md §16.6): playing
// one skeleton's animation on another. Rotations are carried over in model
// space relative to each skeleton's rest pose, so bones whose rest
// orientations differ (a T-pose and an A-pose, different bone axes) still
// point the same way. Only the root's translation moves, scaled by the
// ratio of the skeletons' root heights; every other bone keeps the target's
// own lengths.

struct BoneMapping {
    std::vector<std::pair<std::string, std::string>> bones; // source name -> target name
    std::string root;                                        // the target bone that carries translation (default: the first root)
};

// Pairs bones by name, ignoring case and a rig prefix ("mixamorig:Hips" matches "hips").
BoneMapping AutoMapBones(const Skeleton& source, const Skeleton& target);

class Retargeter {
public:
    Retargeter(const Skeleton& source, const Skeleton& target, const BoneMapping& mapping);

    // A source pose onto the target (unmapped target bones keep their rest pose).
    void Retarget(const Pose& source_pose, Pose& target_pose) const;
    // A whole clip, resampled at `rate` keys per second.
    AnimationClip RetargetClip(const AnimationClip& clip, f32 rate = 30.0f) const;

    usize MappedCount() const;
    f32 TranslationScale() const { return scale_; }

private:
    const Skeleton& source_;
    const Skeleton& target_;
    std::vector<i32> source_of_; // per target bone: the source bone, or -1
    i32 root_ = -1, source_root_ = -1;
    f32 scale_ = 1.0f;
    std::vector<Quaternion> source_rest_model_, target_rest_model_;
};

} // namespace aether::anim
