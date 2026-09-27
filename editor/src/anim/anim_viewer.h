#pragma once

#include "aether/animation/clip.h"

#include <string>
#include <vector>

namespace aether::editor {

// The animation asset viewer (Phase 16 step 6): a clip's playback bar,
// the skeleton's bone hierarchy, the notify track (add, drag, rename,
// delete notifies) and a curve of the selected bone's translation. The
// 3D preview is the renderer's (step 7).
class ClipViewer {
public:
    ClipViewer(const anim::Skeleton& skeleton, anim::AnimationClip& clip) : skeleton_(skeleton), clip_(clip) {}

    void Draw();

    // Playback.
    void Advance(f32 dt); // plays when playing; the notifies it crossed go to Fired()
    void SetTime(f32 time);
    f32 Time() const { return time_; }
    bool playing = false;
    bool loop = true;
    f32 speed = 1.0f;
    const anim::Pose& CurrentPose() const { return pose_; }
    const std::vector<std::string>& Fired() const { return fired_; }

    // Notify editing (undoable; the clip is edited in place).
    usize AddNotify(const std::string& name, f32 time, f32 duration = 0.0f);
    bool MoveNotify(usize index, f32 time);
    bool RenameNotify(usize index, const std::string& name);
    bool RemoveNotify(usize index);
    bool Undo();
    bool Redo();
    bool Dirty() const { return dirty_; }

    i32 selected_bone = -1;
    i32 selected_notify = -1;
    // The selected bone's translation (component 0..2) sampled across the clip, for the curve view.
    std::vector<f32> Curve(i32 bone, u32 component, u32 samples = 64) const;

private:
    void Record();
    void Sample();
    void DrawBones(i32 bone);
    anim::Skeleton const& skeleton_;
    anim::AnimationClip& clip_;
    f32 time_ = 0.0f;
    anim::Pose pose_;
    std::vector<std::string> fired_;
    std::vector<std::vector<anim::AnimNotify>> undo_, redo_;
    bool dirty_ = false;
    i32 dragging_notify_ = -1;
};

} // namespace aether::editor
