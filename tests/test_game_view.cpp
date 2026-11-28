#include "world/game_view_panel.h"

#include "core/scene_document.h"
#include "ui/image_texture.h"
#include "test_framework.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <string>
#include <vector>

// The Game View: the running game's own picture while the document plays, started and stopped with the session, fitted to the window with
// letterboxing. Drawn headless, over a fake surface that records what the panel asks of it.

using namespace aether;
using namespace aether::editor;

namespace {

class HeadlessImGui {
public:
    HeadlessImGui() {
        context_ = ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(1000, 700);
        io.DeltaTime = 1.0f / 60.0f;
        unsigned char* pixels = nullptr;
        int w = 0, h = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
    }
    ~HeadlessImGui() { ImGui::DestroyContext(context_); }
    void Frame(const std::function<void()>& body) {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(1000, 700));
        ImGui::Begin("Game", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar);
        body();
        ImGui::End();
        ImGui::Render();
    }

private:
    ImGuiContext* context_ = nullptr;
};

// A surface that records the calls the panel makes; `picture_ok` gives frames a 320x180 picture (the panel needs a valid one to draw it).
struct FakeGameView : GameViewSurface {
    bool start_ok = true;
    bool picture_ok = true;
    std::string note_text = "no project";
    int starts = 0;
    int stops = 0;
    int frames = 0;
    f32 last_play_dt = -1.0f;
    u32 start_width = 0;
    u32 start_height = 0;
    ImageTexture picture;

    bool Start(const SceneDocument&, u32 width, u32 height) override {
        ++starts;
        start_width = width;
        start_height = height;
        return start_ok;
    }
    bool Frame(f32 play_dt) override {
        ++frames;
        last_play_dt = play_dt;
        if (picture_ok) picture.Set(320, 180, std::vector<u8>(320u * 180u * 4u, 96));
        return true;
    }
    const ImageTexture& Picture() const override { return picture; }
    const std::string& Note() const override { return note_text; }
    void Stop() override { ++stops; }
};

} // namespace

AETHER_TEST(GameView_NotPlayingSaysHowToSeeTheGame) {
    HeadlessImGui imgui;
    SceneDocument document;
    document.NewScene();
    FakeGameView game;
    GameViewPanel panel;
    panel.surface = &game;
    imgui.Frame([&] { panel.Draw(document, 1.0f / 60.0f); });
    AETHER_CHECK(!panel.LastState().playing);
    AETHER_CHECK(!panel.LastState().has_picture);
    AETHER_CHECK(!panel.Running());
    AETHER_CHECK(game.starts == 0 && game.frames == 0); // nothing is cooked or run outside a session
    AETHER_CHECK(panel.LastState().note.find("Play") != std::string::npos);
}

AETHER_TEST(GameView_StartsWithTheSessionAndFollowsItPausedAndStepped) {
    HeadlessImGui imgui;
    SceneDocument document;
    document.NewScene();
    FakeGameView game;
    GameViewPanel panel;
    panel.surface = &game;
    panel.width = 160;
    panel.height = 90;
    document.Play();
    imgui.Frame([&] { panel.Draw(document, 1.0f / 60.0f); });
    AETHER_CHECK(panel.LastState().playing && !panel.LastState().paused && !panel.LastState().simulating);
    AETHER_CHECK(panel.Running() && panel.LastState().has_picture && panel.LastState().note.empty());
    AETHER_CHECK(game.starts == 1 && game.frames == 1);
    AETHER_CHECK(game.start_width == 160 && game.start_height == 90); // the session's resolution, fixed while it runs
    AETHER_CHECK(game.last_play_dt > 0.0f && game.last_play_dt <= 0.1f); // the frame's play time, never more than 0.1 s
    imgui.Frame([&] { panel.Draw(document, 1.0f / 60.0f); });
    AETHER_CHECK(game.starts == 1 && game.frames == 2);
    AETHER_CHECK(game.last_play_dt > 0.0f);

    document.Pause();
    imgui.Frame([&] { panel.Draw(document, 1.0f / 60.0f); });
    AETHER_CHECK(panel.LastState().paused && panel.LastState().playing);
    AETHER_CHECK(game.frames == 3 && game.last_play_dt == 0.0f); // paused: the game gets no time, the last picture stands
    AETHER_CHECK(panel.LastState().has_picture);

    document.Step();
    imgui.Frame([&] { panel.Draw(document, 1.0f / 60.0f); });
    AETHER_CHECK(game.last_play_dt > 0.0f); // exactly one simulation frame
    imgui.Frame([&] { panel.Draw(document, 1.0f / 60.0f); });
    AETHER_CHECK(game.last_play_dt == 0.0f);

    document.Simulate(); // while paused this is Resume: the session stays the Play it was
    imgui.Frame([&] { panel.Draw(document, 1.0f / 60.0f); });
    AETHER_CHECK(!panel.LastState().simulating && !panel.LastState().paused);
    AETHER_CHECK(game.starts == 1 && game.stops == 0);
    document.Stop();
    imgui.Frame([&] { panel.Draw(document, 1.0f / 60.0f); });
    AETHER_CHECK(game.stops == 1);
}

AETHER_TEST(GameView_StopsWithTheSessionAndStartsAFreshOneNextTime) {
    HeadlessImGui imgui;
    SceneDocument document;
    document.NewScene();
    FakeGameView game;
    GameViewPanel panel;
    panel.surface = &game;
    document.Play();
    imgui.Frame([&] { panel.Draw(document, 1.0f / 60.0f); });
    AETHER_CHECK(panel.Running() && game.starts == 1);
    document.Stop();
    imgui.Frame([&] { panel.Draw(document, 1.0f / 60.0f); });
    AETHER_CHECK(!panel.Running() && game.stops == 1);
    AETHER_CHECK(!panel.LastState().playing && !panel.LastState().has_picture);
    AETHER_CHECK(panel.LastState().note.find("not running") != std::string::npos);
    document.Play();
    imgui.Frame([&] { panel.Draw(document, 1.0f / 60.0f); });
    AETHER_CHECK(panel.Running() && game.starts == 2); // a new session starts a new game
    AETHER_CHECK(panel.LastState().has_picture && game.stops == 1);
}

AETHER_TEST(GameView_StopsWhileHiddenWithoutDrawing) {
    HeadlessImGui imgui;
    SceneDocument document;
    document.NewScene();
    FakeGameView game;
    GameViewPanel panel;
    panel.surface = &game;
    const auto draw = [&] { imgui.Frame([&] { panel.Draw(document, 1.0f / 60.0f); }); };

    document.Play();
    panel.SyncSession(document);
    AETHER_CHECK(game.starts == 0 && game.frames == 0); // hidden views do not start or render
    draw();
    AETHER_CHECK(panel.Running() && game.starts == 1 && game.frames == 1);
    document.Pause();
    panel.SyncSession(document);
    AETHER_CHECK(panel.Running() && game.stops == 0 && game.frames == 1);

    document.Stop(); // the host is drawing another tool
    panel.SyncSession(document);
    AETHER_CHECK(!panel.Running() && game.stops == 1 && game.frames == 1);
    panel.SyncSession(document);
    AETHER_CHECK(game.stops == 1); // no duplicate teardown on later frames
    draw();
    AETHER_CHECK(!panel.LastState().has_picture && game.stops == 1);
}

AETHER_TEST(GameView_RestartsWhenStopAndPlayHappenWhileItIsHidden) {
    HeadlessImGui imgui;
    SceneDocument document;
    document.NewScene();
    FakeGameView game;
    GameViewPanel panel;
    panel.surface = &game;
    const auto draw = [&] { imgui.Frame([&] { panel.Draw(document, 1.0f / 60.0f); }); };

    document.Play();
    draw();
    AETHER_CHECK(panel.Running() && game.starts == 1 && game.stops == 0);
    document.Stop(); // another editor tool is visible; Game View never sees the Editing frame
    document.Simulate();
    draw();
    AETHER_CHECK(panel.Running() && panel.LastState().simulating && game.starts == 2 && game.stops == 1);
}

AETHER_TEST(GameView_RetriesAFailedStartAfterAHiddenRestart) {
    HeadlessImGui imgui;
    SceneDocument document;
    document.NewScene();
    FakeGameView game;
    game.start_ok = false;
    GameViewPanel panel;
    panel.surface = &game;
    const auto draw = [&] { imgui.Frame([&] { panel.Draw(document, 1.0f / 60.0f); }); };

    document.Play();
    draw();
    AETHER_CHECK(!panel.Running() && game.starts == 1);
    document.Stop();
    game.start_ok = true;
    document.Play();
    draw();
    AETHER_CHECK(panel.Running() && panel.LastState().has_picture && game.starts == 2 && game.stops == 0);
}

AETHER_TEST(GameView_ASimulateSessionIsReported) {
    HeadlessImGui imgui;
    SceneDocument document;
    document.NewScene();
    FakeGameView game;
    GameViewPanel panel;
    panel.surface = &game;
    document.Simulate();
    imgui.Frame([&] { panel.Draw(document, 1.0f / 60.0f); });
    AETHER_CHECK(panel.LastState().playing && panel.LastState().simulating && !panel.LastState().paused);
    AETHER_CHECK(panel.LastState().has_picture);
}

AETHER_TEST(GameView_AStartThatFailsSaysWhyOnceAndIsNotRetriedEveryFrame) {
    HeadlessImGui imgui;
    SceneDocument document;
    document.NewScene();
    FakeGameView game;
    game.start_ok = false;
    GameViewPanel panel;
    panel.surface = &game;
    document.Play();
    imgui.Frame([&] { panel.Draw(document, 1.0f / 60.0f); });
    imgui.Frame([&] { panel.Draw(document, 1.0f / 60.0f); });
    AETHER_CHECK(game.starts == 1 && game.frames == 0); // cooking is not repeated while the same session fails
    AETHER_CHECK(!panel.Running() && !panel.LastState().has_picture);
    AETHER_CHECK(panel.LastState().note == "no project");
    document.Stop();
    imgui.Frame([&] { panel.Draw(document, 1.0f / 60.0f); });
    document.Play();
    imgui.Frame([&] { panel.Draw(document, 1.0f / 60.0f); });
    AETHER_CHECK(game.starts == 2); // the next session tries again
}

AETHER_TEST(GameView_WithoutASurfaceSaysTheVulkanEditorIsNeeded) {
    HeadlessImGui imgui;
    SceneDocument document;
    document.NewScene();
    GameViewPanel panel; // no surface: a portable or headless build
    document.Play();
    imgui.Frame([&] { panel.Draw(document, 1.0f / 60.0f); });
    AETHER_CHECK(!panel.Running() && !panel.LastState().has_picture);
    AETHER_CHECK(panel.LastState().note.find("Vulkan") != std::string::npos);
}

AETHER_TEST(GameView_FitsThePictureWithLetterboxing) {
    HeadlessImGui imgui;
    SceneDocument document;
    document.NewScene();
    FakeGameView game;
    GameViewPanel panel;
    panel.surface = &game;
    document.Play();
    ImVec2 origin{}, avail{};
    imgui.Frame([&] {
        origin = ImGui::GetCursorScreenPos();
        avail = ImGui::GetContentRegionAvail();
        panel.Draw(document, 1.0f / 60.0f);
    });
    const GameViewPanel::State& state = panel.LastState();
    AETHER_CHECK(state.has_picture);
    const f32 width = state.image_max.x - state.image_min.x;
    const f32 height = state.image_max.y - state.image_min.y;
    const f32 scale = std::min(avail.x / 320.0f, avail.y / 180.0f);
    AETHER_CHECK(std::fabs(width - 320.0f * scale) < 0.5f && std::fabs(height - 180.0f * scale) < 0.5f); // the picture's own aspect
    AETHER_CHECK(std::fabs(state.image_min.x - origin.x) < 0.5f); // no side bars: the picture is as wide as the view asks
    const f32 top = state.image_min.y - origin.y;
    const f32 bottom = (origin.y + avail.y) - state.image_max.y;
    AETHER_CHECK(top > 1.0f && std::fabs(top - bottom) < 0.5f); // equal bars above and below
}
