#pragma once

#include "aether/core/base.h"

#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace aether::editor {

// A rendered picture of a scene from the real renderer: the project is cooked (fast, cached), `aether_player` runs the scene for a
// few frames and writes a screenshot, and the image comes back as RGBA pixels. It is a still, made on demand in a couple of seconds,
// not a live viewport: the editor's own draw cannot show a lit scene (the scene renderer draws a cooked package through a running game),
// so this borrows the player's. It sees what is saved in the project, so save the scene first. Runs on a worker thread.
struct PreviewImage {
    u32 width = 0;
    u32 height = 0;
    std::vector<u8> rgba; // width * height * 4, top row first
};

class ScenePreview {
public:
    enum class State { Idle, Running, Done, Failed };

    explicit ScenePreview(std::filesystem::path project_file);
    ~ScenePreview(); // waits for a render in progress
    ScenePreview(const ScenePreview&) = delete;
    ScenePreview& operator=(const ScenePreview&) = delete;

    // Renders `scene` (a path relative to the project's Content folder) at `width` x `height`. False while a render is already running.
    bool Start(const std::string& scene, u32 width, u32 height);
    State GetState() const { return state_.load(); }
    // What it is doing, or why it failed.
    std::string Message() const;
    // The finished image, once: true the first time it is asked for after a render completes.
    bool TakeImage(PreviewImage& out);
    // Blocks until the render ends (tests, and closing the editor).
    void Wait();

    // Where the preview works, under the project: Saved/Preview.
    std::filesystem::path WorkDir() const;
    // Empty: the player beside the editor (or the one it was built with).
    std::filesystem::path player_path;
    // Frames the player runs before it takes the picture (the scene needs a few to settle: physics, scripts, lighting).
    u32 frames = 30;
    // A render that has not finished by now is killed.
    u32 timeout_seconds = 60;

private:
    void Run(std::string scene, u32 width, u32 height);
    void SetMessage(const std::string& text);

    std::filesystem::path project_file_;
    std::thread worker_;
    std::atomic<State> state_{State::Idle};
    mutable std::mutex mutex_;
    std::string message_;
    PreviewImage image_;
    bool image_ready_ = false;
};

} // namespace aether::editor
