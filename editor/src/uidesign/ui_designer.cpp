#include "uidesign/ui_designer.h"

#include "core/json_edit.h"

#include "aether/ui/basic.h"
#include "aether/ui/controls.h"
#include "aether/ui/panels.h"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>

namespace aether::editor {

using nlohmann::json;
using ui::Rect;
using ui::Vec2;
using ui::Widget;

namespace {

constexpr f32 kLeftWidth = 220.0f;
constexpr f32 kRightWidth = 330.0f;
constexpr f32 kBottomHeight = 190.0f;
constexpr f32 kHandle = 5.0f;
constexpr f32 kLabelWidth = 190.0f;   // timeline track names
constexpr f32 kPixelsPerSecond = 240.0f;
constexpr const char* kTypePayload = "AETHER_UI_TYPE";
constexpr const char* kPathPayload = "AETHER_UI_PATH";

// The layout's metrics without glyph boxes: the canvas draws text with ImGui over the quads.
class MetricsFont final : public ui::Font {
public:
    f32 Ascent(f32 size) const override { return 0.95f * size; }
    f32 LineHeight(f32 size) const override { return 1.25f * size; }
    ui::Glyph GlyphOf(u32, f32 size) const override {
        ui::Glyph g;
        g.advance = 0.6f * size;
        return g;
    }
};
const MetricsFont kFont;

ImVec2 Im(Vec2 v) { return {v.x, v.y}; }
Vec2 V(ImVec2 v) { return {v.x, v.y}; }
ImU32 Col(const ui::Color& c) {
    auto b = [](f32 v) { return static_cast<int>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return IM_COL32(b(c.r), b(c.g), b(c.b), b(c.a));
}

bool Visible(const Widget& w) { return w.visibility != ui::Visibility::Hidden && w.visibility != ui::Visibility::Collapsed; }

bool Accepts(const Widget& w) {
    const i32 max = w.MaxChildren();
    return max < 0 || w.ChildCount() < static_cast<usize>(max);
}

std::string Label(const Widget& w) { return w.name.empty() ? std::string(w.TypeName()) : std::string(w.TypeName()) + "  " + w.name; }

// Enum-valued properties, for combos.
const JsonEnums& Enums() {
    static const JsonEnums e = {
        {"justify", {"Left", "Center", "Right"}},
        {"visibility", {"Visible", "Hidden", "Collapsed", "HitTestInvisible", "SelfHitTestInvisible"}},
        {"fill", {"LeftToRight", "RightToLeft", "BottomToTop", "TopToBottom"}},
        {"kind", {"None", "Color", "Image", "Box", "Frame"}},
    };
    return e;
}

std::string PathKey(const WidgetPath& p) {
    std::string s;
    for (usize i : p) s += std::to_string(i) + "/";
    return s;
}

struct AnchorPreset {
    const char* name;
    ui::Anchors anchors;
    Vec2 alignment;
};
const AnchorPreset kAnchorPresets[] = {
    {"Top left", ui::Anchors::Point(0, 0), {0, 0}},          {"Top", ui::Anchors::Point(0.5f, 0), {0.5f, 0}},
    {"Top right", ui::Anchors::Point(1, 0), {1, 0}},         {"Left", ui::Anchors::Point(0, 0.5f), {0, 0.5f}},
    {"Centre", ui::Anchors::Point(0.5f, 0.5f), {0.5f, 0.5f}}, {"Right", ui::Anchors::Point(1, 0.5f), {1, 0.5f}},
    {"Bottom left", ui::Anchors::Point(0, 1), {0, 1}},       {"Bottom", ui::Anchors::Point(0.5f, 1), {0.5f, 1}},
    {"Bottom right", ui::Anchors::Point(1, 1), {1, 1}},      {"Top, full width", {0, 0, 1, 0}, {0, 0}},
    {"Bottom, full width", {0, 1, 1, 1}, {0, 1}},             {"Left, full height", {0, 0, 0, 1}, {0, 0}},
    {"Right, full height", {1, 0, 1, 1}, {1, 0}},             {"Fill", ui::Anchors::Stretch(), {0, 0}},
};

Vec2 DefaultSize(const std::string& type) {
    if (type == "Button") return {160, 48};
    if (type == "Slider") return {200, 24};
    if (type == "ProgressBar") return {200, 20};
    if (type == "Toggle") return {160, 32};
    if (type == "TextInput" || type == "Dropdown") return {220, 36};
    if (type == "Image" || type == "Border") return {100, 100};
    if (type == "ListView" || type == "ScrollBox" || type == "VerticalBox" || type == "HorizontalBox" || type == "Grid" || type == "Canvas" ||
        type == "Overlay") {
        return {300, 200};
    }
    return {100, 30};
}

} // namespace

const std::vector<ResolutionPreset>& ResolutionPresets() {
    static const std::vector<ResolutionPreset> presets = {
        {"1920 x 1080 (Full HD)", {1920, 1080}, {}},
        {"1280 x 720 (HD)", {1280, 720}, {}},
        {"2560 x 1440 (QHD)", {2560, 1440}, {}},
        {"3840 x 2160 (4K)", {3840, 2160}, {}},
        {"2048 x 1536 (tablet)", {2048, 1536}, {}},
        {"2340 x 1080 (phone, landscape)", {2340, 1080}, {132, 0, 132, 63}},
        {"1080 x 2340 (phone, portrait)", {1080, 2340}, {0, 132, 0, 63}},
        {"1280 x 800 (handheld)", {1280, 800}, {}},
    };
    return presets;
}

UIDesigner::UIDesigner(UILayoutDocument& document) : doc_(document) { SetResolution(0); }

// --- Selection and preview ---------------------------------------------------------------------------

void UIDesigner::Select(const WidgetPath& path, bool add) {
    const auto it = std::find(selection_.begin(), selection_.end(), path);
    if (!add) {
        selection_ = {path};
    } else if (it != selection_.end()) {
        selection_.erase(it);
    } else {
        selection_.push_back(path);
    }
}

std::optional<WidgetPath> UIDesigner::Primary() const {
    if (selection_.empty() || doc_.At(selection_.back()) == nullptr) return std::nullopt;
    return selection_.back();
}

void UIDesigner::SetResolution(usize preset) {
    const auto& p = ResolutionPresets();
    preset_ = std::min(preset, p.size() - 1);
    SetCustomResolution(p[preset_].size, p[preset_].safe);
}

void UIDesigner::SetCustomResolution(Vec2 pixels, const ui::Margin& safe) {
    viewport_.SetSize({std::max(pixels.x, 16.0f), std::max(pixels.y, 16.0f)});
    viewport_.SetSafeArea(safe);
    preview_revision_ = 0;
    fit_ = true;
}

void UIDesigner::SetZoom(f32 zoom) { zoom_ = std::clamp(zoom, 0.05f, 8.0f); }

Widget* UIDesigner::PreviewWidget(const WidgetPath& path) const {
    Widget* w = preview_.get();
    for (usize i : path) {
        if (w == nullptr) return nullptr;
        w = w->Child(i);
    }
    return w;
}

void UIDesigner::Refresh() {
    // Drop selections of widgets that are gone (undo, delete).
    std::erase_if(selection_, [&](const WidgetPath& p) { return doc_.At(p) == nullptr; });
    if (!animation_.empty() && doc_.FindAnimation(animation_) == nullptr) animation_.clear();
    const bool changed = preview_revision_ != doc_.Revision();
    if (!changed && preview_ && preview_time_ == playhead_ && preview_animation_ == animation_) return;
    const Rect area = PreviewArea();
    if (changed) doc_.Layout(area, kFont);
    preview_ = doc_.CopyTree();
    if (!preview_) return;
    if (theme_ != nullptr) ApplyTheme(*theme_, *preview_);
    if (const ui::UIAnimation* a = doc_.FindAnimation(animation_)) {
        for (const ui::UITrack& t : a->tracks) {
            Widget* w = t.widget.empty() ? nullptr : preview_->Find(t.widget);
            if (w != nullptr && !t.keys.empty()) (void)ui::SetProperty(*w, t.property, t.Evaluate(playhead_));
        }
    }
    const ui::LayoutContext ctx{&kFont, nullptr};
    preview_->Measure(ctx);
    preview_->Arrange(area, ctx);
    preview_revision_ = doc_.Revision();
    preview_time_ = playhead_;
    preview_animation_ = animation_;
}

WidgetPath UIDesigner::PickAt(Vec2 p) const {
    WidgetPath best;
    if (!preview_) return best;
    std::function<bool(const Widget&, WidgetPath&)> walk = [&](const Widget& w, WidgetPath& path) {
        if (!Visible(w)) return false;
        for (usize i = w.ChildCount(); i-- > 0;) {
            path.push_back(i);
            if (walk(*w.Child(i), path)) return true;
            path.pop_back();
        }
        if (!path.empty() && w.Geometry().Contains(p)) {
            best = path;
            return true;
        }
        return false;
    };
    WidgetPath path;
    walk(*preview_, path);
    return best;
}

Vec2 UIDesigner::LayoutToScreen(Vec2 p) const {
    const f32 s = viewport_.Scale() * zoom_;
    return {origin_.x + pan_.x + p.x * s, origin_.y + pan_.y + p.y * s};
}

Vec2 UIDesigner::ScreenToLayout(Vec2 p) const {
    const f32 s = viewport_.Scale() * zoom_;
    return {(p.x - origin_.x - pan_.x) / s, (p.y - origin_.y - pan_.y) / s};
}

void UIDesigner::Fit(Vec2 area) {
    const Vec2 res = viewport_.Size();
    SetZoom(std::min((area.x - 40.0f) / res.x, (area.y - 40.0f) / res.y));
    pan_ = {(area.x - res.x * zoom_) * 0.5f, (area.y - res.y * zoom_) * 0.5f};
    fit_ = false;
}

std::vector<WidgetPath> UIDesigner::CanvasSelection() const {
    std::vector<WidgetPath> out;
    for (const WidgetPath& p : selection_) {
        if (doc_.InCanvas(p)) out.push_back(p);
    }
    return out;
}

Rect UIDesigner::Snapped(const Rect& r) const {
    if (!snap || grid <= 0.0f) return r;
    auto s = [&](f32 v) { return std::round(v / grid) * grid; };
    return {s(r.x), s(r.y), r.w, r.h};
}

int UIDesigner::HandleAt(Vec2 screen) const {
    const std::optional<WidgetPath> p = Primary();
    if (!p || !doc_.InCanvas(*p) || selection_.size() != 1) return -1;
    const Widget* w = PreviewWidget(*p);
    if (w == nullptr) return -1;
    const Rect g = w->Geometry();
    const Vec2 a = LayoutToScreen({g.x, g.y}), b = LayoutToScreen({g.Right(), g.Bottom()});
    const Vec2 m{(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f};
    // Clockwise from the top-left: corners and edge middles.
    const Vec2 points[8] = {{a.x, a.y}, {m.x, a.y}, {b.x, a.y}, {b.x, m.y}, {b.x, b.y}, {m.x, b.y}, {a.x, b.y}, {a.x, m.y}};
    for (int i = 0; i < 8; ++i) {
        if (std::abs(screen.x - points[i].x) <= kHandle + 1 && std::abs(screen.y - points[i].y) <= kHandle + 1) return i;
    }
    return -1;
}

// --- Editing helpers ---------------------------------------------------------------------------------

std::optional<WidgetPath> UIDesigner::PlaceWidget(const std::string& type, Vec2 point) {
    // The container under the point that can take it, else the root.
    WidgetPath parent = PickAt(point);
    while (true) {
        const Widget* w = doc_.At(parent);
        if (w != nullptr && Accepts(*w)) break;
        if (parent.empty()) {
            status_ = "Nowhere to put a " + type + ": the root can't take it";
            return std::nullopt;
        }
        parent.pop_back();
    }
    const Widget* container = doc_.At(parent);
    const bool canvas = dynamic_cast<const ui::Canvas*>(container) != nullptr;
    const Rect g = doc_.GeometryOf(parent);
    std::string error;
    const auto path = doc_.AddWidget(type, parent, -1, &error, [&](Widget& w) {
        if (canvas) {
            Vec2 at{point.x - g.x, point.y - g.y};
            if (snap && grid > 0.0f) at = {std::round(at.x / grid) * grid, std::round(at.y / grid) * grid};
            w.slot.position = at;
            w.slot.size = DefaultSize(type);
            if (type == "Text") w.slot.auto_size = true;
        }
        if (type == "Text") static_cast<ui::Text&>(w).text = "Text";
        if (type == "Button") w.AddChild(std::make_unique<ui::Text>("Button"))->slot.h_align = ui::HAlign::Center;
    });
    if (!path) {
        status_ = error;
        return std::nullopt;
    }
    Select(*path);
    status_ = "Added " + doc_.At(*path)->name;
    return path;
}

void UIDesigner::DeleteSelection() {
    std::string error;
    std::vector<WidgetPath> doomed;
    for (const WidgetPath& p : selection_) {
        if (!p.empty()) doomed.push_back(p);
    }
    if (doomed.empty()) return;
    if (doc_.DeleteWidgets(doomed, &error)) selection_.clear();
    else status_ = error;
}

void UIDesigner::DuplicateSelection() {
    const std::optional<WidgetPath> p = Primary();
    if (!p) return;
    std::string error;
    if (const auto copy = doc_.Duplicate(*p, &error)) Select(*copy);
    else status_ = error;
}

void UIDesigner::Nudge(Vec2 delta) {
    for (const WidgetPath& p : CanvasSelection()) {
        const Rect g = doc_.GeometryOf(p);
        doc_.PlaceInCanvas(p, {g.x + delta.x, g.y + delta.y, g.w, g.h}, "nudge");
    }
    Refresh();
}

// --- Timeline ----------------------------------------------------------------------------------------

void UIDesigner::SelectAnimation(const std::string& name) {
    animation_ = doc_.FindAnimation(name) != nullptr ? name : std::string();
    playhead_ = 0.0f;
    playing_ = false;
}

void UIDesigner::SetPlayhead(f32 time) { playhead_ = std::max(time, 0.0f); }

bool UIDesigner::KeyAtPlayhead(usize track) {
    const ui::UIAnimation* a = doc_.FindAnimation(animation_);
    if (a == nullptr || track >= a->tracks.size()) return false;
    const ui::UITrack& t = a->tracks[track];
    const Widget* w = t.widget.empty() ? nullptr : doc_.Root().Find(t.widget);
    f32 value = 0.0f;
    if (w == nullptr || !ui::GetProperty(*w, t.property, value)) {
        status_ = "No " + t.widget + "." + t.property + " to key";
        return false;
    }
    return doc_.SetKey(animation_, track, playhead_, value);
}

bool UIDesigner::SaveNow() {
    std::string error;
    if (doc_.Save(&error)) {
        status_ = "Saved " + doc_.Path().filename().string();
        return true;
    }
    status_ = error;
    return false;
}

// --- Drawing -----------------------------------------------------------------------------------------

void UIDesigner::Draw() {
    Refresh();
    if (playing_) {
        const ui::UIAnimation* a = doc_.FindAnimation(animation_);
        const f32 duration = a != nullptr ? a->Duration() : 0.0f;
        playhead_ += ImGui::GetIO().DeltaTime;
        if (duration <= 0.0f) playing_ = false, playhead_ = 0.0f;
        else if (playhead_ > duration) playhead_ = std::fmod(playhead_, duration);
        Refresh();
    }
    DrawToolbar();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const f32 middle_h = std::max(100.0f, avail.y - kBottomHeight);
    ImGui::BeginChild("left", ImVec2(kLeftWidth, middle_h), ImGuiChildFlags_Borders);
    DrawPalette();
    ImGui::Separator();
    DrawHierarchy();
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("canvas", ImVec2(std::max(100.0f, avail.x - kLeftWidth - kRightWidth - 16.0f), middle_h), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    DrawCanvas();
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("details", ImVec2(kRightWidth, middle_h), ImGuiChildFlags_Borders);
    DrawDetails();
    ImGui::EndChild();
    ImGui::BeginChild("bottom", ImVec2(0, 0), ImGuiChildFlags_Borders);
    if (ImGui::BeginTabBar("bottom_tabs")) {
        if (ImGui::BeginTabItem("Timeline")) {
            DrawTimeline();
            ImGui::EndTabItem();
        }
        const std::string diag = "Diagnostics (" + std::to_string(doc_.Diagnostics().size()) + ")###diag";
        if (ImGui::BeginTabItem(diag.c_str())) {
            DrawDiagnostics();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::EndChild();
    HandleKeys();
}

void UIDesigner::DrawToolbar() {
    if (ImGui::Button("Save")) SaveNow();
    ImGui::SameLine();
    ImGui::BeginDisabled(!doc_.CanUndo());
    if (ImGui::Button("Undo")) doc_.Undo();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!doc_.CanRedo());
    if (ImGui::Button("Redo")) doc_.Redo();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(230);
    const auto& presets = ResolutionPresets();
    if (ImGui::BeginCombo("##resolution", presets[preset_].name.c_str())) {
        for (usize i = 0; i < presets.size(); ++i) {
            if (ImGui::Selectable(presets[i].name.c_str(), i == preset_)) SetResolution(i);
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::Text("%.0f%%", zoom_ * 100.0f);
    ImGui::SameLine();
    if (ImGui::Button("Fit")) fit_ = true;
    ImGui::SameLine();
    ImGui::Checkbox("Snap", &snap);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(60);
    ImGui::DragFloat("Grid", &grid, 1.0f, 1.0f, 256.0f, "%.0f");
    ImGui::SameLine();
    ImGui::TextDisabled("%s%s  %s", doc_.Name().c_str(), doc_.Dirty() ? "*" : "", status_.c_str());
}

void UIDesigner::DrawPalette() {
    ImGui::TextUnformatted("Palette");
    for (const std::string& type : ui::WidgetTypeNames()) {
        ImGui::PushID(type.c_str());
        if (ImGui::Selectable(type.c_str())) {
            // Click: into the selection if it takes children, else at the preview's corner.
            const std::optional<WidgetPath> p = Primary();
            const Rect g = p ? doc_.GeometryOf(*p) : PreviewArea();
            PlaceWidget(type, {g.x + 1.0f, g.y + 1.0f});
        }
        if (ImGui::BeginDragDropSource()) {
            ImGui::SetDragDropPayload(kTypePayload, type.c_str(), type.size() + 1);
            ImGui::Text("%s", type.c_str());
            ImGui::EndDragDropSource();
        }
        ImGui::PopID();
    }
}

void UIDesigner::DrawHierarchy() {
    ImGui::TextUnformatted("Hierarchy");
    WidgetPath path;
    DrawHierarchyNode(doc_.Root(), path);
}

void UIDesigner::DrawHierarchyNode(const Widget& w, WidgetPath& path) {
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth;
    if (w.ChildCount() == 0) flags |= ImGuiTreeNodeFlags_Leaf;
    if (std::find(selection_.begin(), selection_.end(), path) != selection_.end()) flags |= ImGuiTreeNodeFlags_Selected;
    ImGui::PushID(PathKey(path).c_str());
    const bool open = ImGui::TreeNodeEx("node", flags, "%s", Label(w).c_str());
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) Select(path, ImGui::GetIO().KeyCtrl);
    if (!path.empty() && ImGui::BeginDragDropSource()) {
        ImGui::SetDragDropPayload(kPathPayload, path.data(), path.size() * sizeof(usize));
        ImGui::Text("%s", Label(w).c_str());
        ImGui::EndDragDropSource();
    }
    bool moved = false;
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kPathPayload)) {
            WidgetPath from(static_cast<usize>(payload->DataSize) / sizeof(usize));
            std::memcpy(from.data(), payload->Data, static_cast<usize>(payload->DataSize));
            // Onto a container: inside it, last; onto anything else: just before it.
            std::string error;
            std::optional<WidgetPath> to;
            if (w.MaxChildren() != 0) to = doc_.Move(from, path, w.ChildCount(), &error);
            else if (!path.empty()) {
                WidgetPath parent(path.begin(), path.end() - 1);
                usize index = path.back();
                if (from.size() == path.size() && std::equal(parent.begin(), parent.end(), from.begin()) && from.back() < index) --index;
                to = doc_.Move(from, parent, index, &error);
            }
            if (to) Select(*to);
            else status_ = error;
            moved = true;
        }
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kTypePayload)) {
            const std::string type(static_cast<const char*>(payload->Data));
            std::string error;
            if (const auto added = doc_.AddWidget(type, path, -1, &error)) Select(*added);
            else status_ = error;
            moved = true;
        }
        ImGui::EndDragDropTarget();
    }
    if (open) {
        // The tree may have changed under us: stop drawing it this frame.
        for (usize i = 0; !moved && i < w.ChildCount(); ++i) {
            path.push_back(i);
            DrawHierarchyNode(*w.Child(i), path);
            path.pop_back();
        }
        ImGui::TreePop();
    }
    ImGui::PopID();
}

void UIDesigner::DrawCanvas() {
    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    origin_ = V(ImGui::GetCursorScreenPos());
    canvas_size_ = {std::max(avail.x, 50.0f), std::max(avail.y, 50.0f)};
    if (fit_) Fit(canvas_size_);
    ImGui::InvisibleButton("##canvas", Im(canvas_size_), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered();
    // A palette entry dropped on the canvas.
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kTypePayload)) {
            PlaceWidget(static_cast<const char*>(payload->Data), ScreenToLayout(V(io.MousePos)));
        }
        ImGui::EndDragDropTarget();
    }
    Refresh();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 c0 = Im(origin_), c1{origin_.x + canvas_size_.x, origin_.y + canvas_size_.y};
    dl->PushClipRect(c0, c1, true);
    dl->AddRectFilled(c0, c1, IM_COL32(28, 28, 32, 255));
    const f32 k = viewport_.Scale();
    const Vec2 res = viewport_.Size();
    const Vec2 screen0 = LayoutToScreen({0, 0}), screen1 = LayoutToScreen({res.x / k, res.y / k});
    dl->AddRectFilled(Im(screen0), Im(screen1), IM_COL32(48, 48, 56, 255));
    const Rect safe = PreviewArea();
    if (safe.w * k < res.x - 0.5f || safe.h * k < res.y - 0.5f) {
        dl->AddRect(Im(LayoutToScreen({safe.x, safe.y})), Im(LayoutToScreen({safe.Right(), safe.Bottom()})), IM_COL32(255, 200, 60, 160), 0.0f, 0, 1.0f);
    }
    const f32 s = k * zoom_;
    if (preview_) {
        // The quads (solid; images as their tint), then text over them.
        ui::DrawList list;
        preview_->Paint(list, ui::PaintContext{&kFont, 1.0f, nullptr});
        for (const ui::DrawQuad& q : list.quads) {
            const Rect& clip = list.clips[q.clip];
            const ImVec2 cl0 = Im(LayoutToScreen({clip.x, clip.y})), cl1 = Im(LayoutToScreen({clip.Right(), clip.Bottom()}));
            dl->PushClipRect(cl0, cl1, true);
            dl->AddRectFilled(Im(LayoutToScreen({q.rect.x, q.rect.y})), Im(LayoutToScreen({q.rect.Right(), q.rect.Bottom()})), Col(q.color));
            dl->PopClipRect();
        }
        std::function<void(const Widget&, f32)> text = [&](const Widget& w, f32 opacity) {
            if (!Visible(w)) return;
            opacity *= w.opacity;
            const Rect g = w.Geometry();
            std::string str;
            f32 size = 0.0f;
            ui::Color color;
            if (const auto* t = dynamic_cast<const ui::Text*>(&w)) str = t->Shown(), size = t->size, color = t->color;
            else if (const auto* ti = dynamic_cast<const ui::TextInput*>(&w)) str = ti->text.empty() ? ti->ShownHint() : ti->text, size = ti->style.text_size, color = ti->style.text;
            if (!str.empty() && size * s >= 3.0f) {
                const Vec2 at = LayoutToScreen({g.x, g.y});
                dl->AddText(nullptr, size * s, Im(at), Col(color.WithAlpha(opacity)), str.c_str());
            }
            for (usize i = 0; i < w.ChildCount(); ++i) text(*w.Child(i), opacity);
        };
        text(*preview_, 1.0f);

        // Hover and selection.
        const WidgetPath under = hovered ? PickAt(ScreenToLayout(V(io.MousePos))) : WidgetPath{};
        if (hovered && !under.empty()) {
            if (const Widget* w = PreviewWidget(under)) {
                const Rect g = w->Geometry();
                dl->AddRect(Im(LayoutToScreen({g.x, g.y})), Im(LayoutToScreen({g.Right(), g.Bottom()})), IM_COL32(120, 180, 255, 120));
            }
        }
        for (const WidgetPath& p : selection_) {
            const Widget* w = PreviewWidget(p);
            if (w == nullptr) continue;
            const Rect g = w->Geometry();
            const Vec2 a = LayoutToScreen({g.x, g.y}), b = LayoutToScreen({g.Right(), g.Bottom()});
            dl->AddRect(Im(a), Im(b), IM_COL32(255, 150, 40, 255), 0.0f, 0, 2.0f);
            if (selection_.size() == 1 && doc_.InCanvas(p)) {
                const Vec2 m{(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f};
                const Vec2 points[8] = {{a.x, a.y}, {m.x, a.y}, {b.x, a.y}, {b.x, m.y}, {b.x, b.y}, {m.x, b.y}, {a.x, b.y}, {a.x, m.y}};
                for (const Vec2& h : points) dl->AddRectFilled({h.x - kHandle, h.y - kHandle}, {h.x + kHandle, h.y + kHandle}, IM_COL32(255, 150, 40, 255));
                // The anchors, in the parent canvas.
                const ui::Anchors& an = w->slot.anchors;
                const Rect pg = w->Parent() != nullptr ? w->Parent()->Geometry() : Rect{};
                for (const Vec2 f : {Vec2{an.min_x, an.min_y}, Vec2{an.max_x, an.min_y}, Vec2{an.min_x, an.max_y}, Vec2{an.max_x, an.max_y}}) {
                    const Vec2 at = LayoutToScreen({pg.x + f.x * pg.w, pg.y + f.y * pg.h});
                    dl->AddTriangleFilled({at.x, at.y - 6}, {at.x + 6, at.y}, {at.x - 6, at.y}, IM_COL32(120, 255, 160, 220));
                }
            }
        }
    }
    dl->PopClipRect();

    // Gestures.
    const Vec2 mouse = V(io.MousePos);
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        drag_start_ = mouse;
        drag_rects_.clear();
        ++drag_id_;
        if (const int h = HandleAt(mouse); h >= 0) {
            drag_ = Drag::Resize;
            handle_ = h;
            drag_rects_.push_back({*Primary(), doc_.GeometryOf(*Primary())});
        } else {
            const WidgetPath path = PickAt(ScreenToLayout(mouse));
            const bool selected = std::find(selection_.begin(), selection_.end(), path) != selection_.end();
            if (io.KeyCtrl) Select(path, true);
            else if (!selected) Select(path);
            drag_ = Drag::Move;
            for (const WidgetPath& p : CanvasSelection()) drag_rects_.push_back({p, doc_.GeometryOf(p)});
        }
    }
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) {
        drag_ = Drag::Pan;
        drag_start_ = mouse;
        drag_rects_ = {{WidgetPath{}, Rect{pan_.x, pan_.y, 0, 0}}};
    }
    const bool held = drag_ == Drag::Pan ? ImGui::IsMouseDown(ImGuiMouseButton_Middle) : ImGui::IsMouseDown(ImGuiMouseButton_Left);
    if (drag_ != Drag::None && drag_ != Drag::Key && drag_ != Drag::Scrub && !held) drag_ = Drag::None;
    const Vec2 d{(mouse.x - drag_start_.x) / s, (mouse.y - drag_start_.y) / s};
    const bool moved = mouse.x != drag_start_.x || mouse.y != drag_start_.y;
    if (drag_ == Drag::Move && moved) {
        for (const auto& [p, r0] : drag_rects_) doc_.PlaceInCanvas(p, Snapped({r0.x + d.x, r0.y + d.y, r0.w, r0.h}), "drag" + std::to_string(drag_id_));
    } else if (drag_ == Drag::Resize && moved && !drag_rects_.empty()) {
        const Rect r0 = drag_rects_[0].second;
        f32 x0 = r0.x, y0 = r0.y, x1 = r0.Right(), y1 = r0.Bottom();
        const bool left = handle_ == 0 || handle_ == 6 || handle_ == 7, right = handle_ >= 2 && handle_ <= 4;
        const bool top = handle_ <= 2, bottom = handle_ >= 4 && handle_ <= 6;
        auto sn = [&](f32 v) { return snap && grid > 0.0f ? std::round(v / grid) * grid : v; };
        if (left) x0 = std::min(sn(x0 + d.x), x1 - 1.0f);
        if (right) x1 = std::max(sn(x1 + d.x), x0 + 1.0f);
        if (top) y0 = std::min(sn(y0 + d.y), y1 - 1.0f);
        if (bottom) y1 = std::max(sn(y1 + d.y), y0 + 1.0f);
        doc_.PlaceInCanvas(drag_rects_[0].first, {x0, y0, x1 - x0, y1 - y0}, "resize" + std::to_string(drag_id_));
    } else if (drag_ == Drag::Pan && !drag_rects_.empty()) {
        pan_ = {drag_rects_[0].second.x + mouse.x - drag_start_.x, drag_rects_[0].second.y + mouse.y - drag_start_.y};
    }
    if (hovered && io.MouseWheel != 0.0f) {
        // Zoom about the cursor.
        const Vec2 at = ScreenToLayout(mouse);
        SetZoom(zoom_ * std::pow(1.1f, io.MouseWheel));
        pan_ = {mouse.x - origin_.x - at.x * k * zoom_, mouse.y - origin_.y - at.y * k * zoom_};
    }
}

void UIDesigner::DrawDetails() {
    const std::optional<WidgetPath> p = Primary();
    if (!p) {
        ImGui::TextDisabled("Select a widget");
        return;
    }
    const WidgetPath path = *p;
    const Widget* w = doc_.At(path);
    ImGui::Text("%s", w->TypeName());
    if (name_edit_for_ != path || !ImGui::IsAnyItemActive()) {
        name_edit_ = w->name;
        name_edit_for_ = path;
    }
    if (ImGui::InputText("Name", &name_edit_, ImGuiInputTextFlags_EnterReturnsTrue)) {
        std::string error;
        if (!doc_.Rename(path, name_edit_, &error)) status_ = error;
    }
    if (ImGui::CollapsingHeader("Properties", ImGuiTreeNodeFlags_DefaultOpen)) {
        const json props = doc_.EditableProperties(path);
        for (const auto& [key, value] : props.items()) {
            if (key == "type" || key == "name" || key == "slot" || key == "navigation") continue;
            json v = value;
            bool dragging = false;
            ImGui::PushID(key.c_str());
            if (EditJsonField(key, v, dragging, &Enums())) {
                std::string error;
                if (!doc_.SetProperty(path, key, v, &error, dragging ? "prop:" + key : std::string())) status_ = error;
            }
            ImGui::PopID();
        }
    }
    if (!path.empty() && ImGui::CollapsingHeader("Slot", ImGuiTreeNodeFlags_DefaultOpen)) DrawSlot(path);
    if (!doc_.At(path)->name.empty() && ImGui::CollapsingHeader("Bindings", ImGuiTreeNodeFlags_DefaultOpen)) DrawBindings(path);
}

void UIDesigner::DrawSlot(const WidgetPath& path) {
    Widget* w = doc_.At(path);
    ui::Slot s = w->slot;
    bool changed = false;
    std::string field;
    auto drag2 = [&](const char* label, Vec2& v) {
        f32 f[2] = {v.x, v.y};
        if (ImGui::DragFloat2(label, f, 0.5f)) v = {f[0], f[1]}, changed = true, field = label;
    };
    const Widget* parent = w->Parent();
    if (dynamic_cast<const ui::Canvas*>(parent) != nullptr) {
        if (ImGui::BeginCombo("Anchors", "Preset...")) {
            for (const AnchorPreset& a : kAnchorPresets) {
                if (ImGui::Selectable(a.name)) doc_.SetAnchors(path, a.anchors, a.alignment);
            }
            ImGui::EndCombo();
        }
        f32 an[4] = {s.anchors.min_x, s.anchors.min_y, s.anchors.max_x, s.anchors.max_y};
        if (ImGui::DragFloat4("Anchor min/max", an, 0.01f, 0.0f, 1.0f)) {
            doc_.SetAnchors(path, {an[0], an[1], std::max(an[0], an[2]), std::max(an[1], an[3])});
            return;
        }
        drag2("Position", s.position);
        drag2("Size", s.size);
        drag2("Alignment", s.alignment);
        f32 m[4] = {s.margins.left, s.margins.top, s.margins.right, s.margins.bottom};
        if (ImGui::DragFloat4("Margins", m, 0.5f)) s.margins = {m[0], m[1], m[2], m[3]}, changed = true, field = "margins";
        if (ImGui::Checkbox("Auto size", &s.auto_size)) changed = true;
        if (ImGui::DragInt("Z", &s.z)) changed = true, field = "z";
    } else if (dynamic_cast<const ui::Grid*>(parent) != nullptr) {
        int v[4] = {static_cast<int>(s.row), static_cast<int>(s.column), static_cast<int>(s.row_span), static_cast<int>(s.column_span)};
        if (ImGui::DragInt4("Row/col/spans", v, 0.1f, 0, 64)) {
            s.row = static_cast<u32>(std::max(v[0], 0)), s.column = static_cast<u32>(std::max(v[1], 0));
            s.row_span = static_cast<u32>(std::max(v[2], 1)), s.column_span = static_cast<u32>(std::max(v[3], 1));
            changed = true, field = "grid";
        }
    }
    if (dynamic_cast<const ui::Canvas*>(parent) == nullptr) {
        f32 pad[4] = {s.padding.left, s.padding.top, s.padding.right, s.padding.bottom};
        if (ImGui::DragFloat4("Padding", pad, 0.5f)) s.padding = {pad[0], pad[1], pad[2], pad[3]}, changed = true, field = "padding";
        if (dynamic_cast<const ui::BoxPanel*>(parent) != nullptr && ImGui::DragFloat("Fill", &s.fill, 0.05f, 0.0f, 100.0f)) changed = true, field = "fill";
        int h = static_cast<int>(s.h_align), v = static_cast<int>(s.v_align);
        const char* hs[] = {"Fill", "Left", "Center", "Right"};
        const char* vs[] = {"Fill", "Top", "Center", "Bottom"};
        if (ImGui::Combo("Horizontal", &h, hs, 4)) s.h_align = static_cast<ui::HAlign>(h), changed = true;
        if (ImGui::Combo("Vertical", &v, vs, 4)) s.v_align = static_cast<ui::VAlign>(v), changed = true;
    }
    if (changed) {
        const bool dragging = ImGui::IsMouseDragging(0) && !field.empty();
        doc_.Edit("Slot", [&](ui::LayoutDocument&) { w->slot = s; }, dragging ? "slot:" + field + PathKey(path) : std::string());
    }
}

void UIDesigner::DrawBindings(const WidgetPath& path) {
    const Widget* w = doc_.At(path);
    const std::vector<std::string> props = UILayoutDocument::BindableProperties(*w);
    for (usize index : doc_.BindingsOf(w->name)) {
        ui::Binding b = doc_.Bindings()[index];
        ImGui::PushID(static_cast<int>(index));
        bool changed = false;
        if (ImGui::BeginCombo("Property", b.property.c_str())) {
            for (const std::string& p : props) {
                if (ImGui::Selectable(p.c_str(), p == b.property)) b.property = p, changed = true;
            }
            ImGui::EndCombo();
        }
        changed |= ImGui::InputText("Source", &b.source, ImGuiInputTextFlags_EnterReturnsTrue);
        changed |= ImGui::InputText("Divide by", &b.divide_by, ImGuiInputTextFlags_EnterReturnsTrue);
        if (b.property == "text") {
            changed |= ImGui::InputText("Format", &b.format, ImGuiInputTextFlags_EnterReturnsTrue);
            changed |= ImGui::SliderInt("Precision", &b.precision, 0, 6);
        }
        if (b.property == "visible" || b.property == "enabled" || b.property == "checked") changed |= ImGui::Checkbox("Invert", &b.invert);
        changed |= ImGui::Checkbox("Two-way", &b.two_way);
        if (changed) doc_.SetBinding(index, b);
        if (ImGui::SmallButton("Remove binding")) doc_.RemoveBinding(index);
        ImGui::Separator();
        ImGui::PopID();
    }
    if (ImGui::Button("Add binding")) {
        ui::Binding b;
        b.widget = w->name;
        b.property = props.size() > 3 ? props[3] : props[0]; // the widget's own property first
        doc_.AddBinding(b);
    }
}

void UIDesigner::DrawTimeline() {
    const auto& animations = doc_.Animations();
    ImGui::SetNextItemWidth(160);
    if (ImGui::BeginCombo("##animation", animation_.empty() ? "(no animation)" : animation_.c_str())) {
        for (const ui::UIAnimation& a : animations) {
            if (ImGui::Selectable(a.name.c_str(), a.name == animation_)) SelectAnimation(a.name);
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button("New")) SelectAnimation(doc_.AddAnimation("Animation"));
    const ui::UIAnimation* a = doc_.FindAnimation(animation_);
    if (a == nullptr) return;
    ImGui::SameLine();
    if (ImGui::Button("Delete")) {
        doc_.RemoveAnimation(animation_);
        SelectAnimation({});
        return;
    }
    ImGui::SameLine();
    if (ImGui::Button(playing_ ? "Stop" : "Play")) playing_ = !playing_;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90);
    f32 t = playhead_;
    if (ImGui::DragFloat("Time", &t, 0.01f, 0.0f, 600.0f, "%.2f s")) SetPlayhead(t);
    ImGui::SameLine();
    ImGui::TextDisabled("length %.2f s", a->Duration());
    // Add a track.
    ImGui::SetNextItemWidth(140);
    if (ImGui::BeginCombo("##track_widget", new_track_widget_.empty() ? "widget" : new_track_widget_.c_str())) {
        std::function<void(const Widget&)> names = [&](const Widget& w) {
            if (!w.name.empty() && ImGui::Selectable(w.name.c_str(), w.name == new_track_widget_)) new_track_widget_ = w.name;
            for (usize i = 0; i < w.ChildCount(); ++i) names(*w.Child(i));
        };
        names(doc_.Root());
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110);
    if (ImGui::BeginCombo("##track_property", new_track_property_.c_str())) {
        for (const std::string& p : ui::AnimatableProperties()) {
            if (ImGui::Selectable(p.c_str(), p == new_track_property_)) new_track_property_ = p;
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button("Add track")) {
        std::string error;
        if (!doc_.AddTrack(animation_, new_track_widget_, new_track_property_, &error)) status_ = error;
    }

    // Lanes: names on the left, keys on a time ruler, the playhead over them.
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 top = ImGui::GetCursorScreenPos();
    const f32 lane_x = top.x + kLabelWidth, row_h = 20.0f;
    const f32 width = std::max(ImGui::GetContentRegionAvail().x - kLabelWidth, 50.0f);
    auto time_x = [&](f32 time) { return lane_x + time * kPixelsPerSecond; };
    ImGuiIO& io = ImGui::GetIO();
    // Ruler.
    ImGui::InvisibleButton("##ruler", ImVec2(kLabelWidth + width, row_h));
    if (ImGui::IsItemActive() && io.MousePos.x >= lane_x) SetPlayhead((io.MousePos.x - lane_x) / kPixelsPerSecond);
    for (f32 s = 0.0f; time_x(s) < lane_x + width; s += 0.5f) {
        dl->AddLine({time_x(s), top.y + row_h - 6}, {time_x(s), top.y + row_h}, IM_COL32(150, 150, 150, 255));
        if (std::fmod(s, 1.0f) == 0.0f) dl->AddText({time_x(s) + 2, top.y}, IM_COL32(170, 170, 170, 255), (std::to_string(static_cast<int>(s)) + "s").c_str());
    }
    bool key_hit = false;
    for (usize ti = 0; ti < a->tracks.size(); ++ti) {
        const ui::UITrack& track = a->tracks[ti];
        ImGui::PushID(static_cast<int>(ti));
        const ImVec2 row = ImGui::GetCursorScreenPos();
        ImGui::AlignTextToFramePadding();
        ImGui::Text("%s.%s", track.widget.c_str(), track.property.c_str());
        ImGui::SameLine(kLabelWidth - 60);
        if (ImGui::SmallButton("Key")) KeyAtPlayhead(ti);
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) {
            doc_.RemoveTrack(animation_, ti);
            ImGui::PopID();
            break;
        }
        const f32 cy = row.y + row_h * 0.5f;
        dl->AddLine({lane_x, cy}, {lane_x + width, cy}, IM_COL32(70, 70, 80, 255));
        for (usize ki = 0; ki < track.keys.size(); ++ki) {
            const f32 kx = time_x(track.keys[ki].time);
            const bool active = key_track_ == static_cast<i64>(ti) && key_index_ == static_cast<i64>(ki);
            dl->AddQuadFilled({kx, cy - 6}, {kx + 6, cy}, {kx, cy + 6}, {kx - 6, cy}, active ? IM_COL32(255, 200, 80, 255) : IM_COL32(220, 220, 220, 255));
            const bool over = std::abs(io.MousePos.x - kx) <= 6 && std::abs(io.MousePos.y - cy) <= 6;
            if (over && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                key_track_ = static_cast<i64>(ti), key_index_ = static_cast<i64>(ki);
                drag_ = Drag::Key;
                ++drag_id_;
                key_hit = true;
            }
            if (over && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                doc_.RemoveKey(animation_, ti, ki);
                key_track_ = key_index_ = -1;
                break;
            }
        }
        ImGui::PopID();
    }
    // Dragging a key along its lane.
    if (drag_ == Drag::Key) {
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            drag_ = Drag::None;
        } else if (!key_hit && key_track_ >= 0 && io.MouseDelta.x != 0.0f) {
            f32 time = std::max((io.MousePos.x - lane_x) / kPixelsPerSecond, 0.0f);
            time = std::round(time * 100.0f) / 100.0f; // hundredths of a second
            key_index_ = doc_.MoveKey(animation_, static_cast<usize>(key_track_), static_cast<usize>(key_index_), time, "key" + std::to_string(drag_id_));
            if (key_index_ < 0) drag_ = Drag::None;
        }
    }
    const f32 px = time_x(playhead_);
    dl->AddLine({px, top.y}, {px, ImGui::GetCursorScreenPos().y}, IM_COL32(255, 90, 90, 255), 2.0f);
}

void UIDesigner::DrawDiagnostics() {
    const auto& diagnostics = doc_.Diagnostics();
    if (diagnostics.empty()) ImGui::TextDisabled("No problems");
    for (usize i = 0; i < diagnostics.size(); ++i) {
        const LayoutDiagnostic& d = diagnostics[i];
        ImGui::PushID(static_cast<int>(i));
        ImGui::PushStyleColor(ImGuiCol_Text, d.error ? IM_COL32(255, 120, 110, 255) : IM_COL32(240, 200, 90, 255));
        if (ImGui::Selectable((d.code + "  " + d.message).c_str()) && doc_.At(d.path) != nullptr) Select(d.path);
        ImGui::PopStyleColor();
        ImGui::PopID();
    }
}

void UIDesigner::HandleKeys() {
    ImGuiIO& io = ImGui::GetIO();
    if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) || io.WantTextInput) return;
    const bool ctrl = io.KeyCtrl;
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Z, false)) io.KeyShift ? (void)doc_.Redo() : (void)doc_.Undo();
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Y, false)) doc_.Redo();
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) SaveNow();
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_D, false)) DuplicateSelection();
    if (ImGui::IsKeyPressed(ImGuiKey_Delete, false)) DeleteSelection();
    if (!ctrl && ImGui::IsKeyPressed(ImGuiKey_F, false)) fit_ = true;
    const f32 step = io.KeyShift ? std::max(grid, 1.0f) : 1.0f;
    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) Nudge({-step, 0});
    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) Nudge({step, 0});
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) Nudge({0, -step});
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) Nudge({0, step});
}

} // namespace aether::editor
