#pragma once

#include "extensions.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

// Editor scripts (Phase 26 step 4, docs/design/PHASE_SPECS.md §26.5): Luau
// files that extend the editor. Every .luau file under a project's
// Content/Editor/ folder runs once when the project opens (and again on
// Reload), and uses
//
//   editor.AddPanel(name, function() ... end)
//   editor.AddMenuItem("Tools/Level/Bake", function() ... end [, "Ctrl+B"])
//   editor.AddAssetType(name, ".ext", newFileText [, function(path) ... end])
//   editor.Log(text)
//
// to register into the ExtensionRegistry, and, inside a panel's function,
// the immediate-mode ui.* widgets: Text, TextDisabled, Button, Checkbox,
// SliderFloat, InputText, CollapsingHeader, SameLine, Separator, Spacing.
// Widgets that edit a value return the new one:  on = ui.Checkbox("On", on).
//
// A script's registrations are owned by its file, and removed on Reload. A
// script that fails to load, or errors inside a panel or menu action, is
// reported (Errors()), never fatal. Without Luau (AETHER_BUILD_SCRIPTING
// off) Available() is false and Load reports that.

namespace aether::editor {

class EditorScripts {
public:
    explicit EditorScripts(ExtensionRegistry& registry);
    ~EditorScripts();
    EditorScripts(const EditorScripts&) = delete;
    EditorScripts& operator=(const EditorScripts&) = delete;

    static bool Available();

    // Runs every .luau under `folder`, in name order; the previous scripts
    // are unloaded first. Returns how many loaded.
    usize Load(const std::filesystem::path& folder);
    usize Reload();
    // Unloads everything.
    void Unload();

    usize ScriptCount() const;
    // Load failures and the latest runtime error of each script, as
    // "file: message"; empty when all is well.
    std::vector<std::string> Errors() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace aether::editor
