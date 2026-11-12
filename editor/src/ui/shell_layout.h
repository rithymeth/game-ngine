#pragma once

#include "aether/core/base.h"

#include <filesystem>
#include <string>

namespace aether::editor {

// What the portable editor shell remembers between sessions: how wide the side panels were dragged and which
// tool was open. A small JSON file of the user's own (not the project's), written when the editor closes and read
// when it starts. Anything wrong with the file -- missing, damaged, out of range -- gives the defaults, never an error
// dialog: a layout is a convenience, not data.
struct ShellLayout {
    f32 side_fraction = 0.0f; // the side panels' share of the window width; 0 = the shell's own default
    std::string tool;         // the open tool's name; empty = the first
};

// Where the file lives: AETHER_EDITOR_LAYOUT if set, else `aether/editor_layout.json` under XDG_CONFIG_HOME,
// ~/.config or %APPDATA%. Empty when none of those exists, which means "do not remember anything".
std::filesystem::path DefaultShellLayoutPath();

// Reads the file. False (and `layout` unchanged) when it is missing or not a layout; values are clamped
// (side_fraction to 0.15-0.6, or 0 if it is not a number in that range).
bool LoadShellLayout(const std::filesystem::path& file, ShellLayout& layout);

// Writes the file atomically (a temp file, then a rename), making its folder. False with `error` set on failure.
bool SaveShellLayout(const std::filesystem::path& file, const ShellLayout& layout, std::string* error = nullptr);

} // namespace aether::editor
