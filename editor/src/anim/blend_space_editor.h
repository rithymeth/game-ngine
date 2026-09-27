#pragma once

#include "anim/json_history.h"
#include "aether/animation/blend_space.h"

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace aether::editor {

// The blend space editor (Phase 16 step 6): a grid of the parameter axes
// with the samples as points you drag, the triangulation drawn between
// them, and a preview dot whose position shows the weights live.

class BlendSpaceDocument {
public:
    explicit BlendSpaceDocument(anim::BlendSpace space = {}, std::filesystem::path path = {});
    bool Save(std::string* error = nullptr);
    bool SaveAs(const std::filesystem::path& path, std::string* error = nullptr);

    const anim::BlendSpace& Get() const { return space_; }
    bool Dirty() const { return dirty_; }
    std::string Name() const;

    // Every change goes through Edit; the triangulation is rebuilt after it.
    void Edit(const std::string& label, const std::function<void(anim::BlendSpace&)>& change, const std::string& merge_key = {});
    bool Undo();
    bool Redo();
    bool CanUndo() const { return history_.CanUndo(); }
    bool CanRedo() const { return history_.CanRedo(); }

    usize AddSample(const std::string& clip, f32 x, f32 y);
    // Moves a sample, snapped to the grid when `divisions` > 0; `merge_key` merges a drag into one step.
    bool MoveSample(usize index, f32 x, f32 y, u32 divisions = 0, const std::string& merge_key = {});
    bool RemoveSample(usize index);
    const std::vector<anim::BlendDiagnostic>& Diagnostics() const { return diagnostics_; }

private:
    void Changed();
    anim::BlendSpace space_;
    std::filesystem::path path_;
    bool dirty_ = false;
    JsonHistory history_;
    std::vector<anim::BlendDiagnostic> diagnostics_;
};

// The grid's mapping between parameter space and the screen (y up on screen).
struct BlendSpaceCanvas {
    f32 x0 = 0, y0 = 0, width = 1, height = 1; // screen rectangle
    void ParamToScreen(const anim::BlendSpace& s, f32 px, f32 py, f32& sx, f32& sy) const;
    void ScreenToParam(const anim::BlendSpace& s, f32 sx, f32 sy, f32& px, f32& py) const;
    // The sample within `radius` pixels of (sx, sy), nearest first; -1 if none.
    i32 HitTest(const anim::BlendSpace& s, f32 sx, f32 sy, f32 radius = 8.0f) const;
};

class BlendSpaceEditor {
public:
    explicit BlendSpaceEditor(BlendSpaceDocument& document) : doc_(document) {}

    void Draw();

    // The clips offered when adding a sample.
    void SetClipNames(std::vector<std::string> names) { clip_names_ = std::move(names); }
    u32 grid_divisions = 4; // snapping; 0 is free

    f32 preview_x = 0, preview_y = 0;
    std::vector<anim::BlendWeight> PreviewWeights() const;
    i32 Selected() const { return selected_; }
    void Select(i32 sample) { selected_ = sample; }
    const BlendSpaceCanvas& Canvas() const { return canvas_; }

private:
    void DrawGrid();
    void DrawSampleList();
    BlendSpaceDocument& doc_;
    std::vector<std::string> clip_names_;
    BlendSpaceCanvas canvas_;
    i32 selected_ = -1;
    i32 dragging_ = -1;
    bool dragging_preview_ = false;
};

} // namespace aether::editor
