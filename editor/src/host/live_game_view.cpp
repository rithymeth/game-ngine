#include "host/live_game_view.h"

#include "aether/scene/serialization.h"
#include "aether/player/game.h"
#include "core/scene_preview.h"
#include "host/preview_assets.h"

namespace aether::editor {

LiveGameView::LiveGameView(rhi::IDevice& device, ScenePanel& scene, std::function<std::filesystem::path()> project_file)
    : scene_(scene), project_file_(std::move(project_file)), viewport_(device) {}

LiveGameView::~LiveGameView() = default;

bool LiveGameView::Start(const SceneDocument& document, u32 width, u32 height) {
    Stop(); // a new session must not draw from an old viewport or show its picture
    const std::filesystem::path project = project_file_ ? project_file_() : std::filesystem::path();
    if (project.empty()) {
        note_ = "No project is open.";
        return false;
    }
    // The scene is run as it is when the session starts, so the game the person sees is the game the session started.
    const std::vector<u8> bytes = SaveSceneToMemory(document.GetWorld());
    std::filesystem::path cooked;
    std::string error;
    if (!CookForPreview(project, scene_.Path(), project.parent_path() / "Saved" / "GameView", &cooked, &error,
                        PreviewAssets(document, scene_.ContentRoot())) ||
        !viewport_.Load(cooked, scene_.Path(), width, height, &error, &bytes)) {
        note_ = error;
        return false;
    }
    viewport_.BeginPlay();
    play_session_id_ = document.PlaySessionId();
    note_.clear();
    return true;
}

bool LiveGameView::Frame(f32 play_dt) {
    if (!viewport_.Loaded()) return false;
    if (!(play_dt > 0.0f) && has_picture_) return false; // paused: keep this session's last picture
    if (!viewport_.FrameGameCamera(play_dt)) return false;
    if (!texture_.Set(viewport_.Width(), viewport_.Height(), viewport_.Pixels())) return false;
    has_picture_ = true;
    return true;
}

const ImageTexture& LiveGameView::Picture() const {
    static const ImageTexture empty;
    return has_picture_ ? texture_ : empty;
}

EditorWorkspace::GameplaySource LiveGameView::GameplaySource(const SceneDocument& document) const {
    if (play_session_id_ == 0 || document.PlaySessionId() != play_session_id_) return {};
    const auto* game = viewport_.GameForDebugging();
    if (!game) return {};
    const auto runtime = game->GameplayForDebugging();
    return {runtime.world, runtime.attributes, runtime.effects, runtime.abilities, runtime.effect_library,
            runtime.ability_library, runtime.world_generation};
}

void LiveGameView::Stop() {
    play_session_id_ = 0;
    has_picture_ = false;
    viewport_.Unload();
    note_.clear();
}

} // namespace aether::editor
