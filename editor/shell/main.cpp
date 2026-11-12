// The portable editor shell (Phase 24 step 5
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
#include "host/live_viewport_panel.h"
#include "host/imgui_window_input.h"
#include "workspace/editor_workspace.h"

#include "aether/core/log.h"
#include "aether/core/profiler.h"
#include "aether/gfx/rhi/device.h"
#include "aether/platform/window.h"

#include <imgui.h>
#include <imgui_internal.h> // the dock builder: the default arrangement

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include "ui/editor_theme.h"
#include "ui/extensions.h"
#include "ui/shell_layout.h"

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

// The default arrangement, built once when there is no saved one (or after View > Reset layout): the tools
// take the left and middle, the Terrain tool the top right, the console and profiler under it.
void BuildDefaultDockLayout(ImGuiID dockspace, const ImVec2& size) {
    ImGui::DockBuilderRemoveNode(dockspace);
    ImGui::DockBuilderAddNode(dockspace, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspace, size);
    ImGuiID main = dockspace;
    const ImGuiID side = ImGui::DockBuilderSplitNode(main, ImGuiDir_Right, size.x > 1100.0f ? 420.0f / size.x : 0.3f, nullptr, &main);
    ImGuiID side_top = side;
    const ImGuiID side_bottom = ImGui::DockBuilderSplitNode(side_top, ImGuiDir_Down, 0.5f, nullptr, &side_top);
    ImGui::DockBuilderDockWindow("Editors", main);
    ImGui::DockBuilderDockWindow("Terrain", side_top);
    ImGui::DockBuilderDockWindow("Debug", side_bottom);
    ImGui::DockBuilderFinish(dockspace);
}

void DrawEditor(EditorWorkspace& workspace, const char* backend_label, bool& quit) {
    const ImGuiIO& io = ImGui::GetIO();
    bool reset_layout = false;
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            workspace.DrawSceneFileItems();
            ImGui::Separator();
            if (ImGui::MenuItem("Quit")) quit = true;
            ImGui::EndMenu();
        }
        workspace.DrawEditMenu();
        workspace.DrawToolsMenu();
        if (ImGui::BeginMenu("View")) {
            if (ImGui::MenuItem("Reset layout")) reset_layout = true;
            ImGui::EndMenu();
        }
        ImGui::TextDisabled("   Aether Editor  -  %s  -  %zu tools", backend_label, workspace.ToolCount());
        ImGui::EndMainMenuBar();
    }

    // The status bar claims the strip along the bottom first, so the dockspace fills what is left.
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    if (ImGui::BeginViewportSideBar("##status", viewport, ImGuiDir_Down, ImGui::GetFrameHeight(), ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::TextDisabled("%s", backend_label);
        ImGui::SameLine();
        ImGui::TextDisabled("|  %.1f ms  (%.0f fps)  |  drag a panel's tab to dock it, or its edge to resize", 1000.0f / std::max(io.Framerate, 1.0f), io.Framerate);
    }
    ImGui::End();

    const ImGuiID dockspace = ImGui::DockSpaceOverViewport(0, viewport);
    const ImGuiDockNode* node = ImGui::DockBuilderGetNode(dockspace);
    const bool empty = node == nullptr || (node->IsLeafNode() && node->Windows.Size == 0); // nothing saved: the first run
    if ((reset_layout || empty) && viewport->WorkSize.x > 1.0f && viewport->WorkSize.y > 1.0f) BuildDefaultDockLayout(dockspace, viewport->WorkSize);

    constexpr ImGuiWindowFlags kPanel = ImGuiWindowFlags_NoCollapse;
    // Every tool: the list by category on the left, the selected one beside it.
    if (ImGui::Begin("Editors", nullptr, kPanel)) workspace.DrawHubContents();
    ImGui::End();

    if (ImGui::Begin("Terrain", nullptr, kPanel)) workspace.DrawTool(static_cast<usize>(workspace.FindTool("Terrain")));
    ImGui::End();

    if (ImGui::Begin("Debug", nullptr, kPanel) && ImGui::BeginTabBar("##debug")) {
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
        io.IniFilename = nullptr; // set below, once it is known whether this run remembers anything
        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
        // Shortcuts use Ctrl everywhere: the engine's key set has no Cmd key
        // yet, so ImGui's macOS Ctrl/Cmd swap would leave none reachable.
        io.ConfigMacOSXBehaviors = false;
        ApplyEditorTheme();
        ImGuiVulkanHost host(*device, *swap_chain);

        EditorWorkspace workspace;
        // The Live Viewport tool: the project's scene running under the real renderer, drawn on this same device. It is made here, not in the
        // workspace, because the workspace has no graphics device (it also runs headless in tests).
        LiveViewportPanel live(*device, workspace.Scene(), [&workspace] { return workspace.ProjectFile(); });
        f32 frame_dt = 0.0f;
        workspace.Extensions().AddPanel({"Live Viewport", "Scene", [&live, &frame_dt] { live.Draw(frame_dt); }, "shell"});
        // AETHER_EDITOR_TAB picks the tool shown first (the start of its name, any case).
        // What the last session left: the divider and the open tool. A headless run (tests, screenshots) neither
        // reads nor writes it unless AETHER_EDITOR_LAYOUT names a file; AETHER_EDITOR_TAB wins over the remembered tool.
        ShellLayout layout;
        const std::filesystem::path layout_file = (headless && !std::getenv("AETHER_EDITOR_LAYOUT")) ? std::filesystem::path() : DefaultShellLayoutPath();
        // Where the panels are docked is ImGui's own ini, kept beside the layout file.
        static std::string imgui_ini;
        if (!layout_file.empty()) {
            imgui_ini = (layout_file.parent_path() / "editor_panels.ini").string();
            io.IniFilename = imgui_ini.c_str();
        }
        // The tool to open on: the one asked for, else the one left open last time. A tool an extension adds (the Live Viewport is one) only
        // exists after the workspace's first frame, so a name not found yet is tried again for a few frames; until then the layout is not saved,
        // or the remembered tool would be overwritten by the default one.
        std::string pending_tool;
        i32 pending_frames = 0;
        if (LoadShellLayout(layout_file, layout) && !layout.tool.empty()) pending_tool = layout.tool;
        if (const char* tab = std::getenv("AETHER_EDITOR_TAB")) pending_tool = tab;
        auto select_pending = [&] {
            if (pending_tool.empty()) return;
            const i64 tool = workspace.FindTool(pending_tool);
            if (tool >= 0) {
                workspace.Select(static_cast<usize>(tool));
                pending_tool.clear();
            } else if (++pending_frames > 5) {
                pending_tool.clear(); // no such tool: give up and keep the default
            }
        };
        select_pending();
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
            frame_dt = std::max(std::chrono::duration<f32>(now - last).count(), 1.0f / 1000.0f);
            io.DeltaTime = frame_dt;
            last = now;
            io.DisplaySize = ImVec2(static_cast<f32>(swap_chain->Width()), static_cast<f32>(swap_chain->Height()));

            Profiler::Get().BeginFrame();
            host.NewFrame();
            ImGui::NewFrame();
            workspace.Update(io.DeltaTime);
            select_pending();
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
            if (frame % 120 == 0 && !layout_file.empty() && pending_tool.empty()) { // not only at exit: a crash should not lose the divider
                layout.tool = workspace.ToolName(workspace.Selected());
                SaveShellLayout(layout_file, layout);
            }
            Profiler::Get().EndFrame();
        }
        device->WaitForFence(fence);
        if (!layout_file.empty() && pending_tool.empty()) {
            layout.tool = workspace.ToolName(workspace.Selected());
            std::string layout_error;
            if (!SaveShellLayout(layout_file, layout, &layout_error)) AETHER_LOG_WARN("Editor", "Couldn't remember the layout: %s", layout_error.c_str());
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "aether_editor_shell: %s\n", e.what());
        return 1;
    }
    if (ImGui::GetCurrentContext()) ImGui::DestroyContext();
    return 0;
}
