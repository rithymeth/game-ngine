#include "host/live_viewport_panel.h"

#include "aether/scene/components.h"
#include "aether/scene/serialization.h"
#include "core/scene_preview.h"

#include <imgui.h>

#include <algorithm>

namespace aether::editor {

namespace {

// The glTF models a scene's ModelRenderers name, once each, sorted.
std::vector<std::string> ModelPaths(const SceneDocument& document) {
    std::vector<std::string> paths;
    document.GetWorld().ForEach<ModelRenderer>([&](const ModelRenderer& renderer) {
        if (renderer.asset_path[0] != '\0') paths.emplace_back(renderer.asset_path);
    });
    std::sort(paths.begin(), paths.end());
    paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
    return paths;
}

} // namespace

usize LiveViewportPanel::UncookedModels() const {
    usize missing = 0;
    for (const std::string& path : ModelPaths(scene_.Document())) {
        if (!std::binary_search(cooked_models_.begin(), cooked_models_.end(), path)) ++missing;
    }
    return missing;
}

bool LiveViewportPanel::Start() {
    const std::filesystem::path project = project_file_ ? project_file_() : std::filesystem::path();
    if (project.empty()) {
        note_ = "No project is open.";
        return false;
    }
    // The scene is run as it is open, unsaved edits and all, so nothing is written over the project's own file: the cook only has to find the
    // models the scene uses (whose assets are in the project), and the scene itself goes to the game from memory.
    const SceneDocument& document = scene_.Document();
    const std::vector<std::string> models = ModelPaths(document);
    const std::vector<u8> bytes = SaveSceneToMemory(document.GetWorld());
    const u64 revision = document.Revision();
    std::filesystem::path cooked;
    std::string error;
    if (!CookForPreview(project, scene_.Path(), project.parent_path() / "Saved" / "LiveViewport", &cooked, &error, models) ||
        !viewport_.Load(cooked, scene_.Path(), width, height, &error, &bytes)) {
        note_ = error;
        return false;
    }
    note_.clear();
    cooked_models_ = models;
    last_revision_ = revision;
    since_frame_ = 1.0f; // draw straight away
    since_reload_ = 0.0f;
    return true;
}

void LiveViewportPanel::Stop() {
    viewport_.Unload();
    note_.clear();
}

bool LiveViewportPanel::Advance(f32 dt) {
    if (!viewport_.Loaded()) return false;
    since_frame_ += dt;
    since_reload_ += dt;
    bool reloaded = false;
    const SceneDocument& document = scene_.Document();
    if (document.Revision() != last_revision_ && since_reload_ >= reload_interval) {
        last_revision_ = document.Revision(); // taken before the copy, so an edit during it is seen next time
        since_reload_ = 0.0f;
        std::string error;
        if (viewport_.SetScene(SaveSceneToMemory(document.GetWorld()), &error)) reloaded = true;
        else note_ = error;
    }
    if (!reloaded && since_frame_ < 1.0f / std::max(max_fps, 1.0f)) return false;
    const f32 elapsed = since_frame_;
    since_frame_ = 0.0f;
    if (!viewport_.Frame(run_game ? elapsed : 0.0f, camera_.Eye(), camera_.Rotation(), camera_.fov_degrees)) return false;
    fps_ = fps_ <= 0.0f ? 1.0f / std::max(elapsed, 1e-3f) : fps_ * 0.9f + (1.0f / std::max(elapsed, 1e-3f)) * 0.1f; // smoothed, so the number can be read
    return texture_.Set(viewport_.Width(), viewport_.Height(), viewport_.Pixels());
}

void LiveViewportPanel::Draw(f32 dt) {
    const bool loaded = viewport_.Loaded();
    if (ImGui::Button(loaded ? "Reload" : "Start")) Start();
    ImGui::SetItemTooltip(loaded ? "Cook again (picks up a model added since) and load the open scene" : "Cook the project's models and run the open scene here, unsaved edits included");
    if (loaded) {
        ImGui::SameLine();
        if (ImGui::Button("Stop")) Stop();
        ImGui::SameLine();
        ImGui::Checkbox("Run game", &run_game);
        ImGui::SetItemTooltip("Let physics and scripts run. Off: the world is frozen, as authored");
    }
    ImGui::SameLine();
    const usize uncooked = loaded ? UncookedModels() : 0;
    if (!note_.empty()) ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.45f, 1.0f), "%s", note_.c_str());
    else if (uncooked > 0) ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.4f, 1.0f), "%zu model(s) in the scene are not cooked yet: press Reload to see them", uncooked);
    else if (loaded) ImGui::TextDisabled("%.0f fps  |  %zu entities  |  edits show as you make them  |  right-drag: look  middle-drag: pan  wheel: zoom", fps_, viewport_.EntityCount());
    else ImGui::TextDisabled("Runs the open scene with the real renderer, edits and all. Open the scene you want first.");
    if (!loaded) return;
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
