#include "sprite2d/sprite_atlas_panel.h"

#include "core/scene_document.h"

#include "aether/assets/asset_database.h"
#include "aether/assets/image.h"
#include "aether/sprite2d/components.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace aether::editor {
namespace {

bool FitsName(std::string_view name, usize size) {
    return !name.empty() && name.size() < size && name.find('\0') == std::string_view::npos;
}

template <usize N>
bool MatchesName(std::string_view name, const char (&stored)[N]) {
    return name == std::string_view(stored, std::find(stored, stored + N, '\0') - stored);
}

} // namespace

bool SpriteAtlasPanel::Open(const std::filesystem::path& file, const assets::AssetDatabase* database, std::string* error) {
    sprite2d::SpriteAtlas next;
    if (!sprite2d::LoadAtlas(file, next, error)) return false;
    Clear();
    asset_name_ = file.filename().string();
    atlas_ = std::move(next);
    if (database) {
        const auto relative = file.lexically_relative(database->ContentRoot()).generic_string();
        const auto* asset = database->FindByPath(relative);
        if (asset && !asset->missing && asset->importer == "SpriteAtlas") atlas_guid_ = asset->guid;
    }
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
    atlas_guid_ = {};
    pending_image_ = {};
    asset_name_.clear();
    texture_status_.clear();
    show_texture_ = playing_ = false;
    frame_ = 0;
    clip_ = -1;
    elapsed_ = 0.0f;
}

const char* SpriteAtlasPanel::SelectionProblem(const SceneDocument& scene) const {
    if (scene.PlayState() != PlaySession::State::Editing) return "Stop playing or simulating to edit the scene.";
    if (atlas_guid_.IsNull() || scene.Selection().size() != 1)
        return "Select one scene Sprite that uses this atlas.";
    const auto entity = scene.Selection().front();
    const auto& world = scene.GetWorld();
    const auto* sprite = world.IsAlive(entity) ? world.GetComponent<sprite2d::Sprite>(entity) : nullptr;
    if (!sprite || sprite->atlas.guid != atlas_guid_) return "Select one scene Sprite that uses this atlas.";
    return nullptr;
}

const char* SpriteAtlasPanel::FrameAssignmentProblem(const SceneDocument& scene, const std::string& name) const {
    if (const char* problem = SelectionProblem(scene)) return problem;
    const auto entity = scene.Selection().front();
    const auto& world = scene.GetWorld();
    const auto* sprite = world.GetComponent<sprite2d::Sprite>(entity);
    if (world.GetComponent<sprite2d::SpriteAnimator>(entity)) return "A Sprite Animator controls this Sprite's frame.";
    if (!FitsName(name, sizeof(sprite->frame)) || !atlas_.FindFrame(name))
        return "This frame name cannot be stored on a Sprite.";
    if (MatchesName(name, sprite->frame))
        return "The selected Sprite already uses this frame.";
    return nullptr;
}

bool SpriteAtlasPanel::ApplyFrame(SceneDocument& scene, const std::string& name) const {
    if (FrameAssignmentProblem(scene, name)) return false;
    sprite2d::Sprite next;
    sprite2d::SetSpriteFrame(next, name);
    using FrameStorage = decltype(next.frame);
    return scene.SetComponentField(scene.Selection().front(), GetComponentId<sprite2d::Sprite>(), "frame",
                                   reflect::Any::CopyOf(reflect::Reflect<FrameStorage>(), next.frame));
}

const char* SpriteAtlasPanel::ClipAssignmentProblem(const SceneDocument& scene, const sprite2d::SpriteClip* clip) const {
    if (const char* problem = SelectionProblem(scene)) return problem;
    const auto* animator = scene.GetWorld().GetComponent<sprite2d::SpriteAnimator>(scene.Selection().front());
    if (!animator) return "Add a Sprite Animator to the selected Sprite first.";
    if (!clip || !FitsName(clip->name, sizeof(animator->clip))) return "This clip name cannot be stored on a Sprite Animator.";
    if (clip->frames.empty() || !std::isfinite(clip->fps) || clip->fps <= 0.0f)
        return "The clip needs at least one frame and a positive FPS.";
    if (std::any_of(clip->frames.begin(), clip->frames.end(), [](const std::string& frame) {
        return !FitsName(frame, sizeof(sprite2d::Sprite::frame));
    })) return "The clip contains a frame name that cannot be stored on a Sprite.";
    if (MatchesName(clip->name, animator->clip)) return "The selected Sprite Animator already uses this clip.";
    return nullptr;
}

bool SpriteAtlasPanel::ApplyClip(SceneDocument& scene, const std::string& name) const {
    const auto* clip = atlas_.FindClip(name);
    if (ClipAssignmentProblem(scene, clip)) return false;
    sprite2d::SpriteAnimator next;
    sprite2d::SetName(next.clip, clip->name);
    using ClipStorage = decltype(next.clip);
    return scene.SetComponentField(scene.Selection().front(), GetComponentId<sprite2d::SpriteAnimator>(), "clip",
                                   reflect::Any::CopyOf(reflect::Reflect<ClipStorage>(), next.clip));
}

void SpriteAtlasPanel::Draw(SceneDocument* scene) {
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
        if (scene) {
            const char* problem = ClipAssignmentProblem(*scene, &clip);
            ImGui::BeginDisabled(problem != nullptr);
            if (ImGui::Button("Use clip on selected Sprite Animator")) ApplyClip(*scene, clip.name);
            ImGui::EndDisabled();
            if (problem && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", problem);
        }
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
    if (frame) {
        ImGui::TextUnformatted(frame->name.c_str());
        ImGui::Text("Rect: (%d, %d) %d x %d px | Pivot: (%.2f, %.2f)", frame->x, frame->y,
                    frame->w, frame->h, frame->pivot_x, frame->pivot_y);
        if (scene && clip_ < 0) {
            const char* problem = FrameAssignmentProblem(*scene, frame->name);
            ImGui::BeginDisabled(problem != nullptr);
            if (ImGui::Button("Use frame on selected Sprite")) ApplyFrame(*scene, frame->name);
            ImGui::EndDisabled();
            if (problem && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", problem);
        }
        f32 uv[4];
        if (show_texture_ && atlas_.Uv(*frame, uv)) {
            const f32 max_width = std::clamp(ImGui::GetContentRegionAvail().x, 1.0f, 320.0f);
            const f32 scale = std::min({max_width / frame->w, 240.0f / frame->h, 8.0f});
            ImGui::Image(image_.Ref(), ImVec2(frame->w * scale, frame->h * scale), ImVec2(uv[0], uv[1]), ImVec2(uv[2], uv[3]));
        }
    }

    if (!show_texture_) return;
    ImGui::Separator();
    ImGui::TextUnformatted("Source sheet (click a frame)");
    const f32 max_width = std::clamp(ImGui::GetContentRegionAvail().x, 1.0f, 420.0f);
    const f32 scale = std::min({max_width / atlas_.width, 220.0f / atlas_.height, 8.0f});
    ImGui::Image(image_.Ref(), ImVec2(atlas_.width * scale, atlas_.height * scale));
    const ImVec2 top_left = ImGui::GetItemRectMin();
    if (ImGui::IsItemHovered()) {
        const ImVec2 mouse = ImGui::GetMousePos();
        const f32 x = (mouse.x - top_left.x) / scale;
        const f32 y = (mouse.y - top_left.y) / scale;
        // Atlas order breaks ties for hand-authored overlapping frames.
        const auto hit = std::find_if(atlas_.frames.begin(), atlas_.frames.end(), [&](const sprite2d::SpriteFrame& candidate) {
            return x >= candidate.x && x - candidate.x < candidate.w &&
                   y >= candidate.y && y - candidate.y < candidate.h;
        });
        if (hit != atlas_.frames.end()) {
            if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
                frame_ = static_cast<int>(hit - atlas_.frames.begin());
                clip_ = -1;
                playing_ = false;
                frame = &*hit;
            }
            ImGui::SetTooltip("%s", hit->name.c_str());
        }
    }
    if (frame) {
        const ImVec2 lower(top_left.x + frame->x * scale, top_left.y + frame->y * scale);
        const ImVec2 upper(lower.x + frame->w * scale, lower.y + frame->h * scale);
        auto* draw = ImGui::GetWindowDrawList();
        draw->AddRect(lower, upper, IM_COL32(0, 0, 0, 210), 0.0f);
        draw->AddRect(lower, upper, IM_COL32(255, 202, 99, 255));
    }
}

} // namespace aether::editor
