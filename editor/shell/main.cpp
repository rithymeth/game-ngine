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
//   AETHER_PROJECT=file          the .aproject to open (a command-line argument
//                                wins); without either the Project Hub is shown

#include "host/imgui_vulkan_host.h"
#include "host/live_viewport_panel.h"
#include "host/imgui_window_input.h"
#include "workspace/editor_workspace.h"

#include "aether/core/log.h"
#include "aether/core/profiler.h"
#include "aether/gfx/rhi/device.h"
#include "aether/platform/window.h"

#include <imgui.h>
#include <imgui_internal.h> // the dock builder: the built-in arrangements

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include "aether/project/project.h"
#include "ui/editor_theme.h"
#include "ui/extensions.h"
#include "ui/shell_layout.h"
#include "workspace/layout_presets.h"

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

// The arrangements Window > Layout offers under LayoutPresets::BuiltinNames(), each a docking of the three windows
// this shell owns: the tool list and editor ("Editors"), the Terrain tool, and the console/profiler ("Debug").
void BuildDockLayout(ImGuiID dockspace, const ImVec2& size, const std::string& preset) {
    ImGui::DockBuilderRemoveNode(dockspace);
    ImGui::DockBuilderAddNode(dockspace, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspace, size);
    ImGuiID main = dockspace;
    ImGuiID top_right = 0;
    ImGuiID bottom = 0;
    if (preset == "Level design") {
        // The editor takes the room a level needs; the side panels narrow to a quarter.
        const ImGuiID side = ImGui::DockBuilderSplitNode(main, ImGuiDir_Right, 0.24f, nullptr, &main);
        top_right = side;
        bottom = ImGui::DockBuilderSplitNode(side, ImGuiDir_Down, 0.5f, nullptr, &top_right);
    } else if (preset == "Scripting") {
        // The editor in the centre, a small scene at the top right, Output large below it.
        const ImGuiID side = ImGui::DockBuilderSplitNode(main, ImGuiDir_Right, size.x > 1100.0f ? 420.0f / size.x : 0.3f, nullptr, &main);
        top_right = side;
        bottom = ImGui::DockBuilderSplitNode(side, ImGuiDir_Down, 0.65f, nullptr, &top_right);
    } else if (preset == "Animation") {
        // The editor in the centre, the timeline along the bottom, Details on the right.
        bottom = ImGui::DockBuilderSplitNode(main, ImGuiDir_Down, 0.28f, nullptr, &main);
        top_right = ImGui::DockBuilderSplitNode(main, ImGuiDir_Right, 0.3f, nullptr, &main);
    } else { // Default: the tools left and middle, Terrain top right, the console and profiler under it
        const ImGuiID side = ImGui::DockBuilderSplitNode(main, ImGuiDir_Right, size.x > 1100.0f ? 420.0f / size.x : 0.3f, nullptr, &main);
        top_right = side;
        bottom = ImGui::DockBuilderSplitNode(side, ImGuiDir_Down, 0.5f, nullptr, &top_right);
    }
    ImGui::DockBuilderDockWindow("Editors", main);
    ImGui::DockBuilderDockWindow("Terrain", top_right);
    ImGui::DockBuilderDockWindow("Debug", bottom);
    ImGui::DockBuilderFinish(dockspace);
}

// What Window > Layout is showing and what it still has to apply: a saved preset's ini is loaded before the next
// NewFrame, a built-in arrangement is rebuilt once the dockspace exists, and Save Layout As captures the current one.
struct LayoutState {
    LayoutPresets presets;
    std::filesystem::path preset_project; // the project the presets folder belongs to; empty = the user config folder
    std::string active = "Default";       // the name the menu draws with a mark
    std::string apply_builtin;            // a built-in arrangement to rebuild after DockSpaceOverViewport
    std::string apply_text;               // a saved preset's ini, loaded before the next frame
    bool open_save_popup = false;
    char save_name[64] = {};
    std::string save_error;
};

// Layouts are per project (docs/design/EDITOR_UI.md §4.2: Saved/Layouts), so opening another project switches the
// folder too. Without a project they live in the per-user config folder, as the remembered layout itself does.
void SyncPresetFolder(LayoutState& state, EditorWorkspace& workspace) {
    if (state.preset_project == workspace.ProjectFile()) return;
    state.preset_project = workspace.ProjectFile();
    state.presets = state.preset_project.empty() ? LayoutPresets()
                                                 : LayoutPresets(ProjectPaths::ForFile(state.preset_project).saved / "Layouts");
}

// A file saved under a built-in name wins when that name is chosen; Reset to Default bypasses it and always rebuilds.
void ChooseBuiltinLayout(LayoutState& state, const std::string& name) {
    state.active = name;
    state.apply_builtin.clear();
    if (std::optional<std::string> text = state.presets.Load(name)) {
        state.apply_text = std::move(*text);
    } else {
        state.apply_text.clear();
        state.apply_builtin = name;
    }
}

void ChooseSavedLayout(LayoutState& state, const std::string& name) {
    if (std::optional<std::string> text = state.presets.Load(name)) {
        state.active = name;
        state.apply_builtin.clear();
        state.apply_text = std::move(*text);
    }
}

void ResetLayout(LayoutState& state) {
    state.active = "Default";
    state.apply_text.clear();
    state.apply_builtin = "Default";
}

void DrawSaveLayoutPopup(LayoutState& state) {
    if (state.open_save_popup) {
        ImGui::OpenPopup("Save Layout As");
        state.open_save_popup = false;
    }
    if (!ImGui::BeginPopupModal("Save Layout As", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    ImGui::TextUnformatted("Save the current arrangement as:");
    ImGui::SetNextItemWidth(320.0f);
    const bool entered = ImGui::InputTextWithHint("##layout_name", "My layout", state.save_name, sizeof(state.save_name),
                                                  ImGuiInputTextFlags_EnterReturnsTrue);
    if (!state.save_error.empty()) ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", state.save_error.c_str());
    ImGui::TextDisabled("Saved in %s", state.presets.Directory().empty() ? "the user config folder" : state.presets.Directory().string().c_str());
    ImGui::Separator();
    const bool save = ImGui::Button("Save", ImVec2(90.0f, 0.0f)) || entered;
    ImGui::SameLine();
    const bool cancel = ImGui::Button("Cancel", ImVec2(90.0f, 0.0f));
    if (save) {
        std::size_t length = 0;
        const char* ini = ImGui::SaveIniSettingsToMemory(&length);
        ImGui::GetIO().WantSaveIniSettings = false; // captured here, not by the ini writer
        std::string error;
        if (ini == nullptr || length == 0) {
            state.save_error = "The editor has no layout to save yet";
        } else if (state.presets.Save(state.save_name, std::string(ini, length), &error)) {
            state.active = state.presets.Path(state.save_name).stem().string(); // the name the menu will list
            state.apply_text.clear();
            state.apply_builtin.clear();
            state.save_name[0] = '\0';
            state.save_error.clear();
            ImGui::CloseCurrentPopup();
        } else {
            state.save_error = error;
        }
    }
    if (cancel) {
        state.save_name[0] = '\0';
        state.save_error.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void DrawWindowMenu(LayoutState& state) {
    if (!ImGui::BeginMenu("Window")) return;
    if (ImGui::BeginMenu("Layout")) {
        for (const std::string& name : LayoutPresets::BuiltinNames()) {
            std::string label = name;
            if (state.presets.Load(name)) label += " (custom)";
            if (ImGui::MenuItem(label.c_str(), nullptr, state.active == name)) ChooseBuiltinLayout(state, name);
        }
        const std::vector<std::string> saved = state.presets.SavedNames();
        if (!saved.empty()) {
            ImGui::Separator();
            for (const std::string& name : saved) {
                if (ImGui::MenuItem(name.c_str(), nullptr, state.active == name)) ChooseSavedLayout(state, name);
            }
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Save Layout As...")) {
            std::snprintf(state.save_name, sizeof(state.save_name), "%s", state.active.c_str());
            state.save_error.clear();
            state.open_save_popup = true;
        }
        if (ImGui::MenuItem("Reset to Default")) ResetLayout(state);
        ImGui::EndMenu();
    }
    ImGui::EndMenu();
}

void DrawEditor(EditorWorkspace& workspace, LayoutState& layouts, const char* backend_label) {
    const ImGuiIO& io = ImGui::GetIO();
    SyncPresetFolder(layouts, workspace); // a project opened this frame switches Saved/Layouts before the menu lists it
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            workspace.DrawMenuItems("File"); // New, Open, Save, Save as, Quit: the registry's File commands
            ImGui::EndMenu();
        }
        workspace.DrawEditMenu();
        if (ImGui::BeginMenu("Asset")) {
            workspace.DrawMenuItems("Asset"); // Content Browser, Refresh Assets, Validate Assets
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Actor")) {
            workspace.DrawMenuItems("Actor"); // Create Empty Actor, Camera, Cine Camera
            ImGui::EndMenu();
        }
        workspace.DrawToolsMenu();
        if (ImGui::BeginMenu("View")) {
            workspace.DrawMenuItems("View"); // Command Palette (Ctrl+Shift+P), Preferences
            ImGui::EndMenu();
        }
        DrawWindowMenu(layouts); // Layout: the built-in presets, saved ones, Save Layout As, Reset to Default
        workspace.DrawHelpMenu(); // Keyboard Shortcuts (F1), About
        ImGui::TextDisabled("   Aether Editor  -  %s  -  %zu tools", backend_label, workspace.ToolCount());
        ImGui::EndMainMenuBar();
    }
    DrawSaveLayoutPopup(layouts);

    // The main toolbar claims the strip under the menu bar, and the status bar the one along the bottom, so the
    // dockspace fills what is left.
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    if (ImGui::BeginViewportSideBar("##toolbar", viewport, ImGuiDir_Up, ImGui::GetFrameHeight(), ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings)) {
        workspace.DrawMainToolbar();
    }
    ImGui::End();
    // The open documents, as tabs, on their own strip under the toolbar.
    if (ImGui::BeginViewportSideBar("##document_tabs", viewport, ImGuiDir_Up, ImGui::GetFrameHeight(), ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings)) {
        workspace.DrawDocumentTabs();
    }
    ImGui::End();
    if (ImGui::BeginViewportSideBar("##status", viewport, ImGuiDir_Down, ImGui::GetFrameHeight(), ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings)) {
        workspace.DrawStatusBar();
        ImGui::SameLine(0.0f, 10.0f);
        ImGui::TextDisabled("|");
        ImGui::SameLine(0.0f, 10.0f);
        ImGui::TextDisabled("%s  |  %.1f ms  (%.0f fps)", backend_label, 1000.0f / std::max(io.Framerate, 1.0f), io.Framerate);
    }
    ImGui::End();

    const ImGuiID dockspace = ImGui::DockSpaceOverViewport(0, viewport);
    if (viewport->WorkSize.x > 1.0f && viewport->WorkSize.y > 1.0f) {
        if (!layouts.apply_builtin.empty()) { // a built-in arrived from the menu (or the remembered preset): rebuild it
            BuildDockLayout(dockspace, viewport->WorkSize, layouts.apply_builtin);
            layouts.apply_builtin.clear();
        } else {
            const ImGuiDockNode* node = ImGui::DockBuilderGetNode(dockspace);
            const bool empty = node == nullptr || (node->IsLeafNode() && node->Windows.Size == 0); // nothing saved: the first run
            if (empty) BuildDockLayout(dockspace, viewport->WorkSize, "Default");
        }
    }

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

int main(int argc, char** argv) {
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
        // The project to open: the command line first, then AETHER_PROJECT. With neither (or a project that will not
        // open) the Project Hub is the start screen, so a launch with no arguments is not a sample project nobody chose.
        const char* project = argc > 1 ? argv[1] : std::getenv("AETHER_PROJECT");
        if (project != nullptr && *project != '\0') {
            if (!workspace.OpenProject(project)) workspace.OpenProjectHub();
        } else {
            workspace.OpenProjectHub();
        }
        bool quit = false;
        bool close_approved = false;
        if (window) {
            // The title-bar X asks first when the scene has unsaved changes, like File > Quit: the OS destroys the
            // window as soon as it is allowed to, so the prompt has to keep it open.
            window->on_close = [&] {
                if (close_approved || !workspace.HasUnsavedChanges()) return true;
                workspace.RequestQuit();
                return false;
            };
        }
        workspace.on_quit_confirmed = [&] {
            close_approved = true;
            quit = true;
        };
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
        // Window > Layout: the preset last chosen is remembered beside the tool. A saved one is applied as its ini; a
        // built-in is the arrangement rebuilt once the dockspace exists on the first frame.
        LayoutState layouts;
        SyncPresetFolder(layouts, workspace);
        if (!layout.layout.empty()) {
            layouts.active = layout.layout;
            if (LayoutPresets::IsBuiltin(layout.layout)) {
                if (std::optional<std::string> text = layouts.presets.Load(layout.layout)) {
                    layouts.apply_text = std::move(*text);
                } else {
                    layouts.apply_builtin = layout.layout;
                }
            } else if (std::optional<std::string> text = layouts.presets.Load(layout.layout)) {
                layouts.apply_text = std::move(*text);
            } else {
                layouts.active = "Default"; // the file is gone: the arrangement it left in editor_panels.ini stays
            }
        }
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
        // A last session that did not end cleanly left an autosave; offer it once the editor is up. Headless runs
        // (tests, screenshots) skip it: nothing there can answer the prompt, and the answer must not be "discard".
        if (!headless) workspace.OfferRecovery();
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
            if (!layouts.apply_text.empty()) { // Window > Layout chose a saved arrangement: its ini replaces the current one
                ImGui::LoadIniSettingsFromMemory(layouts.apply_text.data(), layouts.apply_text.size());
                layouts.apply_text.clear();
            }
            host.NewFrame();
            ImGui::NewFrame();
            workspace.Update(io.DeltaTime);
            if (frame > 0 && frame % 300 == 0) workspace.RefreshSourceControl(); // a checkout outside the editor shows up within a few seconds
            select_pending();
            DrawEditor(workspace, layouts, backend_label);
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
                layout.layout = layouts.active;
                SaveShellLayout(layout_file, layout);
            }
            Profiler::Get().EndFrame();
        }
        device->WaitForFence(fence);
        if (!layout_file.empty() && pending_tool.empty()) {
            layout.tool = workspace.ToolName(workspace.Selected());
            layout.layout = layouts.active;
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
