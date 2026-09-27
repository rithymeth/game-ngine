#pragma once

#include "aether/ui/controls.h"

#include <nlohmann/json.hpp>

#include <functional>
#include <map>
#include <string>

namespace aether::ui {

// Themes (Phase 18 step 3, docs/design/PHASE_SPECS.md §18.3), saved as
// `.atheme` JSON: named colours, a style per control type and class
// ("Button", "Button.Primary"), and text styles. Colours in the file are
// "@name" (the palette), "#RRGGBB" / "#RRGGBBAA", or [r, g, b, a].
// Styles can extend another and override what they list.

struct TextStyle {
    Color color{0.95f, 0.95f, 0.95f, 1};
    f32 size = 20.0f;
};

struct Theme {
    std::string name;
    std::map<std::string, Color> colors;
    std::map<std::string, ControlStyle> styles; // "Button", "Button.Primary", "Default"
    std::map<std::string, TextStyle> text;       // "Default", "Title", ...

    // The style for a type and class: "Type.class", else "Type", else "Default", else the built-in one.
    const ControlStyle& StyleFor(const std::string& type, const std::string& style_class) const;
    const TextStyle* TextFor(const std::string& style_class) const;
};

// Turns image paths into the renderer's texture ids (unset: 0).
using TextureResolver = std::function<u32(const std::string& image)>;

// Styles every widget under `root`: controls take their ControlStyle; Text
// its text style ("Text.<class>" or "Default"); progress bars their
// normal and accent brushes; Borders with a class their normal brush.
void ApplyTheme(const Theme& theme, Widget& root);

// JSON (colours resolve against `palette`).
nlohmann::json ColorToJson(const Color& c);
bool ColorFromJson(const nlohmann::json& j, Color& out, const std::map<std::string, Color>& palette = {}, std::string* error = nullptr);
nlohmann::json BrushToJson(const Brush& b);
bool BrushFromJson(const nlohmann::json& j, Brush& out, const std::map<std::string, Color>& palette = {}, const TextureResolver& textures = {},
                   std::string* error = nullptr);
nlohmann::json ThemeToJson(const Theme& theme);
bool ThemeFromJson(const nlohmann::json& j, Theme& out, const TextureResolver& textures = {}, std::string* error = nullptr);
std::string SaveTheme(const Theme& theme);
bool LoadTheme(const std::string& text, Theme& out, const TextureResolver& textures = {}, std::string* error = nullptr);

} // namespace aether::ui
