#include "world/scene_preview_panel.h"

#include <imgui.h>

#include <algorithm>

namespace aether::editor {

void ScenePreviewPanel::SetProject(const std::filesystem::path& project_file) {
    if (preview_) preview_->Wait(); // a render of the old project finishes before it is dropped
    preview_ = project_file.empty() ? nullptr : std::make_unique<ScenePreview>(project_file);
    note_.clear();
}

bool ScenePreviewPanel::Render() {
    if (!preview_) {
        note_ = "No project is open.";
        return false;
    }
    if (preview_->GetState() == ScenePreview::State::Running) return false;
    if (!scene_.SaveScene()) { // the player renders what is on disk
        note_ = "The scene could not be saved, so it was not rendered: " + scene_.Status();
        return false;
    }
    note_.clear();
    if (!preview_->Start(scene_.Path(), width, height)) {
        note_ = "A render is already running.";
        return false;
    }
    return true;
}

void ScenePreviewPanel::Poll() {
    if (!preview_) return;
    PreviewImage image;
    if (preview_->TakeImage(image) && !texture_.Set(image.width, image.height, image.rgba)) note_ = "The picture could not be shown.";
}

void ScenePreviewPanel::Draw() {
    if (!preview_) {
        ImGui::TextDisabled("No project is open.");
        return;
    }
    Poll();
    const bool running = preview_->GetState() == ScenePreview::State::Running;
    ImGui::BeginDisabled(running);
    if (ImGui::Button("Render")) Render();
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Save the scene and render it with the real renderer");
    ImGui::SameLine();
    const std::string& message = !note_.empty() ? note_ : preview_->Message();
    const bool failed = preview_->GetState() == ScenePreview::State::Failed || !note_.empty();
    if (failed) ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.45f, 1.0f), "%s", message.c_str());
    else ImGui::TextDisabled("%s", message.empty() ? "Renders the saved scene as the game would draw it, from the scene's camera." : message.c_str());
    if (!texture_.Valid()) return;
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const f32 scale = std::min(avail.x / static_cast<f32>(texture_.Width()), avail.y / static_cast<f32>(texture_.Height()));
    if (!(scale > 0.0f)) return;
    ImGui::Image(texture_.Ref(), ImVec2(static_cast<f32>(texture_.Width()) * scale, static_cast<f32>(texture_.Height()) * scale));
}

} // namespace aether::editor
