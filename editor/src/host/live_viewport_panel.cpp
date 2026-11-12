#include "host/live_viewport_panel.h"

#include "core/scene_preview.h"

#include <imgui.h>

#include <algorithm>

namespace aether::editor {

bool LiveViewportPanel::Start() {
    const std::filesystem::path project = project_file_ ? project_file_() : std::filesystem::path();
    if (project.empty()) {
        note_ = "No project is open.";
        return false;
    }
    if (!scene_.SaveScene()) { // the cook reads what is on disk
        note_ = "The scene could not be saved, so it was not started: " + scene_.Status();
        return false;
    }
    std::filesystem::path cooked;
    std::string error;
    if (!CookForPreview(project, scene_.Path(), project.parent_path() / "Saved" / "LiveViewport", &cooked, &error) ||
        !viewport_.Load(cooked, scene_.Path(), width, height, &error)) {
        note_ = error;
        return false;
    }
    note_.clear();
    since_frame_ = 1.0f; // draw straight away
    return true;
}

void LiveViewportPanel::Stop() {
    viewport_.Unload();
    note_.clear();
}

bool LiveViewportPanel::Advance(f32 dt) {
    if (!viewport_.Loaded()) return false;
    since_frame_ += dt;
    if (since_frame_ < 1.0f / std::max(max_fps, 1.0f)) return false;
    const f32 elapsed = since_frame_;
    since_frame_ = 0.0f;
    if (!viewport_.Frame(elapsed, camera_.Eye(), camera_.Rotation(), camera_.fov_degrees)) return false;
    fps_ = fps_ <= 0.0f ? 1.0f / elapsed : fps_ * 0.9f + (1.0f / elapsed) * 0.1f; // smoothed, so the number can be read
    return texture_.Set(viewport_.Width(), viewport_.Height(), viewport_.Pixels());
}

void LiveViewportPanel::Draw(f32 dt) {
    const bool loaded = viewport_.Loaded();
    if (ImGui::Button(loaded ? "Reload" : "Start")) Start();
    ImGui::SetItemTooltip(loaded ? "Save the scene, cook it and load it again to see your edits" : "Save the scene, cook the project and run it here");
    if (loaded) {
        ImGui::SameLine();
        if (ImGui::Button("Stop")) Stop();
    }
    ImGui::SameLine();
    if (!note_.empty()) ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.45f, 1.0f), "%s", note_.c_str());
    else if (viewport_.Loaded()) ImGui::TextDisabled("%.0f fps  |  %zu entities  |  right-drag: look   middle-drag: pan   wheel: zoom", fps_, viewport_.EntityCount());
    else ImGui::TextDisabled("Runs the saved scene with the real renderer. Edits show after Reload.");
    if (!viewport_.Loaded()) return;
    Advance(dt);
    if (!texture_.Valid()) return;
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const f32 scale = std::min(avail.x / static_cast<f32>(texture_.Width()), avail.y / static_cast<f32>(texture_.Height()));
    if (!(scale > 0.0f)) return;
    const ImVec2 size(static_cast<f32>(texture_.Width()) * scale, static_cast<f32>(texture_.Height()) * scale);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddImage(texture_.Ref(), origin, ImVec2(origin.x + size.x, origin.y + size.y));
    ImGui::InvisibleButton("##live_viewport", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
    HandleCameraInput(camera_, ImGui::IsItemHovered(), ImGui::IsItemActive(), size);
}

} // namespace aether::editor
