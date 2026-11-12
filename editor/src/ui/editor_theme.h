#pragma once

#include <filesystem>

namespace aether::editor {

// The editor's look: one dark palette with a single blue accent, roomier spacing, rounded
// controls, and a readable proportional font. Call once after ImGui::CreateContext(). A host
// that cannot find the font keeps ImGui's built-in one; the colors and spacing still apply.
// `font_file` empty searches the usual places (assets/fonts next to the working directory or the
// executable); returns true when a font was loaded.
bool ApplyEditorTheme(float font_size = 16.0f, const std::filesystem::path& font_file = {});

} // namespace aether::editor
