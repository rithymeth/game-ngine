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
