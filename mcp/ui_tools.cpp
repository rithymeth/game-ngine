#include "ui_tools.h"

#include "image_util.h"
#include "project_host.h"

#include "aether/ui/layout_file.h"
#include "aether/ui/viewport.h"

#include <algorithm>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

namespace aether::mcp {

namespace {

using namespace detail;
namespace ui = aether::ui;

// The layout text from `definition` (an object or its text) or `file` (under Content/ of the open project, or absolute).
std::string ReadLayoutText(AssetHost& host, const Json& args, std::string* from) {
    if (args.contains("definition")) {
        const Json& d = args["definition"];
        if (from != nullptr) *from = "definition";
        if (d.is_object()) return d.dump();
        if (d.is_string()) return d.get<std::string>();
        throw ToolError("\"definition\" must be a JSON object or a string of JSON");
    }
    if (args.contains("file")) {
        const std::string name = RequireString(args, "file");
        fs::path path(name);
        if (!path.is_absolute()) path = host.Require().paths.content / CleanRelative(name);
        std::ifstream in(path, std::ios::binary);
        if (!in) throw ToolError("Can't read " + path.string());
        std::stringstream ss;
        ss << in.rdbuf();
        if (from != nullptr) *from = name;
        return ss.str();
    }
    throw ToolError("Give \"definition\" (the layout as JSON) or \"file\" (a .aui file under Content/)");
}

struct Loaded {
    ui::LayoutDocument doc;
    std::set<std::string> images;
};

Loaded LoadOrThrow(const std::string& text) {
    Loaded l;
    // Every image gets a non-zero id so brushes keep their kind; the picture itself isn't loaded.
    const ui::TextureResolver textures = [&l](const std::string& image) {
        l.images.insert(image);
        return 1u;
    };
    std::string error;
    if (!ui::LoadLayout(text, l.doc, textures, &error)) throw ToolError("Not a valid layout: " + error);
    return l;
}

f32 NumberOr(const Json& args, const char* key, f32 fallback) {
    if (!args.contains(key)) return fallback;
    if (!args[key].is_number()) throw ToolError(std::string("\"") + key + "\" must be a number");
    return args[key].get<f32>();
}

const char* VisibilityName(ui::Visibility v) {
    switch (v) {
    case ui::Visibility::Visible: return "Visible";
    case ui::Visibility::Hidden: return "Hidden";
    case ui::Visibility::Collapsed: return "Collapsed";
    default: return "Visible";
    }
}

Json RectJson(const ui::Rect& r) { return Json::array({r.x, r.y, r.w, r.h}); }

struct Walk {
    ui::Vec2 screen;
    usize count = 0;
    std::map<std::string, std::vector<std::string>> by_name;
    Json problems = Json::array();
    bool truncated = false;
};

Json WalkTree(const ui::Widget& w, const std::string& path, Walk& walk, u32 depth) {
    ++walk.count;
    const std::string here = path.empty() ? std::string(w.TypeName()) : path + "/" + (w.name.empty() ? std::string(w.TypeName()) : w.name);
    if (!w.name.empty()) walk.by_name[w.name].push_back(here);
    const ui::Rect& g = w.Geometry();
    Json j = {{"type", w.TypeName()}, {"rect", RectJson(g)}};
    if (!w.name.empty()) j["name"] = w.name;
    if (w.visibility != ui::Visibility::Visible) j["visibility"] = VisibilityName(w.visibility);
    if (!w.enabled) j["enabled"] = false;
    if (w.TakesSpace() && w.visibility == ui::Visibility::Visible) {
        if (g.w <= 0.0f || g.h <= 0.0f) {
            // Containers with no size are usually a mistake; a Spacer may legitimately have one axis.
            if (std::string(w.TypeName()) != "Spacer") walk.problems.push_back({{"widget", here}, {"problem", "has no size (" + std::to_string(g.w) + " x " + std::to_string(g.h) + ")"}});
        }
        if (g.x < -0.5f || g.y < -0.5f || g.Right() > walk.screen.x + 0.5f || g.Bottom() > walk.screen.y + 0.5f) {
            const bool inside_scroller = w.Parent() != nullptr && (std::string(w.Parent()->TypeName()).find("Scroll") != std::string::npos);
            if (!inside_scroller) walk.problems.push_back({{"widget", here}, {"problem", "extends past the screen"}, {"rect", RectJson(g)}});
        }
    }
    if (w.ChildCount() > 0) {
        if (depth >= 24 || walk.count > 800) {
            walk.truncated = true;
            j["children"] = "...";
        } else {
            Json kids = Json::array();
            for (usize i = 0; i < w.ChildCount(); ++i) kids.push_back(WalkTree(*w.Child(i), here, walk, depth + 1));
            j["children"] = kids;
        }
    }
    return j;
}

const std::set<std::string>& BindableProperties() {
    static const std::set<std::string> p = {"text", "visible", "enabled", "opacity", "percent", "value", "checked", "selected"};
    return p;
}

// Lays the layout out on a screen and returns the viewport holding it.
struct Laid {
    ui::Viewport viewport;
    ui::BuiltinFont font;
    ui::Widget* root = nullptr;
};

void LayOut(Laid& laid, Loaded& l, const Json& args) {
    const f32 w = NumberOr(args, "width", 1920.0f), h = NumberOr(args, "height", 1080.0f);
    if (!(w >= 16.0f && h >= 16.0f && w <= 8192.0f && h <= 8192.0f)) throw ToolError("width and height must be between 16 and 8192");
    laid.viewport.SetSize({w, h});
    if (OptString(args, "scale", "fit") == "none") laid.viewport.scale_settings.rule = ui::ScaleSettings::Rule::None;
    if (OptString(args, "direction", "ltr") == "rtl") laid.viewport.direction = ui::FlowDirection::RightToLeft;
    laid.root = laid.viewport.Add(std::move(l.doc.root));
    laid.viewport.Layout(laid.font);
}

} // namespace

void RegisterUiTools(McpServer& server, std::shared_ptr<AssetHost> host) {
    server.AddTool(
        {"ui_widget_types",
         "Every widget type a UI layout can use (Canvas, VerticalBox, Button, Text, Slider ...) with how many children it takes and what it saves by default, so a layout "
         "can be written without guessing field names. With `type`, only that one.",
         Schema({{"type", {{"type", "string"}, {"description", "One widget type"}}}}),
         [](const Json& args) -> Json {
             const std::string only = OptString(args, "type");
             Json out = Json::array();
             for (const std::string& name : ui::WidgetTypeNames()) {
                 if (!only.empty() && name != only) continue;
                 const ui::WidgetType* t = ui::FindWidgetType(name);
                 if (t == nullptr || !t->make) continue;
                 const std::unique_ptr<ui::Widget> w = t->make();
                 Json j = {{"type", name}, {"max_children", w->MaxChildren()}, {"interactive", w->Interactive()}, {"default", ui::WidgetToJson(*w)}};
                 out.push_back(j);
             }
             if (!only.empty() && out.empty()) throw ToolError("No widget type \"" + only + "\" (call ui_widget_types without `type` for the list)");
             return {{"count", out.size()}, {"widgets", out}};
         }});

    server.AddTool(
        {"ui_check",
         "Check a UI layout (.aui): it must load (errors name the widget by path), and the result lists problems found after laying it out on a screen of `width` x `height` "
         "(default 1920x1080): duplicate widget names, bindings that name a missing widget or a property that can't be bound, widgets that extend past the screen or have no size. "
         "It also returns the widget tree with each widget's rectangle in layout units, the images the layout uses and the bindings. `scale` \"none\" lays out 1 unit per pixel "
         "instead of fitting the reference resolution; `direction` \"rtl\" mirrors it. Give `definition` (JSON) or `file`.",
         Schema({{"definition", {{"description", "The layout: a JSON object, or its text"}}},
                 {"file", {{"type", "string"}, {"description", "A .aui file under the open project's Content/"}}},
                 {"width", {{"type", "number"}}},
                 {"height", {{"type", "number"}}},
                 {"scale", {{"type", "string"}, {"enum", Json::array({"fit", "none"})}}},
                 {"direction", {{"type", "string"}, {"enum", Json::array({"ltr", "rtl"})}}}}),
         [host](const Json& args) -> Json {
             std::string from;
             Loaded l = LoadOrThrow(ReadLayoutText(*host, args, &from));
             const std::vector<ui::Binding> bindings = l.doc.bindings;
             const std::set<std::string> images = l.images;
             Laid laid;
             LayOut(laid, l, args);

             Walk walk;
             walk.screen = laid.viewport.LayoutSize();
             const Json tree = WalkTree(*laid.root, "", walk, 0);
             for (const auto& [name, paths] : walk.by_name) {
                 if (paths.size() > 1) walk.problems.push_back({{"widget", name}, {"problem", "name used by " + std::to_string(paths.size()) + " widgets"}, {"paths", paths}});
             }
             Json bound = Json::array();
             for (const ui::Binding& b : bindings) {
                 bound.push_back({{"widget", b.widget}, {"property", b.property}, {"source", b.source}, {"two_way", b.two_way}});
                 if (laid.root->Find(b.widget) == nullptr) walk.problems.push_back({{"widget", b.widget}, {"problem", "binding names a widget the layout doesn't have"}});
                 if (!BindableProperties().count(b.property)) {
                     walk.problems.push_back({{"widget", b.widget}, {"problem", "\"" + b.property + "\" can't be bound (text, visible, enabled, opacity, percent, value, checked, selected)"}});
                 }
                 if (b.source.find('.') == std::string::npos) walk.problems.push_back({{"widget", b.widget}, {"problem", "source \"" + b.source + "\" should be <source>.<field>"}});
             }
             return {{"layout", from},
                     {"ok", walk.problems.empty()},
                     {"screen", {{"width", laid.viewport.Size().x}, {"height", laid.viewport.Size().y}, {"scale", laid.viewport.Scale()}}},
                     {"widgets", walk.count},
                     {"problems", walk.problems},
                     {"bindings", bound},
                     {"images", Json(std::vector<std::string>(images.begin(), images.end()))},
                     {"tree", tree},
                     {"truncated", walk.truncated}};
         }});

    server.AddTool(
        {"ui_preview",
         "Render a UI layout to a PNG at `width` x `height` (default 960x540, at most 2048). Text shows as solid blocks of the right size and place (a fixed-width stand-in font) "
         "and images as flat grey rectangles tinted by their colour: it shows layout, anchoring, sizing, colour, opacity and clipping rather than final art. `scale` \"none\" lays out "
         "1 unit per pixel instead of fitting the layout's reference resolution; `direction` \"rtl\" mirrors it. Give `definition` (JSON) or `file`.",
         Schema({{"definition", {{"description", "The layout: a JSON object, or its text"}}},
                 {"file", {{"type", "string"}}},
                 {"width", {{"type", "integer"}}},
                 {"height", {{"type", "integer"}}},
                 {"scale", {{"type", "string"}, {"enum", Json::array({"fit", "none"})}}},
                 {"direction", {{"type", "string"}, {"enum", Json::array({"ltr", "rtl"})}}}}),
         [host](const Json& args) -> Json {
             Loaded l = LoadOrThrow(ReadLayoutText(*host, args, nullptr));
             Json size_args = args;
             if (!size_args.contains("width")) size_args["width"] = 960;
             if (!size_args.contains("height")) size_args["height"] = 540;
             if (size_args["width"].get<f32>() > 2048.0f || size_args["height"].get<f32>() > 2048.0f) throw ToolError("width and height are at most 2048");
             Laid laid;
             LayOut(laid, l, size_args);

             const u32 W = static_cast<u32>(laid.viewport.Size().x), H = static_cast<u32>(laid.viewport.Size().y);
             std::vector<u8> rgba(static_cast<usize>(W) * H * 4);
             for (usize i = 0; i < rgba.size(); i += 4) {
                 rgba[i] = 28;
                 rgba[i + 1] = 30;
                 rgba[i + 2] = 36;
                 rgba[i + 3] = 255;
             }
             const ui::DrawList list = laid.viewport.Paint(laid.font);
             for (const ui::DrawQuad& q : list.quads) {
                 ui::Rect r = q.rect;
                 if (q.clip < list.clips.size()) r = r.Intersect(list.clips[q.clip]);
                 if (r.Empty() || q.color.a <= 0.0f) continue;
                 const bool is_image = q.texture != 0 && q.sdf_range <= 0.0f;
                 const f32 cr = is_image ? 0.5f * (q.color.r + 0.5f) : q.color.r;
                 const f32 cg = is_image ? 0.5f * (q.color.g + 0.5f) : q.color.g;
                 const f32 cb = is_image ? 0.5f * (q.color.b + 0.5f) : q.color.b;
                 const f32 a = std::min(1.0f, q.color.a);
                 const i32 x0 = std::max(0, static_cast<i32>(r.x + 0.5f)), y0 = std::max(0, static_cast<i32>(r.y + 0.5f));
                 const i32 x1 = std::min<i32>(static_cast<i32>(W), static_cast<i32>(r.Right() + 0.5f)), y1 = std::min<i32>(static_cast<i32>(H), static_cast<i32>(r.Bottom() + 0.5f));
                 for (i32 y = y0; y < y1; ++y) {
                     for (i32 x = x0; x < x1; ++x) {
                         u8* px = &rgba[(static_cast<usize>(y) * W + static_cast<usize>(x)) * 4];
                         px[0] = static_cast<u8>(std::clamp(px[0] * (1.0f - a) + cr * 255.0f * a, 0.0f, 255.0f));
                         px[1] = static_cast<u8>(std::clamp(px[1] * (1.0f - a) + cg * 255.0f * a, 0.0f, 255.0f));
                         px[2] = static_cast<u8>(std::clamp(px[2] * (1.0f - a) + cb * 255.0f * a, 0.0f, 255.0f));
                     }
                 }
             }
             const std::vector<u8> png = EncodePngRgba(rgba, W, H);
             Json out = {{"width", W}, {"height", H}, {"quads", list.quads.size()}, {"png_bytes", png.size()}};
             out["mcp_content"] = Json::array({{{"type", "image"}, {"data", Base64Encode(png)}, {"mimeType", "image/png"}}});
             return out;
         }});
}

} // namespace aether::mcp
