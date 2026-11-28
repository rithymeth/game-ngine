#include "world/game_view_panel.h"

#include "core/scene_document.h"

#include <algorithm>

namespace aether::editor {

void GameViewPanel::SyncSession(const SceneDocument& document) {
    const u64 session_id = document.PlaySessionId();
    if (session_id_ == session_id) return;
    if (running_ && surface != nullptr) surface->Stop();
    running_ = false;
    start_failed_ = false;
    start_error_.clear();
    session_id_ = session_id;
}

void GameViewPanel::Draw(SceneDocument& document, f32 frame_dt) {
    SyncSession(document);
    const PlaySession::State play_state = document.PlayState();
    const bool playing = play_state != PlaySession::State::Editing;
    state_ = State{};
    state_.playing = playing;
    state_.paused = play_state == PlaySession::State::Paused;
    state_.simulating = document.IsSimulating();

    if (playing && !running_ && !start_failed_) {
        if (surface == nullptr) {
            state_.note = "The Game View needs the Vulkan editor: this build has no renderer.";
        } else if (!surface->Start(document, width, height)) {
            start_failed_ = true;
            start_error_ = surface->Note().empty() ? "The game could not be started." : surface->Note();
        } else {
            running_ = true;
            start_error_.clear();
        }
    }
    if (start_failed_ && playing) state_.note = start_error_; // the failure stands for the whole session
    if (running_ && surface != nullptr) {
        surface->Frame(document.PlayDelta(frame_dt)); // paused: zero, and the last picture stands; Step: exactly one frame
        if (surface->Picture().Valid()) state_.has_picture = true;
        else state_.note = surface->Note();
    }
    if (!playing && state_.note.empty()) state_.note = "The game is not running. Press Play (or Simulate) to see it from its own camera here.";

    const ImVec2 avail = ImGui::GetContentRegionAvail();
    ImGui::Dummy(avail); // the view fills the window; the picture is drawn over it
    if (!(avail.x > 0.0f) || !(avail.y > 0.0f)) return;
    const ImVec2 origin = ImGui::GetItemRectMin();
    const ImVec2 end(origin.x + avail.x, origin.y + avail.y);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(origin, end, IM_COL32(0, 0, 0, 255)); // the letterbox: the game's picture, nothing of the editor's behind it
    if (state_.has_picture && surface != nullptr) {
        const ImageTexture& picture = surface->Picture();
        const f32 scale = std::min(avail.x / static_cast<f32>(picture.Width()), avail.y / static_cast<f32>(picture.Height()));
        if (scale > 0.0f) {
            const ImVec2 size(static_cast<f32>(picture.Width()) * scale, static_cast<f32>(picture.Height()) * scale);
            state_.image_min = ImVec2(origin.x + (avail.x - size.x) * 0.5f, origin.y + (avail.y - size.y) * 0.5f);
            state_.image_max = ImVec2(state_.image_min.x + size.x, state_.image_min.y + size.y);
            draw->AddImage(picture.Ref(), state_.image_min, state_.image_max);
        }
    } else if (!state_.note.empty()) {
        const ImVec2 extent = ImGui::CalcTextSize(state_.note.c_str());
        draw->AddText(ImVec2(origin.x + (avail.x - extent.x) * 0.5f, origin.y + (avail.y - extent.y) * 0.5f), IM_COL32(235, 238, 245, 220),
                      state_.note.c_str());
    }
}

} // namespace aether::editor
