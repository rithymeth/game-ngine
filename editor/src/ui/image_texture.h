#pragma once

#include "aether/core/base.h"

#include <imgui.h>

#include <vector>

struct ImTextureData;

namespace aether::editor {

// One RGBA picture shown through ImGui (ImGui::Image): the renderer backend uploads it on the next frame. Replacing it with a picture of
// the same size updates it in place; a different size makes a new texture and lets the old one be destroyed by the backend first.
// Uses ImGui's user-texture support, which is marked experimental in imgui_internal.h; this is the only place that touches it.
class ImageTexture {
public:
    ImageTexture() = default;
    ~ImageTexture();
    ImageTexture(const ImageTexture&) = delete;
    ImageTexture& operator=(const ImageTexture&) = delete;

    // Shows `rgba` (width * height * 4 bytes, top row first). False for an empty or mis-sized picture (the old one stays).
    bool Set(u32 width, u32 height, const std::vector<u8>& rgba);
    bool Valid() const { return current_ != nullptr; }
    u32 Width() const { return width_; }
    u32 Height() const { return height_; }
    // For ImGui::Image. Only meaningful while Valid().
    ImTextureRef Ref() const;

private:
    void Collect(); // frees what the backend has finished destroying

    ImTextureData* current_ = nullptr;
    std::vector<ImTextureData*> retired_; // replaced by a different size, waiting for the backend to destroy them
    u32 width_ = 0;
    u32 height_ = 0;
};

} // namespace aether::editor
