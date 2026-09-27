#include "aether/ui/theme.h"

#include "aether/ui/basic.h"
#include "aether/ui/panels.h"

#include <cstdlib>
#include <functional>

namespace aether::ui {

using nlohmann::json;

namespace {

constexpr const char* kBrushKinds[] = {"None", "Color", "Image", "Box", "Frame"};

bool Fail(std::string* error, const std::string& m) {
    if (error != nullptr) *error = m;
    return false;
}

bool Floats(const json& j, f32* out, usize n) {
    if (!j.is_array() || j.size() != n) return false;
    for (usize i = 0; i < n; ++i) {
        if (!j[i].is_number()) return false;
        out[i] = j[i].get<f32>();
    }
    return true;
}

} // namespace

const ControlStyle& Theme::StyleFor(const std::string& type, const std::string& style_class) const {
    if (!style_class.empty()) {
        if (auto it = styles.find(type + "." + style_class); it != styles.end()) return it->second;
    }
    if (auto it = styles.find(type); it != styles.end()) return it->second;
    if (auto it = styles.find("Default"); it != styles.end()) return it->second;
    return DefaultStyle();
}

const TextStyle* Theme::TextFor(const std::string& style_class) const {
    if (!style_class.empty()) {
        if (auto it = text.find(style_class); it != text.end()) return &it->second;
    }
    if (auto it = text.find("Default"); it != text.end()) return &it->second;
    return nullptr;
}

void ApplyTheme(const Theme& theme, Widget& w) {
    if (auto* c = dynamic_cast<Control*>(&w)) {
        c->style = theme.StyleFor(w.TypeName(), w.style_class);
    } else if (auto* t = dynamic_cast<Text*>(&w)) {
        if (const TextStyle* s = theme.TextFor(w.style_class)) t->color = s->color, t->size = s->size, t->font = s->font, t->effects = s->effects;
    } else if (auto* p = dynamic_cast<ProgressBar*>(&w)) {
        const ControlStyle& s = theme.StyleFor("ProgressBar", w.style_class);
        p->background = s.normal;
        p->bar = s.accent;
    } else if (auto* b = dynamic_cast<Border*>(&w); b != nullptr && !w.style_class.empty()) {
        b->background = theme.StyleFor("Border", w.style_class).normal;
    }
    for (usize i = 0; i < w.ChildCount(); ++i) ApplyTheme(theme, *w.Child(i));
}

// --- JSON -----------------------------------------------------------------------------------------

json ColorToJson(const Color& c) { return json::array({c.r, c.g, c.b, c.a}); }

bool ColorFromJson(const json& j, Color& out, const std::map<std::string, Color>& palette, std::string* error) {
    if (j.is_string()) {
        const std::string s = j.get<std::string>();
        if (!s.empty() && s[0] == '@') {
            const auto it = palette.find(s.substr(1));
            if (it == palette.end()) return Fail(error, "unknown colour '" + s + "'");
            out = it->second;
            return true;
        }
        if (!s.empty() && s[0] == '#' && (s.size() == 7 || s.size() == 9)) {
            char* end = nullptr;
            const unsigned long v = std::strtoul(s.c_str() + 1, &end, 16);
            if (end == nullptr || *end != '\0') return Fail(error, "bad colour '" + s + "'");
            const bool alpha = s.size() == 9;
            const auto byte = [&](int shift) { return static_cast<f32>((v >> shift) & 0xFF) / 255.0f; };
            out = alpha ? Color{byte(24), byte(16), byte(8), byte(0)} : Color{byte(16), byte(8), byte(0), 1.0f};
            return true;
        }
        return Fail(error, "bad colour '" + s + "'");
    }
    f32 v[4] = {0, 0, 0, 1};
    if (j.is_array() && j.size() == 3 && Floats(j, v, 3)) {
        out = {v[0], v[1], v[2], 1.0f};
        return true;
    }
    if (!Floats(j, v, 4)) return Fail(error, "a colour is \"@name\", \"#RRGGBB(AA)\" or [r, g, b(, a)]");
    out = {v[0], v[1], v[2], v[3]};
    return true;
}

json TextEffectsToJson(const TextEffects& e) {
    const TextEffects none;
    json j = json::object();
    if (e.outline != none.outline) j["outline"] = e.outline;
    if (!(e.outline_color == none.outline_color)) j["outline_color"] = ColorToJson(e.outline_color);
    if (!(e.shadow_offset == none.shadow_offset)) j["shadow_offset"] = {e.shadow_offset.x, e.shadow_offset.y};
    if (!(e.shadow_color == none.shadow_color)) j["shadow_color"] = ColorToJson(e.shadow_color);
    if (e.shadow_softness != none.shadow_softness) j["shadow_softness"] = e.shadow_softness;
    return j;
}

bool TextEffectsFromJson(const json& j, TextEffects& out, const std::map<std::string, Color>& palette, std::string* error) {
    if (!j.is_object()) return Fail(error, "text effects are an object");
    TextEffects e;
    e.outline = j.value("outline", e.outline);
    e.shadow_softness = j.value("shadow_softness", e.shadow_softness);
    if (j.contains("outline_color") && !ColorFromJson(j["outline_color"], e.outline_color, palette, error)) return false;
    if (j.contains("shadow_color") && !ColorFromJson(j["shadow_color"], e.shadow_color, palette, error)) return false;
    if (j.contains("shadow_offset")) {
        f32 v[2] = {0, 0};
        if (!Floats(j["shadow_offset"], v, 2)) return Fail(error, "shadow_offset is [x, y]");
        e.shadow_offset = {v[0], v[1]};
    }
    if (e.outline < 0.0f || e.shadow_softness < 0.0f) return Fail(error, "outline and shadow softness can't be negative");
    out = e;
    return true;
}

json BrushToJson(const Brush& b) {
    json j = {{"kind", kBrushKinds[static_cast<usize>(b.kind)]}};
    if (b.kind == Brush::Kind::None) return j;
    j["tint"] = ColorToJson(b.tint);
    if (!b.image.empty()) j["image"] = b.image;
    if (b.image_size.x != 0.0f || b.image_size.y != 0.0f) j["image_size"] = {b.image_size.x, b.image_size.y};
    if (!(b.uv == Rect{0, 0, 1, 1})) j["uv"] = {b.uv.x, b.uv.y, b.uv.w, b.uv.h};
    if (b.kind == Brush::Kind::Box || b.kind == Brush::Kind::Frame) {
        j["slice"] = {b.slice.left, b.slice.top, b.slice.right, b.slice.bottom};
        if (b.slice_scale != 1.0f) j["slice_scale"] = b.slice_scale;
    }
    return j;
}

bool BrushFromJson(const json& j, Brush& out, const std::map<std::string, Color>& palette, const TextureResolver& textures, std::string* error) {
    // A bare colour is a solid brush.
    if (j.is_string() || j.is_array()) {
        Color c;
        if (!ColorFromJson(j, c, palette, error)) return false;
        out = Brush::Solid(c);
        return true;
    }
    if (!j.is_object()) return Fail(error, "a brush is an object or a colour");
    Brush b;
    const std::string kind = j.value("kind", std::string(j.contains("image") ? "Image" : "Color"));
    bool found = false;
    for (usize i = 0; i < std::size(kBrushKinds); ++i) {
        if (kind == kBrushKinds[i]) b.kind = static_cast<Brush::Kind>(i), found = true;
    }
    if (!found) return Fail(error, "unknown brush kind '" + kind + "'");
    if (j.contains("tint") && !ColorFromJson(j["tint"], b.tint, palette, error)) return false;
    b.image = j.value("image", std::string());
    if (!b.image.empty() && textures) b.texture = textures(b.image);
    f32 v[4];
    if (j.contains("image_size")) {
        if (!Floats(j["image_size"], v, 2)) return Fail(error, "image_size is [w, h]");
        b.image_size = {v[0], v[1]};
    }
    if (j.contains("uv")) {
        if (!Floats(j["uv"], v, 4)) return Fail(error, "uv is [x, y, w, h]");
        b.uv = {v[0], v[1], v[2], v[3]};
    }
    if (j.contains("slice")) {
        if (!Floats(j["slice"], v, 4)) return Fail(error, "slice is [left, top, right, bottom]");
        b.slice = {v[0], v[1], v[2], v[3]};
    }
    b.slice_scale = j.value("slice_scale", 1.0f);
    out = std::move(b);
    return true;
}

namespace {

constexpr const char* kStyleBrushes[] = {"normal", "hovered", "pressed", "disabled", "accent", "focus"};

Brush* BrushField(ControlStyle& s, usize i) {
    Brush* fields[] = {&s.normal, &s.hovered, &s.pressed, &s.disabled, &s.accent, &s.focus};
    return fields[i];
}

json StyleToJson(const ControlStyle& s) {
    json j;
    for (usize i = 0; i < std::size(kStyleBrushes); ++i) j[kStyleBrushes[i]] = BrushToJson(*BrushField(const_cast<ControlStyle&>(s), i));
    j["focus_width"] = s.focus_width;
    j["text"] = ColorToJson(s.text);
    j["hint"] = ColorToJson(s.hint);
    j["text_size"] = s.text_size;
    return j;
}

bool StyleFields(const json& j, ControlStyle& s, const Theme& t, const TextureResolver& textures, std::string* error) {
    for (usize i = 0; i < std::size(kStyleBrushes); ++i) {
        if (j.contains(kStyleBrushes[i]) && !BrushFromJson(j[kStyleBrushes[i]], *BrushField(s, i), t.colors, textures, error)) {
            if (error != nullptr) *error = std::string(kStyleBrushes[i]) + ": " + *error;
            return false;
        }
    }
    s.focus_width = j.value("focus_width", s.focus_width);
    if (j.contains("text") && !ColorFromJson(j["text"], s.text, t.colors, error)) return false;
    if (j.contains("hint") && !ColorFromJson(j["hint"], s.hint, t.colors, error)) return false;
    s.text_size = j.value("text_size", s.text_size);
    return true;
}

} // namespace

json ThemeToJson(const Theme& t) {
    json colors = json::object(), styles = json::object(), text = json::object();
    for (const auto& [name, c] : t.colors) colors[name] = ColorToJson(c);
    for (const auto& [name, s] : t.styles) styles[name] = StyleToJson(s);
    for (const auto& [name, s] : t.text) {
        json& tj = text[name] = {{"color", ColorToJson(s.color)}, {"size", s.size}};
        if (!s.font.empty()) tj["font"] = s.font;
        if (!(s.effects == TextEffects{})) tj["effects"] = TextEffectsToJson(s.effects);
    }
    return {{"version", 1}, {"name", t.name}, {"colors", colors}, {"styles", styles}, {"text", text}};
}

bool ThemeFromJson(const json& j, Theme& out, const TextureResolver& textures, std::string* error) {
    if (!j.is_object()) return Fail(error, "a theme is a JSON object");
    if (j.value("version", 1) > 1) return Fail(error, "this theme was saved by a newer version");
    Theme t;
    t.name = j.value("name", std::string());
    if (j.contains("colors")) {
        for (const auto& [name, c] : j["colors"].items()) {
            Color color;
            if (!ColorFromJson(c, color, {}, error)) return Fail(error, "colour '" + name + "': " + (error ? *error : ""));
            t.colors[name] = color;
        }
    }
    // Styles, each after the one it extends (in any order in the file).
    const json styles = j.value("styles", json::object());
    std::map<std::string, int> state; // 1 resolving, 2 done
    std::function<bool(const std::string&)> resolve = [&](const std::string& name) -> bool {
        if (state[name] == 2) return true;
        if (state[name] == 1) return Fail(error, "styles extend each other in a loop at '" + name + "'");
        if (!styles.contains(name)) return Fail(error, "unknown style '" + name + "' to extend");
        state[name] = 1;
        const json& sj = styles[name];
        ControlStyle s = DefaultStyle();
        if (sj.contains("extends")) {
            const std::string base = sj["extends"].get<std::string>();
            if (!resolve(base)) return false;
            s = t.styles[base];
        }
        std::string e;
        if (!StyleFields(sj, s, t, textures, &e)) return Fail(error, "style '" + name + "': " + e);
        t.styles[name] = s;
        state[name] = 2;
        return true;
    };
    for (const auto& [name, sj] : styles.items()) {
        if (!sj.is_object()) return Fail(error, "style '" + name + "' isn't an object");
        if (!resolve(name)) return false;
    }
    if (j.contains("text")) {
        for (const auto& [name, tj] : j["text"].items()) {
            TextStyle s;
            if (tj.contains("color") && !ColorFromJson(tj["color"], s.color, t.colors, error)) return false;
            s.size = tj.value("size", s.size);
            s.font = tj.value("font", s.font);
            if (tj.contains("effects") && !TextEffectsFromJson(tj["effects"], s.effects, t.colors, error)) return false;
            t.text[name] = s;
        }
    }
    out = std::move(t);
    return true;
}

std::string SaveTheme(const Theme& t) { return ThemeToJson(t).dump(2); }

bool LoadTheme(const std::string& text, Theme& out, const TextureResolver& textures, std::string* error) {
    const json j = json::parse(text, nullptr, false);
    if (j.is_discarded()) return Fail(error, "the theme isn't valid JSON");
    try {
        return ThemeFromJson(j, out, textures, error);
    } catch (const json::exception& e) {
        return Fail(error, std::string("malformed theme: ") + e.what());
    }
}

} // namespace aether::ui
