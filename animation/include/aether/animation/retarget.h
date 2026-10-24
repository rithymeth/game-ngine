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
// ratio of the skeletons' root heights (when the root is on the ground, of the
// first mapped bone above it, such as the hips); every other bone keeps the
// target's own lengths.

struct BoneMapping {
    std::vector<std::pair<std::string, std::string>> bones; // source name -> target name
    std::string root;                                        // the target bone that carries translation (default: the first root)
};

// Pairs bones by name, ignoring case and a rig prefix ("mixamorig:Hips" matches "hips").
BoneMapping AutoMapBones(const Skeleton& source, const Skeleton& target);

// Whether a mapping can carry a source skeleton's animation onto a target, with
// a reason per problem (M8.6: "retargeting reports incompatible skeletons
// clearly"). Errors: RT001 a skeleton isn't valid; RT002 the mapping names a
// bone a skeleton doesn't have; RT003 nothing maps; RT004 the root (the
// mapping's `root`, else the first mapped target bone) isn't mapped or isn't
// in the target; RT005 the hierarchies disagree (a mapped bone's nearest mapped
// ancestor doesn't map to an ancestor of its source bone); RT006 a required
// target bone isn't mapped; RT007 two source bones drive one target bone.
// Warnings: RT008 target bones left unmapped (they keep their rest pose);
// RT009 no mapped bone stands above the ground, so root translation isn't scaled.
struct RetargetIssue {
    std::string code;
    bool error = true;
    std::string bone; // the bone it is about, when there is one
    std::string message;
};
struct RetargetReport {
    bool compatible = false; // no errors
    usize mapped = 0;        // target bones with a source bone
    usize target_bones = 0;
    std::vector<RetargetIssue> issues;
    f32 Coverage() const { return target_bones ? static_cast<f32>(mapped) / static_cast<f32>(target_bones) : 0.0f; }
    // One line per issue: "RT004 error [hips]: ...".
    std::string Describe() const;
};
RetargetReport ValidateRetarget(const Skeleton& source, const Skeleton& target, const BoneMapping& mapping,
                                const std::vector<std::string>& required_target_bones = {});

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
