#pragma once

#include "aether/animation/root_motion.h"

#include <nlohmann/json.hpp>

#include <array>
#include <string>
#include <vector>

namespace aether::anim {

// Blend spaces (Phase 16 step 2, docs/design/PHASE_SPECS.md §16.3): clips
// placed on a 1D line or a 2D plane of parameters (speed, direction);
// the parameters pick the clips and their weights.

struct BlendAxis {
    std::string name = "Speed";
    f32 min = 0.0f, max = 1.0f;
};

struct BlendSample {
    std::string clip; // the clip's asset (or name)
    f32 x = 0.0f, y = 0.0f;
    f32 rate = 1.0f; // playback speed of this clip
};

struct BlendSpace {
    u32 dimensions = 1; // 1 or 2
    BlendAxis x, y;
    std::vector<BlendSample> samples;
    // 2D: a Delaunay triangulation of the samples (Triangulate fills it).
    std::vector<std::array<u32, 3>> triangles;
};

struct BlendWeight {
    u32 sample = 0;
    f32 weight = 0.0f;
};

// Diagnostics: BS001 no samples; BS002 two samples in the same place;
// BS003 a 2D space whose samples are all on a line; BS004 a bad axis
// (min >= max) or dimensions not 1 or 2; BS005 a sample outside its axis
// range (a warning: it still works); BS006 a sample with no clip or a
// rate <= 0.
struct BlendDiagnostic {
    std::string code;
    bool error = true;
    i32 sample = -1;
    std::string message;
};
std::vector<BlendDiagnostic> ValidateBlendSpace(const BlendSpace& space);

// Delaunay triangulation (Bowyer-Watson) of the samples' positions,
// normalized by the axis ranges so both axes count the same.
void Triangulate(BlendSpace& space);

// The weights at (x, y), summing to 1, heaviest first. Parameters are
// clamped to the axes. 1D: the two neighbouring samples. 2D: barycentric in
// the containing triangle, or at the nearest point on the hull outside it.
std::vector<BlendWeight> ComputeWeights(const BlendSpace& space, f32 x, f32 y = 0.0f);

// Several poses blended by weight (normalized); rotations are aligned to
// the first before they're averaged.
void BlendWeighted(const std::vector<const Pose*>& poses, const std::vector<f32>& weights, Pose& out);

// Plays a blend space with its clips in step: every clip is at the same
// normalized phase, and the phase advances at the weighted average of the
// clips' rates over their durations, so feet stay in sync as the weights change.
class BlendSpacePlayer {
public:
    BlendSpacePlayer(const BlendSpace& space, std::vector<const AnimationClip*> clips, const Skeleton& skeleton);

    void SetParameters(f32 x, f32 y = 0.0f);
    // Advances by `dt` seconds and samples the blended pose. With root
    // motion, the motion is taken out of the pose and returned in `motion`.
    void Update(f32 dt, Pose& out, RootMotionDelta* motion = nullptr, const RootMotionSettings* settings = nullptr);

    f32 Phase() const { return phase_; } // 0..1
    f32 PreviousPhase() const { return previous_phase_; } // before the last Update
    const AnimationClip* ClipOf(u32 sample) const { return sample < clips_.size() ? clips_[sample] : nullptr; }
    const std::vector<BlendWeight>& Weights() const { return weights_; }
    f32 CycleDuration() const; // seconds for one cycle at the current weights

private:
    const BlendSpace& space_;
    std::vector<const AnimationClip*> clips_;
    const Skeleton& skeleton_;
    f32 x_ = 0.0f, y_ = 0.0f;
    f32 phase_ = 0.0f, previous_phase_ = 0.0f;
    std::vector<BlendWeight> weights_;
    std::vector<Pose> scratch_;
};

// .ablend files.
nlohmann::json BlendSpaceToJson(const BlendSpace& space);
bool BlendSpaceFromJson(const nlohmann::json& json, BlendSpace& out, std::string* error = nullptr);

} // namespace aether::anim
