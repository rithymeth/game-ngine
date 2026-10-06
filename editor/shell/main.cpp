// The portable editor shell (Phase 24 step 5, docs/design/PHASE_SPECS.md
// §24.4): every tool editor (editor/src/workspace: Blueprint, Luau,
// Material, VFX, animation, AI, audio, UI, world, networking and debug
// tools) in a window (GLFW on Linux and macOS, Win32 on Windows) or
// offscreen, drawn with Dear ImGui through the RHI's Vulkan backend. The
// D3D12 editor (editor/main.cpp) is the Windows editor with the 3D viewport
// and hosts the same tools; this shell is what runs everywhere Vulkan does,
// lavapipe in CI included.
//
// Environment:
//   AETHER_EDITOR_HEADLESS=1     render offscreen (also when there's no display)
//   AETHER_EDITOR_MAX_FRAMES=N   quit after N frames
//   AETHER_EDITOR_SCREENSHOT=p   save the last frame as a PNG at p
//   AETHER_EDITOR_SIZE=WxH       the window or offscreen size (default 1600x960)
//   AETHER_EDITOR_TAB=name       the tool to open on (the start of its name:
//                                blueprint, material, particle, sound, ...)

#include "host/imgui_vulkan_host.h"
#include "host/imgui_window_input.h"
#include "workspace/editor_workspace.h"

#include "aether/core/log.h"
#include "aether/core/profiler.h"
#include "aether/gfx/rhi/device.h"
#include "aether/platform/window.h"

#include <imgui.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <string>
#include <vector>

using namespace aether;
using namespace aether::editor;
namespace rhi = aether::gfx::rhi;

namespace {

void ApplyEditorStyle() {
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 4.0f;
    style.FrameRounding = 3.0f;
    style.TabRounding = 3.0f;
    style.WindowPadding = ImVec2(8, 8);
    style.ItemSpacing = ImVec2(8, 5);
    ImVec4* colors = style.Colors;
    colors[ImGuiCol_TitleBgActive] = ImVec4(0.16f, 0.29f, 0.48f, 1.0f);
    colors[ImGuiCol_Tab] = ImVec4(0.18f, 0.22f, 0.28f, 1.0f);
    colors[ImGuiCol_TabSelected] = ImVec4(0.26f, 0.42f, 0.66f, 1.0f);
}

void DrawEditor(EditorWorkspace& workspace, const char* backend_label, bool& quit) {
    const ImGuiIO& io = ImGui::GetIO();
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Quit")) quit = true;
            ImGui::EndMenu();
        }
        workspace.DrawToolsMenu();
        ImGui::TextDisabled("   Aether Editor  -  %s  -  %zu tools", backend_label, workspace.ToolCount());
        ImGui::EndMainMenuBar();
    }

    const f32 top = ImGui::GetFrameHeight();
    const f32 side = io.DisplaySize.x > 1100 ? 420.0f : io.DisplaySize.x * 0.35f;
    const f32 main_w = io.DisplaySize.x - side;
    const f32 main_h = io.DisplaySize.y - top;
    constexpr ImGuiWindowFlags kFixed = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                                        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus;

    // Every tool: the list by category on the left, the selected one beside it.
    ImGui::SetNextWindowPos(ImVec2(0, top));
    ImGui::SetNextWindowSize(ImVec2(main_w, main_h));
    ImGui::Begin("Editors", nullptr, kFixed | ImGuiWindowFlags_NoTitleBar);
    workspace.DrawHubContents();
    ImGui::End();

    ImGui::SetNextWindowPos(ImVec2(main_w, top));
    ImGui::SetNextWindowSize(ImVec2(side, main_h * 0.5f));
    ImGui::Begin("Terrain", nullptr, kFixed);
    workspace.DrawTool(static_cast<usize>(workspace.FindTool("Terrain")));
    ImGui::End();

    ImGui::SetNextWindowPos(ImVec2(main_w, top + main_h * 0.5f));
    ImGui::SetNextWindowSize(ImVec2(side, main_h * 0.5f));
    ImGui::Begin("Debug", nullptr, kFixed);
    if (ImGui::BeginTabBar("##debug")) {
        for (const char* tool : {"Console", "Profiler"}) {
            if (ImGui::BeginTabItem(tool)) {
                workspace.DrawTool(static_cast<usize>(workspace.FindTool(tool)));
                ImGui::EndTabItem();
            }
        }
        ImGui::EndTabBar();
    }
    ImGui::End();

    workspace.DrawWindows();
}

bool HasDisplay() {
#if defined(_WIN32)
    return true;
#else
    return std::getenv("DISPLAY") != nullptr || std::getenv("WAYLAND_DISPLAY") != nullptr;
#endif
}

} // namespace

int main() {
    i32 max_frames = -1;
    if (const char* env = std::getenv("AETHER_EDITOR_MAX_FRAMES")) max_frames = std::atoi(env);
    const char* screenshot_path = std::getenv("AETHER_EDITOR_SCREENSHOT");
    bool headless = std::getenv("AETHER_EDITOR_HEADLESS") != nullptr || !HasDisplay();
    u32 width = 1600, height = 960;
    if (const char* env = std::getenv("AETHER_EDITOR_SIZE")) {
        unsigned w = 0, h = 0;
        if (std::sscanf(env, "%ux%u", &w, &h) == 2 && w >= 320 && h >= 240) width = w, height = h;
    }


    try {
        std::unique_ptr<Window> window;
        void* native_window = nullptr;
        if (!headless) {
            WindowDesc desc;
            desc.title = "Aether Editor";
            desc.width = width, desc.height = height;
            window = std::make_unique<Window>(desc);
            if (window->IsValid()) {
#if defined(_WIN32)
                native_window = window->NativeHandle();
#else
                native_window = window->PlatformWindow();
#endif
            } else {
                AETHER_LOG_WARN("Editor", "No window could be opened; rendering offscreen");
                window.reset();
                headless = true;
            }
        }

        std::unique_ptr<rhi::IDevice> device = rhi::CreateDevice(rhi::Backend::Vulkan, false);
        if (!device) {
            std::fprintf(stderr, "aether_editor_shell: no Vulkan device\n");
            return 1;
        }
        std::unique_ptr<rhi::ISwapChain> swap_chain = device->CreateSwapChain(native_window, width, height, 2);
        bool resized = false;
        if (window) {
            window->on_resize = [&](u32, u32) { resized = true; };
        }

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        // Shortcuts use Ctrl everywhere: the engine's key set has no Cmd key
        // yet, so ImGui's macOS Ctrl/Cmd swap would leave none reachable.
        io.ConfigMacOSXBehaviors = false;
        ApplyEditorStyle();
        ImGuiVulkanHost host(*device, *swap_chain);

        EditorWorkspace workspace;
        // AETHER_EDITOR_TAB picks the tool shown first (the start of its name, any case).
        if (const char* tab = std::getenv("AETHER_EDITOR_TAB")) {
            const i64 tool = workspace.FindTool(tab);
            if (tool >= 0) workspace.Select(static_cast<usize>(tool));
        }
        AETHER_LOG_INFO("Editor", "Aether editor shell: Vulkan, %s, %ux%u", headless ? "offscreen" : "window",
                        swap_chain->Width(), swap_chain->Height());
        const char* backend_label = headless ? "Vulkan (offscreen)" : "Vulkan";

        std::unique_ptr<rhi::ICommandList> cmd = device->CreateCommandList();
        u64 fence = 0;
        auto last = std::chrono::steady_clock::now();
        bool quit = false;
        for (i32 frame = 0; !quit && (max_frames < 0 || frame < max_frames); ++frame) {
            if (window) {
                if (!window->PumpMessages()) break;
                const std::vector<WindowEvent> events = window->TakeEvents();
                FeedImGuiEvents(events, io);
                if (window->IsMinimized()) continue;
                if (resized) {
                    device->WaitForFence(fence);
                    swap_chain->Resize(window->Width(), window->Height());
                    resized = false;
                }
            }
            const auto now = std::chrono::steady_clock::now();
            io.DeltaTime = std::max(std::chrono::duration<f32>(now - last).count(), 1.0f / 1000.0f);
            last = now;
            io.DisplaySize = ImVec2(static_cast<f32>(swap_chain->Width()), static_cast<f32>(swap_chain->Height()));

            Profiler::Get().BeginFrame();
            host.NewFrame();
            ImGui::NewFrame();
            workspace.Update(io.DeltaTime);
            DrawEditor(workspace, backend_label, quit);
            ImGui::Render();

            device->WaitForFence(fence); // one command list: the last frame must be done with it
            swap_chain->AcquireNextImage();
            cmd->Reset();
            cmd->BeginRenderPass(*swap_chain, {0.06f, 0.07f, 0.09f, 1.0f});
            host.Render(*cmd, ImGui::GetDrawData());
            cmd->EndRenderPass();
            cmd->Close();
            fence = device->Submit(*cmd, swap_chain.get());

            const bool last_frame = quit || (max_frames >= 0 && frame + 1 >= max_frames);
            if (last_frame && screenshot_path) {
                device->WaitForFence(fence);
                std::vector<u8> pixels;
                if (swap_chain->ReadBack(pixels)) {
                    const int w = static_cast<int>(swap_chain->Width()), h = static_cast<int>(swap_chain->Height());
                    if (stbi_write_png(screenshot_path, w, h, 4, pixels.data(), w * 4)) {
                        AETHER_LOG_INFO("Editor", "Saved a screenshot to %s", screenshot_path);
                    } else {
                        AETHER_LOG_ERROR("Editor", "Couldn't write %s", screenshot_path);
                    }
                } else {
                    AETHER_LOG_WARN("Editor", "This swap chain can't be read back; no screenshot");
                }
            }
            swap_chain->Present(true);
            Profiler::Get().EndFrame();
        }
        device->WaitForFence(fence);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "aether_editor_shell: %s\n", e.what());
        return 1;
    }
    if (ImGui::GetCurrentContext()) ImGui::DestroyContext();
    return 0;
}
