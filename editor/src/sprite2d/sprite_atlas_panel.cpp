#include "sprite2d/sprite_atlas_panel.h"

#include "aether/assets/asset_database.h"
#include "aether/assets/image.h"

#include <algorithm>
#include <limits>

namespace aether::editor {

bool SpriteAtlasPanel::Open(const std::filesystem::path& file, const assets::AssetDatabase* database, std::string* error) {
    sprite2d::SpriteAtlas next;
    if (!sprite2d::LoadAtlas(file, next, error)) return false;
    Clear();
    asset_name_ = file.filename().string();
    atlas_ = std::move(next);
    if (atlas_.texture.IsNull()) {
        texture_status_ = "No texture assigned; atlas metadata is still available.";
        return true;
    }
    const auto* record = database ? database->Find(atlas_.texture) : nullptr;
    if (!record || record->missing || record->importer != "Texture") {
        texture_status_ = "The atlas texture was not found in this project.";
        return true;
    }
    const auto path = database->SourcePath(record->guid);
    constexpr u64 kMaxPreviewPixels = 16 * 1024 * 1024;
    if (!assets::DecodeImageFile(path.string(), pending_image_, kMaxPreviewPixels)) {
        texture_status_ = "Cannot preview " + record->path + ": image is unreadable or too large.";
    } else if (pending_image_.width != atlas_.width || pending_image_.height != atlas_.height) {
        texture_status_ = record->path + " does not match the atlas pixel dimensions. Re-slice it or restore the matching sheet.";
        pending_image_ = {};
    } else {
        texture_status_ = record->path;
    }
    return true;
}

void SpriteAtlasPanel::Clear() {
    atlas_ = {};
    pending_image_ = {};
    asset_name_.clear();
    texture_status_.clear();
    show_texture_ = playing_ = false;
    frame_ = 0;
    clip_ = -1;
    elapsed_ = 0.0f;
}

void SpriteAtlasPanel::Draw() {
    if (asset_name_.empty()) {
        ImGui::TextWrapped("Open a .aatlas file from the Content Browser.");
        return;
    }
    if (!pending_image_.pixels.empty()) {
        show_texture_ = image_.Set(pending_image_.width, pending_image_.height, pending_image_.pixels);
        if (!show_texture_) texture_status_ = "Source texture is too large for this preview.";
        pending_image_ = {};
    }
    ImGui::TextUnformatted(asset_name_.c_str());
    ImGui::Text("%u x %u px | %zu frames | %zu clips", atlas_.width, atlas_.height, atlas_.frames.size(), atlas_.clips.size());
    ImGui::TextWrapped("%s", texture_status_.c_str());
    if (atlas_.frames.empty()) return;
    if (ImGui::BeginCombo("Clip", clip_ < 0 ? "Manual frame" : atlas_.clips[clip_].name.c_str())) {
        if (ImGui::Selectable("Manual frame", clip_ < 0)) {
            clip_ = -1;
            playing_ = false;
        }
        for (usize i = 0; i < atlas_.clips.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::Selectable(atlas_.clips[i].name.c_str(), clip_ == static_cast<int>(i))) {
                clip_ = static_cast<int>(i);
                elapsed_ = 0.0f;
                playing_ = false;
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    const sprite2d::SpriteFrame* frame = nullptr;
    if (clip_ >= 0) {
        const auto& clip = atlas_.clips[clip_];
        ImGui::Text("%.2f fps | %s | %zu frames", clip.fps, clip.loop ? "Loop" : "Once", clip.frames.size());
        if (!clip.frames.empty()) {
            const bool was_playing = playing_;
            if (ImGui::Button(playing_ ? "Pause" : "Play")) {
                bool finished = false;
                (void)sprite2d::ClipFrameAt(clip, elapsed_, &finished);
                if (finished) elapsed_ = 0.0f;
                playing_ = !playing_;
            }
            ImGui::SameLine();
            const bool restarted = ImGui::Button("Restart");
            if (restarted) elapsed_ = 0.0f;
            if (!restarted && was_playing && playing_) elapsed_ += ImGui::GetIO().DeltaTime;
            bool finished = false;
            const usize index = sprite2d::ClipFrameAt(clip, elapsed_, &finished);
            if (finished) playing_ = false;
            ImGui::Text("Clip frame %zu / %zu", index + 1, clip.frames.size());
            frame = atlas_.FindFrame(clip.frames[index]);
        }
    } else {
        const int last = static_cast<int>(std::min(atlas_.frames.size() - 1, static_cast<usize>(std::numeric_limits<int>::max())));
        ImGui::SliderInt("Frame index", &frame_, 0, last);
        frame = &atlas_.frames[frame_];
    }
    if (!frame) return;
    ImGui::TextUnformatted(frame->name.c_str());
    ImGui::Text("Rect: (%d, %d) %d x %d px | Pivot: (%.2f, %.2f)", frame->x, frame->y,
                frame->w, frame->h, frame->pivot_x, frame->pivot_y);
    f32 uv[4];
    if (show_texture_ && atlas_.Uv(*frame, uv)) {
        const f32 max_width = std::clamp(ImGui::GetContentRegionAvail().x, 1.0f, 320.0f);
        const f32 scale = std::min({max_width / frame->w, 240.0f / frame->h, 8.0f});
        ImGui::Image(image_.Ref(), ImVec2(frame->w * scale, frame->h * scale), ImVec2(uv[0], uv[1]), ImVec2(uv[2], uv[3]));
    }
}

} // namespace aether::editor
