#include "ui/image_texture.h"

#include <imgui_internal.h>

#include <algorithm>
#include <cstring>

namespace aether::editor {

namespace {

// A texture the renderer backend has made GPU resources for must outlive the backend's last look at it (its shutdown walks the list of
// textures from the last frame), so one still holding backend data is unregistered but not freed when the editor closes: it is a few
// hundred kilobytes at the end of the process, against a use-after-free.
void Release(ImTextureData* tex) {
    if (!tex) return;
    if (ImGui::GetCurrentContext() && tex->RefCount > 0) ImGui::UnregisterUserTexture(tex);
    if (tex->BackendUserData == nullptr) IM_DELETE(tex);
}

} // namespace

ImageTexture::~ImageTexture() {
    Release(current_);
    for (ImTextureData* tex : retired_) Release(tex);
}

void ImageTexture::Collect() {
    for (auto it = retired_.begin(); it != retired_.end();) {
        if ((*it)->Status == ImTextureStatus_Destroyed) {
            if (ImGui::GetCurrentContext()) ImGui::UnregisterUserTexture(*it);
            IM_DELETE(*it);
            it = retired_.erase(it);
        } else {
            ++it;
        }
    }
}

bool ImageTexture::Set(u32 width, u32 height, const std::vector<u8>& rgba) {
    if (width == 0 || height == 0 || width > 16384 || height > 16384) return false;
    if (rgba.size() != static_cast<usize>(width) * height * 4) return false;
    if (!ImGui::GetCurrentContext()) return false;
    Collect();
    if (current_ && width_ == width && height_ == height) {
        std::memcpy(current_->Pixels, rgba.data(), rgba.size());
        ImTextureDataQueueUpload(current_, 0, 0, static_cast<int>(width), static_cast<int>(height));
        return true;
    }
    if (current_) {
        current_->WantDestroyNextFrame = true; // the backend destroys it a frame after it was last drawn
        retired_.push_back(current_);
    }
    current_ = IM_NEW(ImTextureData)();
    current_->Create(ImTextureFormat_RGBA32, static_cast<int>(width), static_cast<int>(height));
    std::memcpy(current_->Pixels, rgba.data(), rgba.size());
    current_->UseColors = true;
    ImGui::RegisterUserTexture(current_);
    width_ = width;
    height_ = height;
    return true;
}

ImTextureRef ImageTexture::Ref() const { return current_ ? current_->GetTexRef() : ImTextureRef(); }

} // namespace aether::editor
