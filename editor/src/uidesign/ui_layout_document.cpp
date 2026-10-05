#include "uidesign/ui_layout_document.h"

#include "aether/ui/basic.h"
#include "aether/ui/controls.h"
#include "aether/ui/panels.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

namespace aether::editor {

using nlohmann::json;
using ui::Widget;

namespace {

bool Fail(std::string* error, const std::string& message) {
    if (error != nullptr) *error = message;
    return false;
}

void CollectNames(const Widget& w, std::vector<std::string>& out) {
    if (!w.name.empty()) out.push_back(w.name);
    for (usize i = 0; i < w.ChildCount(); ++i) CollectNames(*w.Child(i), out);
}

// Widgets have AddChild (at the end) and RemoveChild: put one at `index` by taking the later ones off and back.
Widget* InsertChild(Widget& parent, usize index, std::unique_ptr<Widget> child) {
    std::vector<std::unique_ptr<Widget>> later;
    while (parent.ChildCount() > index) later.push_back(parent.RemoveChild(parent.Child(index)));
    Widget* added = parent.AddChild(std::move(child));
    for (auto& w : later) parent.AddChild(std::move(w));
    return added;
}

bool Accepts(const Widget& parent, usize extra = 1) {
    const i32 max = parent.MaxChildren();
    return max < 0 || parent.ChildCount() + extra <= static_cast<usize>(max);
}

bool IsPrefix(const WidgetPath& prefix, const WidgetPath& path) {
    return prefix.size() <= path.size() && std::equal(prefix.begin(), prefix.end(), path.begin());
}

// "Button12" -> "Button".
std::string BaseOf(const std::string& name) {
    usize end = name.size();
    while (end > 0 && name[end - 1] >= '0' && name[end - 1] <= '9') --end;
    return end == 0 ? name : name.substr(0, end);
}

std::string Unique(const std::string& base, const std::set<std::string>& taken, bool numbered) {
    if (!numbered && !taken.count(base)) return base;
    for (usize n = 1;; ++n) {
        const std::string candidate = base + std::to_string(n);
        if (!taken.count(candidate)) return candidate;
    }
}

void RenameCopies(json& j, std::set<std::string>& taken) {
    if (const auto it = j.find("name"); it != j.end() && it->is_string() && !it->get<std::string>().empty()) {
        const std::string fresh = Unique(BaseOf(it->get<std::string>()), taken, true);
        taken.insert(fresh);
        *it = fresh;
    }
    if (const auto it = j.find("children"); it != j.end() && it->is_array()) {
        for (json& c : *it) RenameCopies(c, taken);
    }
}

// A Canvas child's slot for a rectangle (root space) inside the canvas `g`.
void SlotForRect(ui::Slot& s, const ui::Rect& g, const ui::Rect& r, const ui::Rect& current) {
    auto axis = [](bool stretch, f32 amin, f32 amax, f32 origin, f32 extent, f32 pos, f32 len, f32 pivot, f32& out_pos, f32& out_len, f32& inset_min,
                   f32& inset_max) {
        if (stretch) {
            inset_min = pos - (origin + amin * extent);
            inset_max = origin + amax * extent - (pos + len);
        } else {
            out_pos = pos - (origin + amin * extent) + pivot * len;
            out_len = len;
        }
    };
    axis(s.anchors.StretchX(), s.anchors.min_x, s.anchors.max_x, g.x, g.w, r.x, r.w, s.alignment.x, s.position.x, s.size.x, s.margins.left, s.margins.right);
    axis(s.anchors.StretchY(), s.anchors.min_y, s.anchors.max_y, g.y, g.h, r.y, r.h, s.alignment.y, s.position.y, s.size.y, s.margins.top, s.margins.bottom);
    // Resizing takes over from the widget's own size.
    if (s.auto_size && (std::abs(r.w - current.w) > 0.01f || std::abs(r.h - current.h) > 0.01f)) s.auto_size = false;
}

} // namespace

UILayoutDocument::UILayoutDocument() {
    layout_.root = std::make_unique<ui::Canvas>();
    layout_.root->name = "Root";
}

UILayoutDocument::UILayoutDocument(ui::LayoutDocument layout, std::filesystem::path path) : layout_(std::move(layout)), path_(std::move(path)) {
    if (!layout_.root) {
        layout_.root = std::make_unique<ui::Canvas>();
        layout_.root->name = "Root";
    }
}

bool UILayoutDocument::Load(const std::filesystem::path& path, std::string* error) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return Fail(error, "can't open '" + path.string() + "'");
    std::stringstream ss;
    ss << f.rdbuf();
    ui::LayoutDocument loaded;
    if (!ui::LoadLayout(ss.str(), loaded, {}, error)) return false;
    layout_ = std::move(loaded);
    path_ = path;
    history_.Clear();
    dirty_ = false;
    ++revision_;
    return true;
}

bool UILayoutDocument::Save(std::string* error) {
    if (path_.empty()) return Fail(error, "the layout has no file yet (Save As)");
    std::ofstream f(path_, std::ios::binary | std::ios::trunc);
    if (!f) return Fail(error, "can't write '" + path_.string() + "'");
    f << Text();
    if (!f) return Fail(error, "can't write '" + path_.string() + "'");
    dirty_ = false;
    return true;
}

bool UILayoutDocument::SaveAs(const std::filesystem::path& path, std::string* error) {
    const std::filesystem::path old = path_;
    path_ = path;
    if (Save(error)) return true;
    path_ = old;
    return false;
}

std::string UILayoutDocument::Text() const { return ui::SaveLayout(*layout_.root, layout_.bindings, layout_.animations); }

std::string UILayoutDocument::Name() const { return path_.empty() ? std::string("Untitled") : path_.stem().string(); }

Widget* UILayoutDocument::At(const WidgetPath& path) const {
    Widget* w = layout_.root.get();
    for (usize i : path) {
        if (w == nullptr) return nullptr;
        w = w->Child(i);
    }
    return w;
}

std::optional<WidgetPath> UILayoutDocument::PathOf(const Widget* widget) const {
    if (widget == nullptr) return std::nullopt;
    WidgetPath path;
    const Widget* w = widget;
    while (w->Parent() != nullptr) {
        const Widget* p = w->Parent();
        usize i = 0;
        while (i < p->ChildCount() && p->Child(i) != w) ++i;
        path.insert(path.begin(), i);
        w = p;
    }
    if (w != layout_.root.get()) return std::nullopt;
    return path;
}

std::optional<WidgetPath> UILayoutDocument::PathOf(const std::string& name) const {
    if (name.empty()) return std::nullopt;
    return PathOf(layout_.root->Find(name));
}

const ui::UIAnimation* UILayoutDocument::FindAnimation(const std::string& name) const {
    for (const ui::UIAnimation& a : layout_.animations) {
        if (a.name == name) return &a;
    }
    return nullptr;
}

ui::UIAnimation* UILayoutDocument::MutableAnimation(const std::string& name) { return const_cast<ui::UIAnimation*>(FindAnimation(name)); }

json UILayoutDocument::Snapshot() const { return json::parse(Text()); }

void UILayoutDocument::Restore(const json& snapshot) {
    ui::LayoutDocument restored;
    if (ui::LoadLayout(snapshot.dump(), restored)) layout_ = std::move(restored);
}

void UILayoutDocument::Changed() {
    dirty_ = true;
    ++revision_;
}

void UILayoutDocument::Edit(const std::string& label, const std::function<void(ui::LayoutDocument&)>& change, const std::string& merge_key) {
    history_.Record(label, Snapshot(), merge_key);
    change(layout_);
    Changed();
}

bool UILayoutDocument::Undo() {
    json restore;
    if (!history_.Undo(Snapshot(), restore)) return false;
    Restore(restore);
    Changed();
    return true;
}

bool UILayoutDocument::Redo() {
    json restore;
    if (!history_.Redo(Snapshot(), restore)) return false;
    Restore(restore);
    Changed();
    return true;
}

std::string UILayoutDocument::UniqueName(const std::string& base) const {
    std::vector<std::string> names;
    CollectNames(*layout_.root, names);
    const std::set<std::string> taken(names.begin(), names.end());
    return taken.count(base) ? Unique(BaseOf(base), taken, true) : base;
}

// --- The tree ----------------------------------------------------------------------------------------

std::optional<WidgetPath> UILayoutDocument::AddWidget(const std::string& type, const WidgetPath& parent_path, i64 index, std::string* error,
                                                      const std::function<void(Widget&)>& setup) {
    const ui::WidgetType* t = ui::FindWidgetType(type);
    Widget* parent = At(parent_path);
    if (t == nullptr) return Fail(error, "unknown widget type '" + type + "'"), std::nullopt;
    if (parent == nullptr) return Fail(error, "no such parent"), std::nullopt;
    if (!Accepts(*parent)) {
        return Fail(error, std::string(parent->TypeName()) + " '" + parent->name + "' can't take " +
                               (parent->MaxChildren() == 0 ? "children" : "another child")),
               std::nullopt;
    }
    std::vector<std::string> names;
    CollectNames(*layout_.root, names);
    std::unique_ptr<Widget> w = t->make();
    w->name = Unique(type, {names.begin(), names.end()}, true);
    if (setup) setup(*w);
    const usize at = index < 0 ? parent->ChildCount() : std::min<usize>(static_cast<usize>(index), parent->ChildCount());
    Widget* added = nullptr;
    Edit("Add " + type, [&](ui::LayoutDocument&) { added = InsertChild(*parent, at, std::move(w)); });
    return PathOf(added);
}

bool UILayoutDocument::DeleteWidgets(std::vector<WidgetPath> paths, std::string* error) {
    std::vector<Widget*> doomed;
    for (const WidgetPath& p : paths) {
        if (p.empty()) return Fail(error, "the root can't be deleted");
        if (At(p) == nullptr) return Fail(error, "no such widget");
    }
    // Only the outermost of nested ones.
    std::sort(paths.begin(), paths.end());
    paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
    for (const WidgetPath& p : paths) {
        const bool inside = std::any_of(paths.begin(), paths.end(), [&](const WidgetPath& q) { return q != p && IsPrefix(q, p); });
        if (!inside) doomed.push_back(At(p));
    }
    if (doomed.empty()) return false;
    std::vector<std::string> names;
    for (Widget* w : doomed) CollectNames(*w, names);
    Edit(doomed.size() == 1 ? "Delete " + std::string(doomed[0]->name.empty() ? doomed[0]->TypeName() : doomed[0]->name) : "Delete widgets",
         [&](ui::LayoutDocument&) {
             for (Widget* w : doomed) (void)w->Parent()->RemoveChild(w);
             ForgetWidgets(names);
         });
    return true;
}

void UILayoutDocument::ForgetWidgets(const std::vector<std::string>& names) {
    const std::set<std::string> gone(names.begin(), names.end());
    std::erase_if(layout_.bindings, [&](const ui::Binding& b) { return gone.count(b.widget) > 0; });
    for (ui::UIAnimation& a : layout_.animations) std::erase_if(a.tracks, [&](const ui::UITrack& t) { return gone.count(t.widget) > 0; });
}

std::optional<WidgetPath> UILayoutDocument::Duplicate(const WidgetPath& path, std::string* error) {
    Widget* w = At(path);
    if (w == nullptr || path.empty()) return Fail(error, path.empty() ? "the root can't be duplicated" : "no such widget"), std::nullopt;
    Widget* parent = w->Parent();
    if (!Accepts(*parent)) return Fail(error, std::string(parent->TypeName()) + " '" + parent->name + "' can't take another child"), std::nullopt;
    json j = ui::WidgetToJson(*w);
    std::vector<std::string> names;
    CollectNames(*layout_.root, names);
    std::set<std::string> taken(names.begin(), names.end());
    RenameCopies(j, taken);
    if (dynamic_cast<ui::Canvas*>(parent) != nullptr) {
        json& slot = j["slot"];
        slot["position"] = {w->slot.position.x + 16.0f, w->slot.position.y + 16.0f};
    }
    std::unique_ptr<Widget> copy = ui::WidgetFromJson(j, {}, error);
    if (!copy) return std::nullopt;
    Widget* added = nullptr;
    Edit("Duplicate " + (w->name.empty() ? std::string(w->TypeName()) : w->name), [&](ui::LayoutDocument&) { added = InsertChild(*parent, path.back() + 1, std::move(copy)); });
    return PathOf(added);
}

std::optional<WidgetPath> UILayoutDocument::Move(const WidgetPath& from, const WidgetPath& parent_path, usize index, std::string* error) {
    Widget* w = At(from);
    Widget* parent = At(parent_path);
    if (from.empty()) return Fail(error, "the root can't be moved"), std::nullopt;
    if (w == nullptr || parent == nullptr) return Fail(error, "no such widget"), std::nullopt;
    if (IsPrefix(from, parent_path)) return Fail(error, "a widget can't go inside itself"), std::nullopt;
    const bool same_parent = w->Parent() == parent;
    if (!same_parent && !Accepts(*parent)) {
        return Fail(error, std::string(parent->TypeName()) + " '" + parent->name + "' can't take " + (parent->MaxChildren() == 0 ? "children" : "another child")),
               std::nullopt;
    }
    Edit("Move " + (w->name.empty() ? std::string(w->TypeName()) : w->name), [&](ui::LayoutDocument&) {
        std::unique_ptr<Widget> taken = w->Parent()->RemoveChild(w);
        (void)InsertChild(*parent, std::min(index, parent->ChildCount()), std::move(taken));
    });
    return PathOf(w);
}

bool UILayoutDocument::Rename(const WidgetPath& path, const std::string& name, std::string* error) {
    Widget* w = At(path);
    if (w == nullptr) return Fail(error, "no such widget");
    if (w->name == name) return true;
    const std::string old = w->name;
    if (!name.empty() && layout_.root->Find(name) != nullptr) return Fail(error, "another widget is called '" + name + "'");
    if (name.empty() && !old.empty()) {
        const bool used = !BindingsOf(old).empty() || std::any_of(layout_.animations.begin(), layout_.animations.end(), [&](const ui::UIAnimation& a) {
            return std::any_of(a.tracks.begin(), a.tracks.end(), [&](const ui::UITrack& t) { return t.widget == old; });
        });
        if (used) return Fail(error, "bindings or animations use '" + old + "': it needs a name");
    }
    Edit("Rename " + (old.empty() ? std::string(w->TypeName()) : old), [&](ui::LayoutDocument& d) {
        w->name = name;
        if (old.empty()) return;
        for (ui::Binding& b : d.bindings) {
            if (b.widget == old) b.widget = name;
        }
        for (ui::UIAnimation& a : d.animations) {
            for (ui::UITrack& t : a.tracks) {
                if (t.widget == old) t.widget = name;
            }
        }
    });
    return true;
}

// --- Properties --------------------------------------------------------------------------------------

json UILayoutDocument::PropertiesOf(const WidgetPath& path) const {
    const Widget* w = At(path);
    if (w == nullptr) return json::object();
    json j = ui::WidgetToJson(*w);
    j.erase("children");
    return j;
}

json UILayoutDocument::EditableProperties(const WidgetPath& path) const {
    const Widget* w = At(path);
    if (w == nullptr) return json::object();
    // What WidgetToJson leaves out at its default, by type.
    json full = {{"class", ""}, {"tooltip", ""}, {"enabled", true}, {"visibility", "Visible"}, {"opacity", 1.0f}, {"clip", false}};
    const std::string type = w->TypeName();
    if (type == "HorizontalBox" || type == "VerticalBox") full["spacing"] = 0.0f;
    if (type == "Grid") full["column_spacing"] = 0.0f, full["row_spacing"] = 0.0f;
    if (type == "Border") full["padding"] = {0.0f, 0.0f, 0.0f, 0.0f}, full["hit_test"] = true;
    if (type == "ScrollBox") full["horizontal"] = false, full["wheel_step"] = 60.0f;
    if (type == "Text") full["justify"] = "Left", full["wrap_width"] = 0.0f, full["font"] = "", full["text_key"] = "";
    if (type == "TextInput") full["hint_key"] = "";
    if (type == "Image") full["desired_size"] = {0.0f, 0.0f}, full["hit_test"] = true;
    if (dynamic_cast<const ui::Control*>(w) != nullptr) full["focusable"] = true;
    if (type == "Slider") full["step"] = 0.0f, full["nav_step"] = 0.0f;
    if (type == "ProgressBar") full["fill"] = "LeftToRight";
    if (type == "TextInput") full["max_length"] = 0, full["password"] = false;
    full.update(PropertiesOf(path));
    if (type == "ScrollBox" || type == "ListView") full.erase("clip"); // they always clip
    return full;
}

bool UILayoutDocument::SetProperty(const WidgetPath& path, const std::string& key, const json& value, std::string* error, const std::string& merge_key) {
    Widget* w = At(path);
    if (w == nullptr) return Fail(error, "no such widget");
    if (key == "type" || key == "children") return Fail(error, "'" + key + "' can't be set as a property");
    if (key == "name") {
        if (!value.is_string()) return Fail(error, "a name is a string");
        return Rename(path, value.get<std::string>(), error);
    }
    if (const json current = EditableProperties(path); current.contains(key) && current[key] == value) return true; // no change
    json j = ui::WidgetToJson(*w);
    j[key] = value;
    std::string why;
    std::unique_ptr<Widget> rebuilt = ui::WidgetFromJson(j, {}, &why);
    if (!rebuilt) return Fail(error, why.empty() ? "bad value for '" + key + "'" : why);
    // Children came along in the saved form; the widget itself is new.
    Edit("Set " + key, [&](ui::LayoutDocument& d) {
        if (path.empty()) {
            d.root = std::move(rebuilt);
            return;
        }
        Widget* parent = w->Parent();
        (void)parent->RemoveChild(w);
        (void)InsertChild(*parent, path.back(), std::move(rebuilt));
    }, merge_key);
    return true;
}

// --- Canvas placement --------------------------------------------------------------------------------

void UILayoutDocument::Layout(const ui::Rect& area, const ui::Font& font) {
    const ui::LayoutContext ctx{&font, nullptr};
    layout_.root->Measure(ctx);
    layout_.root->Arrange(area, ctx);
}

ui::Rect UILayoutDocument::GeometryOf(const WidgetPath& path) const {
    const Widget* w = At(path);
    return w != nullptr ? w->Geometry() : ui::Rect{};
}

bool UILayoutDocument::InCanvas(const WidgetPath& path) const {
    const Widget* w = At(path);
    return w != nullptr && w->Parent() != nullptr && dynamic_cast<const ui::Canvas*>(w->Parent()) != nullptr;
}

bool UILayoutDocument::PlaceInCanvas(const WidgetPath& path, const ui::Rect& rect, const std::string& merge_key) {
    if (!InCanvas(path)) return false;
    Widget* w = At(path);
    const ui::Rect g = w->Parent()->Geometry(), current = w->Geometry();
    ui::Slot s = w->slot;
    SlotForRect(s, g, {rect.x, rect.y, std::max(rect.w, 0.0f), std::max(rect.h, 0.0f)}, current);
    Edit("Place " + (w->name.empty() ? std::string(w->TypeName()) : w->name), [&](ui::LayoutDocument&) { w->slot = s; }, merge_key);
    return true;
}

bool UILayoutDocument::SetAnchors(const WidgetPath& path, const ui::Anchors& anchors, std::optional<ui::Vec2> alignment) {
    if (!InCanvas(path)) return false;
    Widget* w = At(path);
    const ui::Rect g = w->Parent()->Geometry(), current = w->Geometry();
    ui::Slot s = w->slot;
    s.anchors = anchors;
    if (alignment) s.alignment = *alignment;
    SlotForRect(s, g, current, current);
    Edit("Anchors", [&](ui::LayoutDocument&) { w->slot = s; });
    return true;
}

// --- Bindings ----------------------------------------------------------------------------------------

void UILayoutDocument::AddBinding(const ui::Binding& binding) {
    Edit("Add binding", [&](ui::LayoutDocument& d) { d.bindings.push_back(binding); });
}

bool UILayoutDocument::SetBinding(usize index, const ui::Binding& binding, const std::string& merge_key) {
    if (index >= layout_.bindings.size()) return false;
    Edit("Edit binding", [&](ui::LayoutDocument& d) { d.bindings[index] = binding; }, merge_key);
    return true;
}

bool UILayoutDocument::RemoveBinding(usize index) {
    if (index >= layout_.bindings.size()) return false;
    Edit("Remove binding", [&](ui::LayoutDocument& d) { d.bindings.erase(d.bindings.begin() + static_cast<std::ptrdiff_t>(index)); });
    return true;
}

std::vector<usize> UILayoutDocument::BindingsOf(const std::string& widget) const {
    std::vector<usize> out;
    for (usize i = 0; i < layout_.bindings.size(); ++i) {
        if (!widget.empty() && layout_.bindings[i].widget == widget) out.push_back(i);
    }
    return out;
}

std::vector<std::string> UILayoutDocument::BindableProperties(const Widget& w) {
    std::vector<std::string> out{"visible", "enabled", "opacity"};
    if (dynamic_cast<const ui::Text*>(&w) != nullptr || dynamic_cast<const ui::TextInput*>(&w) != nullptr) out.push_back("text");
    if (dynamic_cast<const ui::ProgressBar*>(&w) != nullptr) out.push_back("percent");
    if (dynamic_cast<const ui::Slider*>(&w) != nullptr) out.push_back("value");
    if (dynamic_cast<const ui::Toggle*>(&w) != nullptr) out.push_back("checked");
    if (dynamic_cast<const ui::Dropdown*>(&w) != nullptr || dynamic_cast<const ui::ListView*>(&w) != nullptr) out.push_back("selected");
    return out;
}

// --- Animations --------------------------------------------------------------------------------------

std::string UILayoutDocument::AddAnimation(const std::string& name) {
    std::set<std::string> taken;
    for (const ui::UIAnimation& a : layout_.animations) taken.insert(a.name);
    const std::string fresh = Unique(name.empty() ? std::string("Animation") : name, taken, false);
    Edit("Add animation", [&](ui::LayoutDocument& d) {
        ui::UIAnimation a;
        a.name = fresh;
        d.animations.push_back(std::move(a));
    });
    return fresh;
}

bool UILayoutDocument::RenameAnimation(const std::string& from, const std::string& to, std::string* error) {
    if (FindAnimation(from) == nullptr) return Fail(error, "no animation '" + from + "'");
    if (to.empty()) return Fail(error, "an animation needs a name");
    if (from == to) return true;
    if (FindAnimation(to) != nullptr) return Fail(error, "there's already an animation '" + to + "'");
    Edit("Rename animation", [&](ui::LayoutDocument&) { MutableAnimation(from)->name = to; });
    return true;
}

bool UILayoutDocument::RemoveAnimation(const std::string& name) {
    if (FindAnimation(name) == nullptr) return false;
    Edit("Remove animation", [&](ui::LayoutDocument& d) { std::erase_if(d.animations, [&](const ui::UIAnimation& a) { return a.name == name; }); });
    return true;
}

bool UILayoutDocument::AddTrack(const std::string& animation, const std::string& widget, const std::string& property, std::string* error) {
    const ui::UIAnimation* a = FindAnimation(animation);
    if (a == nullptr) return Fail(error, "no animation '" + animation + "'");
    if (widget.empty() || layout_.root->Find(widget) == nullptr) return Fail(error, "no widget '" + widget + "'");
    if (!ui::IsAnimatable(property)) return Fail(error, "'" + property + "' can't be animated");
    for (const ui::UITrack& t : a->tracks) {
        if (t.widget == widget && t.property == property) return Fail(error, widget + "." + property + " already has a track");
    }
    Edit("Add track", [&](ui::LayoutDocument&) { MutableAnimation(animation)->tracks.push_back({widget, property, {}}); });
    return true;
}

bool UILayoutDocument::RemoveTrack(const std::string& animation, usize track) {
    const ui::UIAnimation* a = FindAnimation(animation);
    if (a == nullptr || track >= a->tracks.size()) return false;
    Edit("Remove track", [&](ui::LayoutDocument&) {
        auto& tracks = MutableAnimation(animation)->tracks;
        tracks.erase(tracks.begin() + static_cast<std::ptrdiff_t>(track));
    });
    return true;
}

bool UILayoutDocument::SetKey(const std::string& animation, usize track, f32 time, f32 value, ui::Ease ease, const std::string& merge_key) {
    const ui::UIAnimation* a = FindAnimation(animation);
    if (a == nullptr || track >= a->tracks.size() || time < 0.0f || !std::isfinite(time) || !std::isfinite(value)) return false;
    Edit("Set key", [&](ui::LayoutDocument&) {
        std::vector<ui::UIKey>& keys = MutableAnimation(animation)->tracks[track].keys;
        for (ui::UIKey& k : keys) {
            if (std::abs(k.time - time) < 1e-3f) {
                k.value = value;
                k.ease = ease;
                return;
            }
        }
        keys.insert(std::upper_bound(keys.begin(), keys.end(), time, [](f32 t, const ui::UIKey& k) { return t < k.time; }), ui::UIKey{time, value, ease});
    }, merge_key);
    return true;
}

i64 UILayoutDocument::MoveKey(const std::string& animation, usize track, usize key, f32 time, const std::string& merge_key) {
    const ui::UIAnimation* a = FindAnimation(animation);
    if (a == nullptr || track >= a->tracks.size() || key >= a->tracks[track].keys.size() || !std::isfinite(time)) return -1;
    time = std::max(time, 0.0f);
    i64 index = -1;
    Edit("Move key", [&](ui::LayoutDocument&) {
        std::vector<ui::UIKey>& keys = MutableAnimation(animation)->tracks[track].keys;
        ui::UIKey moved = keys[key];
        moved.time = time;
        keys.erase(keys.begin() + static_cast<std::ptrdiff_t>(key));
        // Landing on another key replaces it.
        std::erase_if(keys, [&](const ui::UIKey& k) { return std::abs(k.time - time) < 1e-3f; });
        const auto at = std::upper_bound(keys.begin(), keys.end(), time, [](f32 t, const ui::UIKey& k) { return t < k.time; });
        index = at - keys.begin();
        keys.insert(at, moved);
    }, merge_key);
    return index;
}

bool UILayoutDocument::RemoveKey(const std::string& animation, usize track, usize key) {
    const ui::UIAnimation* a = FindAnimation(animation);
    if (a == nullptr || track >= a->tracks.size() || key >= a->tracks[track].keys.size()) return false;
    Edit("Remove key", [&](ui::LayoutDocument&) {
        auto& keys = MutableAnimation(animation)->tracks[track].keys;
        keys.erase(keys.begin() + static_cast<std::ptrdiff_t>(key));
    });
    return true;
}

// --- Checks ------------------------------------------------------------------------------------------

const std::vector<LayoutDiagnostic>& UILayoutDocument::Diagnostics() const {
    if (diagnostics_revision_ == revision_) return diagnostics_;
    diagnostics_revision_ = revision_;
    diagnostics_.clear();
    auto add = [&](const char* code, const std::string& message, WidgetPath path = {}, bool error = true) {
        diagnostics_.push_back({code, message, std::move(path), error});
    };
    // Duplicate names: every widget after the first with a name.
    std::map<std::string, int> seen;
    std::function<void(const Widget&, WidgetPath&)> walk = [&](const Widget& w, WidgetPath& path) {
        if (!w.name.empty() && ++seen[w.name] == 2) add("UD001", "more than one widget is called '" + w.name + "'", path);
        for (usize i = 0; i < w.ChildCount(); ++i) {
            path.push_back(i);
            walk(*w.Child(i), path);
            path.pop_back();
        }
    };
    WidgetPath root;
    walk(*layout_.root, root);
    for (usize i = 0; i < layout_.bindings.size(); ++i) {
        const ui::Binding& b = layout_.bindings[i];
        const std::string what = "binding " + std::to_string(i + 1) + " (" + b.widget + "." + b.property + ")";
        const Widget* w = b.widget.empty() ? nullptr : layout_.root->Find(b.widget);
        if (w == nullptr) {
            add("UD002", what + ": no widget '" + b.widget + "'");
            continue;
        }
        const std::vector<std::string> props = BindableProperties(*w);
        if (std::find(props.begin(), props.end(), b.property) == props.end()) {
            add("UD003", what + ": a " + std::string(w->TypeName()) + " can't bind '" + b.property + "'", *PathOf(w));
        }
        if (b.source.empty()) add("UD004", what + ": no source", *PathOf(w));
    }
    for (const ui::UIAnimation& a : layout_.animations) {
        std::set<std::string> tracks;
        usize keys = 0;
        for (const ui::UITrack& t : a.tracks) {
            keys += t.keys.size();
            const Widget* w = t.widget.empty() ? nullptr : layout_.root->Find(t.widget);
            if (w == nullptr) add("UD005", "animation '" + a.name + "': no widget '" + t.widget + "'");
            else if (!ui::IsAnimatable(t.property)) add("UD006", "animation '" + a.name + "': '" + t.property + "' can't be animated", *PathOf(w));
            if (!tracks.insert(t.widget + "." + t.property).second) add("UD008", "animation '" + a.name + "': two tracks on " + t.widget + "." + t.property, {}, false);
        }
        if (keys == 0) add("UD007", "animation '" + a.name + "' has no keys", {}, false);
    }
    return diagnostics_;
}

usize UILayoutDocument::ErrorCount() const {
    const auto& d = Diagnostics();
    return static_cast<usize>(std::count_if(d.begin(), d.end(), [](const LayoutDiagnostic& x) { return x.error; }));
}

std::unique_ptr<Widget> UILayoutDocument::CopyTree() const { return ui::WidgetFromJson(ui::WidgetToJson(*layout_.root)); }

} // namespace aether::editor
