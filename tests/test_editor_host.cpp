#include "host/imgui_window_input.h"
#include "test_framework.h"

#if defined(AETHER_HAS_EDITOR_VULKAN)
#include "host/imgui_vulkan_host.h"
#endif

#include <imgui.h>

#include <cstdio>
#include <memory>
#include <stdexcept>
#include <vector>

// Phase 24 step 5: the editor's platform layer - window events driving
// Dear ImGui, and ImGui drawn through the Vulkan RHI (offscreen, so it runs
// on lavapipe in CI).

using namespace aether;
using namespace aether::editor;
using input::Key;

#define CHECK(...) AETHER_CHECK((__VA_ARGS__))

namespace {

struct ImGuiContextScope {
    ImGuiContextScope() {
        ImGui::CreateContext();
        ImGui::GetIO().ConfigMacOSXBehaviors = false; // tests press Ctrl on every platform
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(320, 240);
        io.DeltaTime = 1.0f / 60.0f;
        // Apply every queued event in the next frame (ImGui otherwise
        // spreads some across frames, which a live editor wants).
        io.ConfigInputTrickleEventQueue = false;
        // The font atlas is built by the renderer backend; build it here so
        // NewFrame works without one.
        unsigned char* pixels = nullptr;
        int w = 0, h = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
    }
    ~ImGuiContextScope() { ImGui::DestroyContext(); }
};

WindowEvent KeyEvent(Key key, bool down) {
    WindowEvent e;
    e.type = WindowEventType::Key, e.key = key, e.down = down;
    return e;
}

} // namespace

AETHER_TEST(EditorHost_KeysMapToImGui) {
    CHECK(ToImGuiKey(Key::A) == ImGuiKey_A && ToImGuiKey(Key::Z) == ImGuiKey_Z);
    CHECK(ToImGuiKey(Key::Num0) == ImGuiKey_0 && ToImGuiKey(Key::Num9) == ImGuiKey_9);
    CHECK(ToImGuiKey(Key::F1) == ImGuiKey_F1 && ToImGuiKey(Key::F12) == ImGuiKey_F12);
    CHECK(ToImGuiKey(Key::Enter) == ImGuiKey_Enter && ToImGuiKey(Key::Grave) == ImGuiKey_GraveAccent);
    CHECK(ToImGuiKey(Key::Left) == ImGuiKey_LeftArrow && ToImGuiKey(Key::RightCtrl) == ImGuiKey_RightCtrl);
    CHECK(ToImGuiKey(Key::GamepadA) == ImGuiKey_GamepadFaceDown);
    CHECK(ToImGuiKey(Key::MouseLeft) == ImGuiKey_None && ToImGuiKey(Key::MouseX) == ImGuiKey_None);
}

AETHER_TEST(EditorHost_WindowEventsDriveImGui) {
    ImGuiContextScope scope;
    ImGuiIO& io = ImGui::GetIO();
    WindowEvent move;
    move.type = WindowEventType::MouseMove, move.x = 40, move.y = 25, move.dx = 2, move.dy = 1;
    WindowEvent wheel;
    wheel.type = WindowEventType::Scroll, wheel.dy = -1;
    WindowEvent typed;
    typed.type = WindowEventType::Char, typed.codepoint = 'q';
    WindowEvent resize;
    resize.type = WindowEventType::Resize, resize.x = 640, resize.y = 480;
    const std::vector<WindowEvent> events = {move, KeyEvent(Key::MouseLeft, true), KeyEvent(Key::LeftCtrl, true),
                                             KeyEvent(Key::S, true), wheel, typed, resize};
    FeedImGuiEvents(events, io);
    ImGui::NewFrame();
    CHECK(io.MousePos.x == 40 && io.MousePos.y == 25);
    CHECK(ImGui::IsMouseDown(ImGuiMouseButton_Left) && !ImGui::IsMouseDown(ImGuiMouseButton_Right));
    CHECK(ImGui::IsKeyDown(ImGuiKey_S) && ImGui::IsKeyDown(ImGuiKey_LeftCtrl) && io.KeyCtrl && !io.KeyShift);
    CHECK(ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_S));
    CHECK(io.MouseWheel == -1);
    CHECK(io.InputQueueCharacters.Size == 1 && io.InputQueueCharacters[0] == 'q');
    CHECK(io.DisplaySize.x == 640 && io.DisplaySize.y == 480);
    ImGui::EndFrame();

    // Releasing the right-hand key keeps Ctrl held while the left one is.
    FeedImGuiEvents(std::vector<WindowEvent>{KeyEvent(Key::RightCtrl, true), KeyEvent(Key::RightCtrl, false),
                                             KeyEvent(Key::S, false), KeyEvent(Key::MouseLeft, false)},
                    io);
    ImGui::NewFrame();
    CHECK(io.KeyCtrl && !ImGui::IsKeyDown(ImGuiKey_S) && !ImGui::IsMouseDown(ImGuiMouseButton_Left));
    ImGui::EndFrame();
    FeedImGuiEvents(std::vector<WindowEvent>{KeyEvent(Key::LeftCtrl, false)}, io);
    ImGui::NewFrame();
    CHECK(!io.KeyCtrl);
    ImGui::EndFrame();
}

#if defined(AETHER_HAS_EDITOR_VULKAN)
AETHER_TEST(EditorHost_ImGuiDrawsThroughVulkan) {
    namespace rhi = gfx::rhi;
    std::unique_ptr<rhi::IDevice> device;
    try {
        device = rhi::CreateDevice(rhi::Backend::Vulkan, false);
    } catch (const std::exception&) {
    }
    if (!device) {
        std::printf("    (no Vulkan device: install a driver, or Mesa's lavapipe, to run this)\n");
        return;
    }
    constexpr u32 kWidth = 200, kHeight = 120;
    std::unique_ptr<rhi::ISwapChain> target = device->CreateSwapChain(nullptr, kWidth, kHeight, 2);
    ImGui::CreateContext();
    ImGui::GetIO().ConfigMacOSXBehaviors = false; // tests press Ctrl on every platform
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    {
        ImGuiVulkanHost host(*device, *target);
        std::unique_ptr<rhi::ICommandList> cmd = device->CreateCommandList();
        std::vector<u8> pixels;
        for (int frame = 0; frame < 2; ++frame) {
            io.DisplaySize = ImVec2(kWidth, kHeight);
            io.DeltaTime = 1.0f / 60.0f;
            host.NewFrame();
            ImGui::NewFrame();
            // A solid pure-red rectangle on the left half, and some text.
            ImGui::GetForegroundDrawList()->AddRectFilled(ImVec2(0, 0), ImVec2(100, 120), IM_COL32(255, 0, 0, 255));
            ImGui::SetNextWindowPos(ImVec2(110, 10));
            ImGui::SetNextWindowSize(ImVec2(80, 100));
            ImGui::Begin("Panel");
            ImGui::Text("Aether");
            ImGui::End();
            ImGui::Render();
            target->AcquireNextImage();
            cmd->Reset();
            cmd->BeginRenderPass(*target, {0, 0, 0, 1});
            host.Render(*cmd, ImGui::GetDrawData());
            cmd->EndRenderPass();
            cmd->Close();
            device->WaitForFence(device->Submit(*cmd, target.get()));
            CHECK(target->ReadBack(pixels));
            target->Present(false);
        }
        const auto at = [&](u32 x, u32 y) { return &pixels[(y * kWidth + x) * 4]; };
        // ImGui's top-left origin lands at the image's top-left.
        CHECK(at(10, 10)[0] == 255 && at(10, 10)[1] == 0 && at(90, 110)[0] == 255);
        // The window draws something other than the clear colour on the right.
        u32 lit = 0;
        for (u32 y = 10; y < 110; ++y) {
            for (u32 x = 110; x < 190; ++x) lit += at(x, y)[0] + at(x, y)[1] + at(x, y)[2] > 30 ? 1 : 0;
        }
        CHECK(lit > 1000);
        CHECK(at(105, 60)[0] == 0 && at(195, 115)[2] == 0); // outside both: the clear colour
    }
    ImGui::DestroyContext();
}
#endif
