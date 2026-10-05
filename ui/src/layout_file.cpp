#include "aether/ui/layout_file.h"

#include "aether/ui/basic.h"
#include "aether/ui/controls.h"
#include "aether/ui/panels.h"

#include <map>

namespace aether::ui {

using nlohmann::json;

namespace {

constexpr const char* kVisibility[] = {"Visible", "Hidden", "Collapsed", "HitTestInvisible", "SelfHitTestInvisible"};
constexpr const char* kHAlign[] = {"Fill", "Left", "Center", "Right"};
constexpr const char* kVAlign[] = {"Fill", "Top", "Center", "Bottom"};
constexpr const char* kTextAlign[] = {"Left", "Center", "Right"};
constexpr const char* kFill[] = {"LeftToRight", "RightToLeft", "BottomToTop", "TopToBottom"};

template <usize N>
int IndexOf(const char* const (&names)[N], const std::string& s) {
    for (usize i = 0; i < N; ++i) {
        if (s == names[i]) return static_cast<int>(i);
    }
    return -1;
}

bool Fail(std::string* error, const std::string& m) {
    if (error != nullptr) *error = m;
    return false;
}

json V2(Vec2 v) { return {v.x, v.y}; }
json M4(const Margin& m) { return {m.left, m.top, m.right, m.bottom}; }
Vec2 ReadV2(const json& j, Vec2 d) { return j.is_array() && j.size() == 2 ? Vec2{j[0].get<f32>(), j[1].get<f32>()} : d; }
Margin ReadM4(const json& j, Margin d) {
    if (j.is_number()) return Margin::All(j.get<f32>());
    return j.is_array() && j.size() == 4 ? Margin{j[0].get<f32>(), j[1].get<f32>(), j[2].get<f32>(), j[3].get<f32>()} : d;
}
bool SameMargin(const Margin& a, const Margin& b) { return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom; }

// A string property naming one of `names`; the default when missing.
template <typename E, usize N>
bool ReadEnum(const json& j, const char* key, const char* const (&names)[N], E& out, std::string* error) {
    if (!j.contains(key)) return true;
    const int i = IndexOf(names, j[key].get<std::string>());
    if (i < 0) return Fail(error, std::string("unknown ") + key + " '" + j[key].get<std::string>() + "'");
    out = static_cast<E>(i);
    return true;
}

json SlotToJson(const Slot& s) {
    const Slot d;
    json j = json::object();
    if (s.anchors.min_x != d.anchors.min_x || s.anchors.min_y != d.anchors.min_y || s.anchors.max_x != d.anchors.max_x || s.anchors.max_y != d.anchors.max_y) {
        j["anchors"] = {s.anchors.min_x, s.anchors.min_y, s.anchors.max_x, s.anchors.max_y};
    }
    if (!(s.position == d.position)) j["position"] = V2(s.position);
    if (!(s.size == d.size)) j["size"] = V2(s.size);
    if (!(s.alignment == d.alignment)) j["alignment"] = V2(s.alignment);
    if (!SameMargin(s.margins, d.margins)) j["margins"] = M4(s.margins);
    if (s.auto_size) j["auto_size"] = true;
    if (s.z != 0) j["z"] = s.z;
    if (!SameMargin(s.padding, d.padding)) j["padding"] = M4(s.padding);
    if (s.fill != 0.0f) j["fill"] = s.fill;
    if (s.h_align != d.h_align) j["h_align"] = kHAlign[static_cast<usize>(s.h_align)];
    if (s.v_align != d.v_align) j["v_align"] = kVAlign[static_cast<usize>(s.v_align)];
    if (s.row != 0) j["row"] = s.row;
    if (s.column != 0) j["column"] = s.column;
    if (s.row_span != 1) j["row_span"] = s.row_span;
    if (s.column_span != 1) j["column_span"] = s.column_span;
    return j;
}

bool SlotFromJson(const json& j, Slot& s, std::string* error) {
    if (j.contains("anchors")) {
        const json& a = j["anchors"];
        if (!a.is_array() || a.size() != 4) return Fail(error, "anchors are [min_x, min_y, max_x, max_y]");
        s.anchors = {a[0].get<f32>(), a[1].get<f32>(), a[2].get<f32>(), a[3].get<f32>()};
    }
    s.position = ReadV2(j.value("position", json()), s.position);
    s.size = ReadV2(j.value("size", json()), s.size);
    s.alignment = ReadV2(j.value("alignment", json()), s.alignment);
    s.margins = ReadM4(j.value("margins", json()), s.margins);
    s.auto_size = j.value("auto_size", s.auto_size);
    s.z = j.value("z", s.z);
    s.padding = ReadM4(j.value("padding", json()), s.padding);
    s.fill = j.value("fill", s.fill);
    if (!ReadEnum(j, "h_align", kHAlign, s.h_align, error) || !ReadEnum(j, "v_align", kVAlign, s.v_align, error)) return false;
    s.row = j.value("row", s.row);
    s.column = j.value("column", s.column);
    s.row_span = j.value("row_span", s.row_span);
    s.column_span = j.value("column_span", s.column_span);
    return true;
}

// A property helper: brush fields.
bool ReadBrush(const json& j, const char* key, Brush& out, const TextureResolver& textures, std::string* error) {
    if (!j.contains(key)) return true;
    std::string e;
    if (!BrushFromJson(j[key], out, {}, textures, &e)) return Fail(error, std::string(key) + ": " + e);
    return true;
}

template <typename T>
WidgetType Type(const char* name, std::function<void(const T&, json&)> save, std::function<bool(T&, const json&, const TextureResolver&, std::string*)> load) {
    WidgetType t;
    t.name = name;
    t.make = [] { return std::make_unique<T>(); };
    t.save = [save](const Widget& w, json& j) { save(static_cast<const T&>(w), j); };
    t.load = [load](Widget& w, const json& j, const TextureResolver& tex, std::string* e) { return load(static_cast<T&>(w), j, tex, e); };
    return t;
}

using TR = const TextureResolver&;

std::map<std::string, WidgetType>& Registry() {
    static std::map<std::string, WidgetType> registry = [] {
        std::map<std::string, WidgetType> r;
        auto add = [&](WidgetType t) { r[t.name] = std::move(t); };
        auto none = [](const auto&, json&) {};
        auto nothing = [](auto&, const json&, TR, std::string*) { return true; };
        add(Type<Canvas>("Canvas", none, nothing));
        add(Type<Overlay>("Overlay", none, nothing));
        auto box_save = [](const BoxPanel& b, json& j) {
            if (b.spacing != 0.0f) j["spacing"] = b.spacing;
        };
        auto box_load = [](BoxPanel& b, const json& j, TR, std::string*) {
            b.spacing = j.value("spacing", b.spacing);
            return true;
        };
        add(Type<HorizontalBox>("HorizontalBox", box_save, box_load));
        add(Type<VerticalBox>("VerticalBox", box_save, box_load));
        add(Type<Grid>(
            "Grid",
            [](const Grid& g, json& j) {
                if (!g.column_fill.empty()) j["column_fill"] = g.column_fill;
                if (!g.row_fill.empty()) j["row_fill"] = g.row_fill;
                if (g.column_spacing != 0.0f) j["column_spacing"] = g.column_spacing;
                if (g.row_spacing != 0.0f) j["row_spacing"] = g.row_spacing;
            },
            [](Grid& g, const json& j, TR, std::string*) {
                if (j.contains("column_fill")) g.column_fill = j["column_fill"].get<std::vector<f32>>();
                if (j.contains("row_fill")) g.row_fill = j["row_fill"].get<std::vector<f32>>();
                g.column_spacing = j.value("column_spacing", g.column_spacing);
                g.row_spacing = j.value("row_spacing", g.row_spacing);
                return true;
            }));
        add(Type<SizeBox>(
            "SizeBox",
            [](const SizeBox& b, json& j) {
                for (auto [k, v] : {std::pair{"width", b.width}, {"height", b.height}, {"min_width", b.min_width}, {"min_height", b.min_height},
                                    {"max_width", b.max_width}, {"max_height", b.max_height}}) {
                    if (v != 0.0f) j[k] = v;
                }
            },
            [](SizeBox& b, const json& j, TR, std::string*) {
                b.width = j.value("width", 0.0f), b.height = j.value("height", 0.0f);
                b.min_width = j.value("min_width", 0.0f), b.min_height = j.value("min_height", 0.0f);
                b.max_width = j.value("max_width", 0.0f), b.max_height = j.value("max_height", 0.0f);
                return true;
            }));
        add(Type<Spacer>(
            "Spacer", [](const Spacer& s, json& j) { j["size"] = V2(s.size); },
            [](Spacer& s, const json& j, TR, std::string*) {
                s.size = ReadV2(j.value("size", json()), s.size);
                return true;
            }));
        add(Type<Border>(
            "Border",
            [](const Border& b, json& j) {
                j["background"] = BrushToJson(b.background);
                if (!SameMargin(b.padding, {})) j["padding"] = M4(b.padding);
                if (!b.hit_test) j["hit_test"] = false;
            },
            [](Border& b, const json& j, TR tex, std::string* e) {
                b.padding = ReadM4(j.value("padding", json()), b.padding);
                b.hit_test = j.value("hit_test", b.hit_test);
                return ReadBrush(j, "background", b.background, tex, e);
            }));
        add(Type<ScrollBox>(
            "ScrollBox",
            [](const ScrollBox& s, json& j) {
                if (s.horizontal) j["horizontal"] = true;
                if (s.wheel_step != 60.0f) j["wheel_step"] = s.wheel_step;
            },
            [](ScrollBox& s, const json& j, TR, std::string*) {
                s.horizontal = j.value("horizontal", s.horizontal);
                s.wheel_step = j.value("wheel_step", s.wheel_step);
                return true;
            }));
        add(Type<Text>(
            "Text",
            [](const Text& t, json& j) {
                j["text"] = t.text;
                if (!t.text_key.empty()) j["text_key"] = t.text_key;
                j["size"] = t.size;
                j["color"] = ColorToJson(t.color);
                if (t.justify != TextAlign::Left) j["justify"] = kTextAlign[static_cast<usize>(t.justify)];
                if (t.wrap_width != 0.0f) j["wrap_width"] = t.wrap_width;
                if (!t.font.empty()) j["font"] = t.font;
                if (!(t.effects == TextEffects{})) j["effects"] = TextEffectsToJson(t.effects);
            },
            [](Text& t, const json& j, TR, std::string* e) {
                t.text = j.value("text", t.text);
                t.text_key = j.value("text_key", t.text_key);
                t.size = j.value("size", t.size);
                if (j.contains("color") && !ColorFromJson(j["color"], t.color, {}, e)) return false;
                t.wrap_width = j.value("wrap_width", t.wrap_width);
                t.font = j.value("font", t.font);
                if (j.contains("effects") && !TextEffectsFromJson(j["effects"], t.effects, {}, e)) return false;
                return ReadEnum(j, "justify", kTextAlign, t.justify, e);
            }));
        add(Type<Image>(
            "Image",
            [](const Image& i, json& j) {
                j["brush"] = BrushToJson(i.brush);
                if (i.desired_size.x != 0.0f || i.desired_size.y != 0.0f) j["desired_size"] = V2(i.desired_size);
                if (!i.hit_test) j["hit_test"] = false;
            },
            [](Image& i, const json& j, TR tex, std::string* e) {
                i.desired_size = ReadV2(j.value("desired_size", json()), i.desired_size);
                i.hit_test = j.value("hit_test", i.hit_test);
                return ReadBrush(j, "brush", i.brush, tex, e);
            }));
        auto focusable_save = [](const Control& c, json& j) {
            if (!c.focusable) j["focusable"] = false;
        };
        auto focusable_load = [](Control& c, const json& j) { c.focusable = j.value("focusable", c.focusable); };
        add(Type<Button>(
            "Button",
            [=](const Button& b, json& j) {
                focusable_save(b, j);
                j["padding"] = M4(b.padding);
            },
            [=](Button& b, const json& j, TR, std::string*) {
                focusable_load(b, j);
                b.padding = ReadM4(j.value("padding", json()), b.padding);
                return true;
            }));
        add(Type<Toggle>(
            "Toggle",
            [=](const Toggle& t, json& j) {
                focusable_save(t, j);
                j["checked"] = t.checked;
                j["box_size"] = t.box_size;
            },
            [=](Toggle& t, const json& j, TR, std::string*) {
                focusable_load(t, j);
                t.checked = j.value("checked", t.checked);
                t.box_size = j.value("box_size", t.box_size);
                return true;
            }));
        add(Type<Slider>(
            "Slider",
            [=](const Slider& s, json& j) {
                focusable_save(s, j);
                j["value"] = s.value, j["min"] = s.min, j["max"] = s.max;
                if (s.step != 0.0f) j["step"] = s.step;
                if (s.nav_step != 0.0f) j["nav_step"] = s.nav_step;
            },
            [=](Slider& s, const json& j, TR, std::string*) {
                focusable_load(s, j);
                s.min = j.value("min", s.min), s.max = j.value("max", s.max), s.value = j.value("value", s.value);
                s.step = j.value("step", s.step), s.nav_step = j.value("nav_step", s.nav_step);
                return true;
            }));
        add(Type<ProgressBar>(
            "ProgressBar",
            [](const ProgressBar& p, json& j) {
                j["percent"] = p.percent;
                if (p.fill != ProgressBar::Fill::LeftToRight) j["fill"] = kFill[static_cast<usize>(p.fill)];
                j["background"] = BrushToJson(p.background);
                j["bar"] = BrushToJson(p.bar);
            },
            [](ProgressBar& p, const json& j, TR tex, std::string* e) {
                p.percent = j.value("percent", p.percent);
                return ReadEnum(j, "fill", kFill, p.fill, e) && ReadBrush(j, "background", p.background, tex, e) && ReadBrush(j, "bar", p.bar, tex, e);
            }));
        add(Type<TextInput>(
            "TextInput",
            [=](const TextInput& t, json& j) {
                focusable_save(t, j);
                j["text"] = t.text, j["hint"] = t.hint;
                if (!t.hint_key.empty()) j["hint_key"] = t.hint_key;
                if (t.max_length != 0) j["max_length"] = t.max_length;
                if (t.password) j["password"] = true;
            },
            [=](TextInput& t, const json& j, TR, std::string*) {
                focusable_load(t, j);
                t.SetText(j.value("text", t.text));
                t.hint = j.value("hint", t.hint);
                t.hint_key = j.value("hint_key", t.hint_key);
                t.max_length = j.value("max_length", t.max_length);
                t.password = j.value("password", t.password);
                return true;
            }));
        add(Type<Dropdown>(
            "Dropdown",
            [=](const Dropdown& d, json& j) {
                focusable_save(d, j);
                j["options"] = d.options, j["selected"] = d.selected;
            },
            [=](Dropdown& d, const json& j, TR, std::string*) {
                focusable_load(d, j);
                if (j.contains("options")) d.options = j["options"].get<std::vector<std::string>>();
                d.selected = j.value("selected", d.selected);
                return true;
            }));
        add(Type<ListView>(
            "ListView",
            [=](const ListView& l, json& j) {
                focusable_save(l, j);
                j["item_height"] = l.item_height;
            },
            [=](ListView& l, const json& j, TR, std::string*) {
                focusable_load(l, j);
                l.item_height = j.value("item_height", l.item_height);
                return true;
            }));
        return r;
    }();
    return registry;
}

} // namespace

void RegisterWidgetType(WidgetType type) { Registry()[type.name] = std::move(type); }

const WidgetType* FindWidgetType(const std::string& name) {
    const auto& r = Registry();
    const auto it = r.find(name);
    return it == r.end() ? nullptr : &it->second;
}

std::vector<std::string> WidgetTypeNames() {
    std::vector<std::string> names;
    for (const auto& [name, t] : Registry()) names.push_back(name);
    return names;
}

json WidgetToJson(const Widget& w) {
    json j = {{"type", w.TypeName()}};
    if (!w.name.empty()) j["name"] = w.name;
    if (!w.style_class.empty()) j["class"] = w.style_class;
    if (!w.tooltip.empty()) j["tooltip"] = w.tooltip;
    if (!w.enabled) j["enabled"] = false;
    if (w.visibility != Visibility::Visible) j["visibility"] = kVisibility[static_cast<usize>(w.visibility)];
    if (w.opacity != 1.0f) j["opacity"] = w.opacity;
    if (w.clip_children && dynamic_cast<const ScrollBox*>(&w) == nullptr && dynamic_cast<const ListView*>(&w) == nullptr) j["clip"] = true;
    const auto& n = w.navigation;
    if (!n.up.empty() || !n.down.empty() || !n.left.empty() || !n.right.empty()) {
        j["navigation"] = {{"up", n.up}, {"down", n.down}, {"left", n.left}, {"right", n.right}};
    }
    const json slot = SlotToJson(w.slot);
    if (!slot.empty()) j["slot"] = slot;
    if (const WidgetType* t = FindWidgetType(w.TypeName()); t != nullptr && t->save) t->save(w, j);
    // A ListView's rows are made by code, not saved.
    if (w.ChildCount() > 0 && dynamic_cast<const ListView*>(&w) == nullptr) {
        json children = json::array();
        for (usize i = 0; i < w.ChildCount(); ++i) children.push_back(WidgetToJson(*w.Child(i)));
        j["children"] = children;
    }
    return j;
}

namespace {

std::unique_ptr<Widget> FromJson(const json& j, const TextureResolver& textures, const std::string& where, std::string* error) {
    auto fail = [&](const std::string& m) {
        Fail(error, where + ": " + m);
        return nullptr;
    };
    if (!j.is_object()) return fail("a widget is an object");
    const std::string type = j.value("type", std::string());
    const WidgetType* t = FindWidgetType(type);
    if (t == nullptr) return fail("unknown widget type '" + type + "'");
    std::unique_ptr<Widget> w = t->make();
    w->name = j.value("name", std::string());
    w->style_class = j.value("class", std::string());
    w->tooltip = j.value("tooltip", std::string());
    w->enabled = j.value("enabled", true);
    w->opacity = j.value("opacity", 1.0f);
    if (j.value("clip", false)) w->clip_children = true;
    std::string e;
    if (!ReadEnum(j, "visibility", kVisibility, w->visibility, &e)) return fail(e);
    if (j.contains("navigation")) {
        const json& n = j["navigation"];
        w->navigation = {n.value("up", std::string()), n.value("down", std::string()), n.value("left", std::string()), n.value("right", std::string())};
    }
    if (j.contains("slot") && !SlotFromJson(j["slot"], w->slot, &e)) return fail("slot: " + e);
    if (t->load && !t->load(*w, j, textures, &e)) return fail(e);
    if (j.contains("children")) {
        const json& children = j["children"];
        if (!children.is_array()) return fail("children must be a list");
        for (usize i = 0; i < children.size(); ++i) {
            const std::string child_where = where + "/" + children[i].value("name", std::string(children[i].value("type", "?"))) + "[" + std::to_string(i) + "]";
            std::unique_ptr<Widget> c = FromJson(children[i], textures, child_where, error);
            if (!c) return nullptr;
            if (w->AddChild(std::move(c)) == nullptr) {
                const i32 max = w->MaxChildren();
                return fail(type + (max == 0 ? " takes no children" : " takes at most " + std::to_string(max) + " child" + (max == 1 ? "" : "ren")));
            }
        }
    }
    return w;
}

json BindingToJson(const Binding& b) {
    json j = {{"widget", b.widget}, {"property", b.property}, {"source", b.source}};
    if (!b.divide_by.empty()) j["divide_by"] = b.divide_by;
    if (!b.format.empty()) j["format"] = b.format;
    if (b.precision != 0) j["precision"] = b.precision;
    if (b.invert) j["invert"] = true;
    if (b.two_way) j["two_way"] = true;
    return j;
}

} // namespace

std::unique_ptr<Widget> WidgetFromJson(const json& j, const TextureResolver& textures, std::string* error) {
    try {
        return FromJson(j, textures, j.is_object() ? j.value("name", std::string(j.value("type", "?"))) : "?", error);
    } catch (const json::exception& e) {
        Fail(error, std::string("malformed widget: ") + e.what());
        return nullptr;
    }
}

std::string SaveLayout(const Widget& root, const std::vector<Binding>& bindings, const std::vector<UIAnimation>& animations) {
    json j = {{"version", 1}, {"root", WidgetToJson(root)}};
    if (!animations.empty()) {
        json a = json::array();
        for (const UIAnimation& x : animations) a.push_back(AnimationToJson(x));
        j["animations"] = a;
    }
    if (!bindings.empty()) {
        json b = json::array();
        for (const Binding& x : bindings) b.push_back(BindingToJson(x));
        j["bindings"] = b;
    }
    return j.dump(2);
}

bool LoadLayout(const std::string& text, LayoutDocument& out, const TextureResolver& textures, std::string* error) {
    const json j = json::parse(text, nullptr, false);
    if (j.is_discarded()) return Fail(error, "the layout isn't valid JSON");
    if (!j.is_object() || !j.contains("root")) return Fail(error, "a layout has a root widget");
    if (j.value("version", 1) > 1) return Fail(error, "this layout was saved by a newer version");
    LayoutDocument doc;
    doc.root = WidgetFromJson(j["root"], textures, error);
    if (!doc.root) return false;
    try {
        for (const json& b : j.value("bindings", json::array())) {
            Binding x;
            x.widget = b.at("widget").get<std::string>();
            x.property = b.at("property").get<std::string>();
            x.source = b.at("source").get<std::string>();
            x.divide_by = b.value("divide_by", std::string());
            x.format = b.value("format", std::string());
            x.precision = b.value("precision", 0);
            x.invert = b.value("invert", false);
            x.two_way = b.value("two_way", false);
            doc.bindings.push_back(std::move(x));
        }
    } catch (const json::exception& e) {
        return Fail(error, std::string("malformed binding: ") + e.what());
    }
    try {
        for (const json& a : j.value("animations", json::array())) {
            UIAnimation anim;
            if (!AnimationFromJson(a, anim, error)) return false;
            doc.animations.push_back(std::move(anim));
        }
    } catch (const json::exception& e) {
        return Fail(error, std::string("malformed animation: ") + e.what());
    }
    out = std::move(doc);
    return true;
}

} // namespace aether::ui
