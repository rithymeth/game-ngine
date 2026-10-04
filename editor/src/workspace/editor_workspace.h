#pragma once

#include "aether/core/base.h"

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

// Every tool editor the engine has, in one place: the Blueprint, Material,
// animation, AI, audio, VFX, UI, world, networking, debug and project
// (Project Settings, Build and Package) tools, each on
// a sample document so it opens on something real. The Windows editor
// (D3D12) and the portable editor shell (Vulkan) both host it, so neither
// shows less than the other.
//
// Drawing: DrawToolsMenu() goes inside the host's BeginMainMenuBar(),
// DrawHub() is one window with every tool (a list by category and the
// selected tool), and DrawWindows() draws the tools popped out into their
// own windows. All of it is plain Dear ImGui, so it runs headless too.

namespace aether::editor {

class ExtensionRegistry;
class EditorScripts;

class EditorWorkspace {
public:
    // `project_file`: the .aproject the Project tools (Project Settings,
    // Build and Package) work on; empty for a sample project (SampleProject()).
    explicit EditorWorkspace(std::filesystem::path project_file = {});
    ~EditorWorkspace();
    EditorWorkspace(const EditorWorkspace&) = delete;
    EditorWorkspace& operator=(const EditorWorkspace&) = delete;

    // Per frame, before drawing: runs the networked play session and the
    // console's queued log lines.
    void Update(f32 dt);

    // A small project with a startup scene, made under the temp folder the
    // first time and reused after; empty if it can't be made.
    static std::filesystem::path SampleProject(std::string* error = nullptr);
    const std::filesystem::path& ProjectFile() const;
    // Points the Project tools (settings, Build and Package, Plugins) and
    // the plugins' modules at another project. False if it isn't one.
    bool OpenProject(const std::filesystem::path& project_file);

    // What plugins, game modules and the project's editor scripts add (§26.5):
    // panels (tools under "Extensions"), menus, property drawers, asset types.
    ExtensionRegistry& Extensions();
    // The project's Luau editor scripts (Content/Editor/*.luau).
    EditorScripts& Scripts();

    usize ToolCount() const;
    const char* ToolName(usize tool) const;
    const char* ToolCategory(usize tool) const;
    // The tool whose name starts with `name` (case-insensitive), or -1.
    i64 FindTool(std::string_view name) const;

    // The tool the hub shows.
    usize Selected() const;
    void Select(usize tool);
    // Popped out into its own window.
    bool IsWindowOpen(usize tool) const;
    void SetWindowOpen(usize tool, bool open);

    // The tool's contents, into the current window.
    void DrawTool(usize tool);
    // A "Tools" menu: each category's tools; picking one pops it out.
    void DrawToolsMenu();
    // A window with every tool. `open` (optional) gets the title-bar X.
    void DrawHub(const char* title = "Tools", bool* open = nullptr);
    // The hub's contents into the current window (the shell fills its main area with this).
    void DrawHubContents();
    // The popped-out tool windows.
    void DrawWindows();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace aether::editor
