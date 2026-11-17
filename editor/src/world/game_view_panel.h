#pragma once

#include "core/play_session.h"
#include "ui/image_texture.h"

#include <imgui.h>

#include <string>

namespace aether::editor {

class SceneDocument;

// Where the Game View's picture comes from: the running game drawn from its own camera. Implemented where a graphics device exists
// (editor/src/host/live_game_view.h, over a LiveViewport); the portable panel only asks for a picture and a note, so it builds and runs
// headless, without Vulkan.
class GameViewSurface {
public:
    virtual ~GameViewSurface() = default;
    // Cooks what the scene needs and starts it at width x height, as the document is when the play session starts. False, with Note() set.
    virtual bool Start(const SceneDocument& document, u32 width, u32 height) = 0;
    // Advances the running game by `play_dt` (the document's PlayDelta: zero while paused) and draws it from its own camera. True if a
    // new picture was read back; the last one stands when nothing moved.
    virtual bool Frame(f32 play_dt) = 0;
    virtual const ImageTexture& Picture() const = 0;
    // Why there is no picture, or what is wrong.
    virtual const std::string& Note() const = 0;
    // Stops the running game and frees the picture (the play session ended).
    virtual void Stop() = 0;
};

// The Game View tool: the game's own picture while the document plays or is paused -- the active Camera entity wherever the game's
// scripts, physics or animation have taken it, drawn by the real renderer -- with no editor chrome and no editor camera. It starts with the
// play session and stops with it, so it always shows this session's game; the Scene View stays the editor's own view, the other half of
// Unreal's Scene/Game pair. Fills the current window with the picture, letterboxed to its aspect, or with a note saying why there is none.
class GameViewPanel {
public:
    // Not owned; without one the view says the Vulkan editor is needed.
    GameViewSurface* surface = nullptr;
    // The picture's resolution while playing: fixed for the session (the game's swap chain is made with it) and fitted to the window.
    u32 width = 1280;
    u32 height = 720;

    // `frame_dt` is the frame's real time; the game gets the document's play time for it (zero while paused, one frame after Step).
    void Draw(SceneDocument& document, f32 frame_dt);

    // What the last draw did, for a host or a test without the pixels.
    struct State {
        bool playing = false; // the document is playing or paused
        bool paused = false;
        bool simulating = false; // this session was started by Simulate
        bool has_picture = false;
        ImVec2 image_min{}, image_max{}; // the picture's rectangle on screen
        std::string note;                // why it is not there or what went wrong
    };
    const State& LastState() const { return state_; }
    // Whether the game is running here: started for this play session and not stopped.
    bool Running() const { return running_; }

private:
    State state_;
    bool running_ = false;
    bool start_failed_ = false; // this session's start failed already; do not cook again on every frame
    std::string start_error_;   // why it failed, said on every draw of the session
};

} // namespace aether::editor
