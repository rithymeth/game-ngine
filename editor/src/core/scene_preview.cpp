#include "core/scene_preview.h"

#include "aether/assets/image.h"
#include "aether/cook/cooker.h"
#include "aether/cook/package.h"
#include "aether/platform/process.h"

#include <algorithm>
#include <chrono>
#include <system_error>

namespace aether::editor {

namespace stdfs = std::filesystem;

ScenePreview::ScenePreview(stdfs::path project_file) : project_file_(std::move(project_file)) {}

ScenePreview::~ScenePreview() { Wait(); }

stdfs::path ScenePreview::WorkDir() const { return project_file_.parent_path() / "Saved" / "Preview"; }

std::string ScenePreview::Message() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return message_;
}

void ScenePreview::SetMessage(const std::string& text) {
    std::lock_guard<std::mutex> lock(mutex_);
    message_ = text;
}

bool ScenePreview::Start(const std::string& scene, u32 width, u32 height) {
    if (state_.load() == State::Running) return false;
    if (worker_.joinable()) worker_.join(); // the previous render has finished (Done or Failed)
    state_.store(State::Running);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        image_ready_ = false;
        message_ = "Cooking the project";
    }
    width = std::clamp<u32>(width, 64, 3840);
    height = std::clamp<u32>(height, 64, 2160);
    worker_ = std::thread([this, scene, width, height] { Run(scene, width, height); });
    return true;
}

void ScenePreview::Wait() {
    if (worker_.joinable()) worker_.join();
}

bool ScenePreview::TakeImage(PreviewImage& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!image_ready_) return false;
    out = std::move(image_);
    image_ready_ = false;
    return true;
}

void ScenePreview::Run(std::string scene, u32 width, u32 height) {
    auto fail = [this](const std::string& why) {
        SetMessage(why);
        state_.store(State::Failed);
    };
    const stdfs::path player = player_path.empty() ? cook::FindPlayerExecutable() : player_path;
    if (player.empty() || !stdfs::exists(player)) return fail("The player (aether_player) was not found beside the editor, so there is nothing to render with.");

    const stdfs::path work = WorkDir();
    const stdfs::path cooked = work / "Cooked";
    std::error_code ec;
    stdfs::create_directories(cooked, ec);
    if (ec) return fail("Can't make " + work.string() + ": " + ec.message());

    cook::CookOptions options;
    options.project_file = project_file_;
    options.output_dir = cooked;
    options.configuration = cook::BuildConfiguration::Development;
    options.always_cook.push_back(scene); // the scene being previewed, whether or not the project starts with it
    const cook::CookReport report = cook::Cook(options);
    if (!report.ok) return fail("Cooking failed: " + (report.error.empty() ? std::string("see the log") : report.error));

    const stdfs::path picture = work / "preview.bmp";
    stdfs::remove(picture, ec);
    SetMessage("Rendering");
    platform::ChildProcess process;
    std::string error;
    const std::vector<std::string> args = {"--pak", cooked.string(), "--scene", scene, "--frames", std::to_string(frames),
                                           "--size", std::to_string(width) + "x" + std::to_string(height),
                                           "--screenshot", picture.string(), "--user-dir", (work / "User").string()};
    if (!process.Start(player, args, work, &error)) return fail("The player would not start: " + error);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds);
    while (process.Running()) {
        if (std::chrono::steady_clock::now() > deadline) {
            process.Kill();
            process.Wait();
            return fail("The render took longer than " + std::to_string(timeout_seconds) + " seconds and was stopped.");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    int exit_code = 0;
    process.Wait(&exit_code);
    if (exit_code != 0) return fail("The player exited with code " + std::to_string(exit_code) + " (is there a display or a Vulkan driver, and does the scene \"" + scene + "\" exist?)");

    if (!stdfs::exists(picture)) return fail("The player ran but wrote no picture (" + picture.string() + ").");
    assets::ImageData decoded;
    if (!assets::DecodeImageFile(picture.string(), decoded)) return fail("The picture the player wrote (" + picture.string() + ", " + std::to_string(stdfs::file_size(picture)) + " bytes) could not be read as an image.");
    {
        std::lock_guard<std::mutex> lock(mutex_);
        image_.width = decoded.width;
        image_.height = decoded.height;
        image_.rgba = std::move(decoded.pixels);
        image_ready_ = true;
        message_ = "Rendered " + scene;
    }
    state_.store(State::Done);
}

} // namespace aether::editor
