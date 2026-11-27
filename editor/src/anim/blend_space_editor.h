#pragma once

#include "anim/blend_space_document.h"

#include <string>
#include <vector>

namespace aether::editor {

// ImGui view for the blend space document; the document stays UI independent.

class BlendSpaceEditor {
public:
    explicit BlendSpaceEditor(BlendSpaceDocument& document) : doc_(document) {}

    void Draw();
    void ResetForLoadedSpace();

    // The clips offered when adding a sample.
    void SetClipNames(std::vector<std::string> names) { clip_names_ = std::move(names); }
    u32 grid_divisions = 4; // snapping; 0 is free

    f32 preview_x = 0, preview_y = 0;
    std::vector<anim::BlendWeight> PreviewWeights() const;
    i32 Selected() const { return selected_; }
    void Select(i32 sample) { selected_ = sample; }
    const BlendSpaceCanvas& Canvas() const { return canvas_; }

private:
    void ClearSampleInteraction();
    void DrawGrid();
    void DrawSampleList();
    BlendSpaceDocument& doc_;
    std::vector<std::string> clip_names_;
    BlendSpaceCanvas canvas_;
    i32 selected_ = -1;
    i32 dragging_ = -1;
    u64 drag_id_ = 0;
    bool dragging_preview_ = false;
    bool drag_moved_ = false;
    f32 drag_start_x_ = 0, drag_start_y_ = 0;
};

} // namespace aether::editor
