#include "host/scene_shading.h"

#include "aether/renderer/material_instance.h"
#include "aether/scene/components.h"
#include "aether/scene/gameplay.h"
#include "aether/scene/serialization.h"
#include "core/scene_preview.h"

#include <algorithm>
#include <system_error>

namespace aether::editor {

namespace {

// The assets a scene needs in the cook, once each, sorted: the glTF models its ModelRenderers name, and the images its MaterialParameters name as
// texture parameters (a path that is not a file under `content` is left out, so a typo does not stop the cook). As the Live Viewport cooks.
std::vector<std::string> ModelPaths(const SceneDocument& document, const std::filesystem::path& content) {
    std::vector<std::string> paths;
    document.GetWorld().ForEach<ModelRenderer>([&](const ModelRenderer& renderer) {
        if (renderer.asset_path[0] != '\0') paths.emplace_back(renderer.asset_path);
    });
    document.GetWorld().ForEach<MaterialParameters>([&](const MaterialParameters& overrides) {
        for (const MaterialParameterValue& value : overrides.values) {
            std::error_code ignored;
            if (value.is_texture && !value.texture.empty() && std::filesystem::is_regular_file(content / value.texture, ignored)) paths.push_back(value.texture);
        }
        std::error_code ignored;
        if (!overrides.material.empty() && std::filesystem::is_regular_file(content / overrides.material, ignored)) paths.push_back(overrides.material);
    });
    std::sort(paths.begin(), paths.end());
    paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
    return paths;
}

// The picture fills the view's rectangle, so it has the view's aspect; at most 1920 pixels along its long side, though (a 4K dock is not worth the GPU).
void FitResolution(u32& width, u32& height) {
    width = std::clamp(width, 64u, 4096u);
    height = std::clamp(height, 64u, 4096u);
    const f32 scale = 1920.0f / static_cast<f32>(std::max(width, height));
    if (scale < 1.0f) {
        width = std::max(64u, static_cast<u32>(static_cast<f32>(width) * scale));
        height = std::max(64u, static_cast<u32>(static_cast<f32>(height) * scale));
    }
}

} // namespace

LiveSceneShading::LiveSceneShading(rhi::IDevice& device, ScenePanel& scene, std::function<std::filesystem::path()> project_file)
    : scene_(scene), project_file_(std::move(project_file)), viewport_(device) {}

LiveSceneShading::~LiveSceneShading() = default;

bool LiveSceneShading::Start(const SceneDocument& document, u32 width, u32 height) {
    const std::filesystem::path project = project_file_ ? project_file_() : std::filesystem::path();
    if (project.empty()) {
        note_ = "No project is open.";
        return false;
    }
    u32 draw_width = width, draw_height = height;
    FitResolution(draw_width, draw_height);
    scene_.ModelBounds().Clear(); // a model may have changed on disk since it was measured
    const std::vector<std::string> models = ModelPaths(document, scene_.ContentRoot());
    const std::vector<u8> bytes = SaveSceneToMemory(document.GetWorld());
    const u64 revision = document.Revision();
    const u64 structure = document.StructureRevision();
    std::filesystem::path cooked;
    std::string error;
    if (!CookForPreview(project, scene_.Path(), project.parent_path() / "Saved" / "SceneViewShading", &cooked, &error, models) ||
        !viewport_.Load(cooked, scene_.Path(), draw_width, draw_height, &error, &bytes)) {
        note_ = error.empty() ? "The scene could not be started." : error;
        return false;
    }
    note_.clear();
    cooked_ = cooked;
    scene_path_ = scene_.Path();
    width_ = viewport_.Width();
    height_ = viewport_.Height();
    last_revision_ = revision;
    last_structure_ = structure;
    running_ = true;
    since_frame_ = 1.0f; // draw the first frame now
    since_reload_ = 0.0f;
    wanted_width_ = width;
    wanted_height_ = height;
    size_still_ = 0.0f;
    return true;
}

bool LiveSceneShading::LoadAgain(const SceneDocument& document, u32 width, u32 height) {
    if (cooked_.empty()) return Start(document, width, height);
    u32 draw_width = width, draw_height = height;
    FitResolution(draw_width, draw_height);
    std::string error;
    const std::vector<u8> bytes = SaveSceneToMemory(document.GetWorld());
    if (!viewport_.Load(cooked_, scene_path_, draw_width, draw_height, &error, &bytes)) {
        note_ = error.empty() ? "The picture could not be rebuilt at the new size." : error;
        return false;
    }
    note_.clear();
    width_ = viewport_.Width();
    height_ = viewport_.Height();
    last_revision_ = document.Revision();
    last_structure_ = document.StructureRevision();
    since_frame_ = 1.0f;
    return true;
}

bool LiveSceneShading::Advance(SceneDocument& document, const SceneViewCamera& camera, u32 width, u32 height, SceneViewMode mode, f32 dt) {
    if (!running_) {
        if (!Start(document, width, height)) return false;
    } else if (scene_path_ != scene_.Path()) { // another scene was opened under this view
        const u32 keep_width = width_ > 0 ? width_ : width, keep_height = height_ > 0 ? height_ : height;
        if (!Start(document, keep_width, keep_height)) return false;
    }
    viewport_.SetViewMode(mode == SceneViewMode::Unlit      ? player::SceneRenderer::ViewMode::Unlit
                          : mode == SceneViewMode::Normals  ? player::SceneRenderer::ViewMode::Normals
                                                            : player::SceneRenderer::ViewMode::Lit);
    const u64 revision = document.Revision();
    since_reload_ += dt;
    if (revision != last_revision_ && since_reload_ >= reload_interval) {
        since_reload_ = 0.0f;
        last_revision_ = revision; // taken before the copy, so an edit made during it is seen next time
        const u64 structure = document.StructureRevision();
        bool overflowed = false;
        std::vector<LiveViewport::ComponentEdit> edits;
        for (const CommandStack::ComponentEdit& edit : document.TakeComponentEdits(&overflowed)) edits.push_back({edit.entity, edit.component});
        // A value changed (a gizmo drag, a light's colour): patched into the running scene, which keeps the game's state. Anything else loads it again.
        if (!(structure == last_structure_ && !overflowed && viewport_.PatchScene(document.GetWorld(), edits))) {
            std::string error;
            if (viewport_.SetScene(SaveSceneToMemory(document.GetWorld()), &error)) {
                last_structure_ = structure;
            } else {
                note_ = error;
            }
        }
    }
    // A new size, kept for a moment: the picture is rebuilt at it, so the picture and the overlays share one aspect (a splitter drag asks for a new size every frame).
    u32 want_width = width, want_height = height;
    FitResolution(want_width, want_height);
    if (want_width != width_ || want_height != height_) {
        if (want_width == wanted_width_ && want_height == wanted_height_) {
            size_still_ += dt;
        } else {
            wanted_width_ = want_width;
            wanted_height_ = want_height;
            size_still_ = 0.0f;
        }
        if (size_still_ >= resize_delay) {
            size_still_ = 0.0f;
            if (!LoadAgain(document, width, height)) return false;
        }
    } else {
        wanted_width_ = want_width;
        wanted_height_ = want_height;
        size_still_ = 0.0f;
    }
    since_frame_ += dt;
    if (since_frame_ < 1.0f / std::max(max_fps, 1.0f)) return texture_.Valid();
    since_frame_ = 0.0f;
    const bool ortho = camera.projection == SceneViewProjection::Orthographic;
    if (!viewport_.Frame(0.0f, camera.Eye(), camera.Rotation(), camera.fov_degrees, ortho ? Projection::Orthographic : Projection::Perspective,
                         ortho ? camera.ortho_height : 0.0f)) {
        return texture_.Valid();
    }
    return texture_.Set(viewport_.Width(), viewport_.Height(), viewport_.Pixels());
}

void LiveSceneShading::Stop() {
    if (!running_) return;
    running_ = false;
    viewport_.Unload();
    note_.clear();
    cooked_.clear();
    scene_path_.clear();
    width_ = 0;
    height_ = 0;
    wanted_width_ = 0;
    wanted_height_ = 0;
    size_still_ = 0.0f;
    last_revision_ = 0;
    last_structure_ = 0;
    since_frame_ = 0.0f;
    since_reload_ = 0.0f;
}

} // namespace aether::editor
