#include "test_framework.h"
#include "uidesign/ui_designer.h"

#include "aether/ui/basic.h"
#include "aether/ui/controls.h"
#include "aether/ui/panels.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <functional>

using namespace aether;
using namespace aether::editor;
using ui::Rect;
using ui::Vec2;

// Phase 18 step 6: the UI Designer's layout document (tree edits, names
// that bindings and tracks follow, generic properties, Canvas placement and
// anchors, bindings, animation keys, diagnostics, undo, files) and the
// designer itself, drawn headless (placing, selecting, moving and resizing
// with the mouse, resolutions and safe areas, themes, the timeline).

// Braced initializers carry commas, so the check takes them all.
#define CHECK(...) AETHER_CHECK((__VA_ARGS__))

namespace {

bool Near(f32 a, f32 b, f32 tol = 1e-3f) { return std::fabs(a - b) <= tol; }
bool Near(const Rect& a, const Rect& b, f32 tol = 1e-3f) { return Near(a.x, b.x, tol) && Near(a.y, b.y, tol) && Near(a.w, b.w, tol) && Near(a.h, b.h, tol); }
const ui::BuiltinFont kFont;

class HeadlessImGui {
public:
    HeadlessImGui() {
        context_ = ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(1600, 1000);
        io.DeltaTime = 1.0f / 60.0f;
        unsigned char* pixels = nullptr;
        int w = 0, h = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
    }
    ~HeadlessImGui() { ImGui::DestroyContext(context_); }
    void Frame(const std::function<void()>& body) {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(1600, 1000));
        ImGui::Begin("Designer", nullptr, ImGuiWindowFlags_NoMove);
        body();
        ImGui::End();
        ImGui::Render();
    }

private:
    ImGuiContext* context_ = nullptr;
};

bool Has(const std::vector<LayoutDiagnostic>& d, const char* code) {
    return std::any_of(d.begin(), d.end(), [&](const LayoutDiagnostic& x) { return x.code == code; });
}

} // namespace

AETHER_TEST(UIDesigner_LayoutDocument) {
    UILayoutDocument doc;
    CHECK(doc.Root().name == "Root" && std::string(doc.Root().TypeName()) == "Canvas" && !doc.Dirty() && doc.Name() == "Untitled");
    std::string error;
    // Adding: unique names, only where they fit.
    const auto play = doc.AddWidget("Button", {});
    const auto quit = doc.AddWidget("Button", {});
    CHECK(play == WidgetPath{0} && quit == WidgetPath{1} && doc.At(*play)->name == "Button1" && doc.At(*quit)->name == "Button2");
    const auto label = doc.AddWidget("Text", *play);
    CHECK(label == (WidgetPath{0, 0}) && doc.At(*label)->name == "Text1" && doc.Dirty());
    CHECK(!doc.AddWidget("Text", *play, -1, &error) && error.find("another child") != std::string::npos);
    CHECK(!doc.AddWidget("Text", *label, -1, &error) && error.find("can't take children") != std::string::npos);
    CHECK(!doc.AddWidget("Gizmo", {}, -1, &error) && error.find("unknown widget type") != std::string::npos);
    CHECK(doc.AddWidget("Spacer", {}, 0) == WidgetPath{0} && doc.At({1})->name == "Button1"); // at an index
    CHECK(doc.UndoLabel() == "Add Spacer" && doc.Undo() && doc.At({0})->name == "Button1" && doc.CanRedo() && doc.Redo() && doc.Undo());
    CHECK(doc.UniqueName("Button1") == "Button3" && doc.UniqueName("Menu") == "Menu" && doc.PathOf("Text1") == (WidgetPath{0, 0}));

    // Renaming: unique, and bindings and tracks follow.
    CHECK(doc.Rename(*play, "Play") && !doc.Rename(*quit, "Play", &error) && error.find("another widget") != std::string::npos);
    ui::Binding enabled;
    enabled.widget = "Play";
    enabled.property = "enabled";
    enabled.source = "game.can_play";
    doc.AddBinding(enabled);
    CHECK(doc.AddAnimation("Intro") == "Intro" && doc.AddAnimation("Intro") == "Intro1" && doc.RemoveAnimation("Intro1"));
    CHECK(doc.AddTrack("Intro", "Play", "opacity") && !doc.AddTrack("Intro", "Play", "opacity", &error) && error.find("already") != std::string::npos);
    CHECK(!doc.AddTrack("Intro", "Nobody", "opacity", &error) && !doc.AddTrack("Intro", "Play", "wobble", &error) && error.find("can't be animated") != std::string::npos);
    CHECK(doc.Rename(*play, "Start") && doc.Bindings()[0].widget == "Start" && doc.FindAnimation("Intro")->tracks[0].widget == "Start");
    CHECK(!doc.Rename(*play, "", &error) && error.find("needs a name") != std::string::npos);
    CHECK(doc.Undo() && doc.At(*play)->name == "Play" && doc.Bindings()[0].widget == "Play" && doc.Redo());

    // Properties through the widget's saved form: any type, with its children kept.
    CHECK(doc.SetProperty(*label, "text", "Start game") && static_cast<ui::Text*>(doc.At(*label))->text == "Start game");
    CHECK(doc.SetProperty(*label, "justify", "Center") && static_cast<ui::Text*>(doc.At(*label))->justify == ui::TextAlign::Center);
    CHECK(!doc.SetProperty(*label, "justify", "Middle", &error) && error.find("Middle") != std::string::npos);
    CHECK(doc.SetProperty(*label, "color", {1.0f, 0.0f, 0.0f, 1.0f}) && static_cast<ui::Text*>(doc.At(*label))->color.g == 0.0f);
    CHECK(doc.SetProperty(*play, "padding", {1, 2, 3, 4}) && static_cast<ui::Button*>(doc.At(*play))->padding.right == 3.0f);
    CHECK(doc.At(*play)->ChildCount() == 1 && static_cast<ui::Text*>(doc.At(*label))->text == "Start game"); // its child came along
    CHECK(doc.SetProperty({}, "clip", true) && doc.Root().clip_children);
    CHECK(doc.SetProperty(*play, "name", "Begin") && doc.At(*play)->name == "Begin" && doc.Bindings()[0].widget == "Begin");
    CHECK(!doc.SetProperty(*play, "children", nlohmann::json::array(), &error) && !doc.SetProperty(*play, "type", "Text", &error));
    const u64 before = doc.Revision();
    CHECK(doc.SetProperty(*label, "wrap_width", 0.0f) && doc.Revision() == before); // already so: no step
    const nlohmann::json text_props = doc.EditableProperties(*label), button_props = doc.EditableProperties(*play);
    CHECK(text_props["justify"] == "Center" && text_props["wrap_width"] == 0.0f && text_props["visibility"] == "Visible" && !text_props.contains("focusable"));
    CHECK(button_props["focusable"] == true && !button_props.contains("children") && doc.PropertiesOf(*play).contains("padding"));
    CHECK(doc.SetProperty(*play, "focusable", false) && !static_cast<ui::Button*>(doc.At(*play))->focusable);

    // Canvas placement: rectangles become slots under the anchors, and anchors change without moving it.
    doc.Layout({0, 0, 1920, 1080}, kFont);
    CHECK(doc.InCanvas(*quit) && !doc.InCanvas(*label) && !doc.InCanvas({}));
    CHECK(doc.PlaceInCanvas(*quit, {100, 200, 300, 50}));
    ui::Slot s = doc.At(*quit)->slot;
    CHECK(s.position == Vec2{100, 200} && s.size == Vec2{300, 50});
    doc.Layout({0, 0, 1920, 1080}, kFont);
    CHECK(Near(doc.GeometryOf(*quit), {100, 200, 300, 50}));
    CHECK(doc.SetAnchors(*quit, ui::Anchors::Point(1, 1), Vec2{1, 1}));
    s = doc.At(*quit)->slot;
    CHECK(Near(s.position.x, -1520) && Near(s.position.y, -830) && s.alignment == Vec2{1, 1});
    doc.Layout({0, 0, 1920, 1080}, kFont);
    CHECK(Near(doc.GeometryOf(*quit), {100, 200, 300, 50}));
    doc.Layout({0, 0, 1280, 720}, kFont); // stays in the bottom-right corner
    CHECK(Near(doc.GeometryOf(*quit), {1280 - 1820, 720 - 880, 300, 50}));
    doc.Layout({0, 0, 1920, 1080}, kFont);
    CHECK(doc.SetAnchors(*quit, ui::Anchors::Stretch()));
    s = doc.At(*quit)->slot;
    CHECK(Near(s.margins.left, 100) && Near(s.margins.top, 200) && Near(s.margins.right, 1520) && Near(s.margins.bottom, 830));
    CHECK(doc.PlaceInCanvas(*quit, {50, 60, 500, 100}));
    doc.Layout({0, 0, 1920, 1080}, kFont);
    CHECK(Near(doc.GeometryOf(*quit), {50, 60, 500, 100}) && !doc.PlaceInCanvas(*label, {0, 0, 1, 1}));
    // A drag is one undo step.
    CHECK(doc.SetAnchors(*quit, ui::Anchors::Point(0, 0), Vec2{0, 0}));
    doc.Layout({0, 0, 1920, 1080}, kFont);
    for (int i = 1; i <= 5; ++i) doc.PlaceInCanvas(*quit, {50.0f + i * 10.0f, 60, 500, 100}, "drag1");
    CHECK(doc.At(*quit)->slot.position.x == 100.0f && doc.Undo() && doc.At(*quit)->slot.position.x == 50.0f && doc.Redo());
    // Auto-sized text stays so when moved, and stops when resized.
    const auto note = doc.AddWidget("Text", {}, -1, nullptr, [](ui::Widget& w) { w.slot.auto_size = true; });
    doc.Layout({0, 0, 1920, 1080}, kFont);
    Rect g = doc.GeometryOf(*note);
    CHECK(doc.PlaceInCanvas(*note, {g.x + 10, g.y, g.w, g.h}) && doc.At(*note)->slot.auto_size);
    CHECK(doc.PlaceInCanvas(*note, {g.x, g.y, g.w + 40, g.h}) && !doc.At(*note)->slot.auto_size);

    // Moving and reordering.
    CHECK(!doc.Move(*note, *play, 0, &error) && error.find("another child") != std::string::npos);
    const auto box = doc.AddWidget("VerticalBox", {});
    CHECK(box == WidgetPath{3});
    const auto moved = doc.Move(*note, *box, 0);
    CHECK(moved == (WidgetPath{2, 0}) && doc.At(*moved)->name == "Text2" && doc.At({2})->ChildCount() == 1);
    CHECK(!doc.Move({2}, {2, 0}, 0, &error) && error.find("inside itself") != std::string::npos && !doc.Move({}, {2}, 0, &error));
    CHECK(doc.Move({0}, {}, 5) == WidgetPath{2} && doc.At({2})->name == "Begin" && doc.Undo() && doc.At({0})->name == "Begin");

    // Duplicating renames the copy and everything in it.
    const auto copy = doc.Duplicate({0});
    CHECK(copy == WidgetPath{1} && doc.At(*copy)->name == "Begin1" && doc.At(*copy)->Child(0)->name == "Text3");
    CHECK(doc.At(*copy)->slot.position.x == doc.At({0})->slot.position.x + 16.0f && !doc.Duplicate({}, &error));

    // Bindings and what widgets can bind.
    CHECK(doc.BindingsOf("Begin") == std::vector<usize>{0} && doc.BindingsOf("").empty());
    ui::Binding changed = doc.Bindings()[0];
    changed.invert = true;
    CHECK(doc.SetBinding(0, changed) && doc.Bindings()[0].invert && !doc.SetBinding(9, changed) && !doc.RemoveBinding(9));
    const std::vector<std::string> text_bindable = UILayoutDocument::BindableProperties(*doc.At({0, 0}));
    CHECK(std::find(text_bindable.begin(), text_bindable.end(), "text") != text_bindable.end());
    const std::vector<std::string> bar_bindable = UILayoutDocument::BindableProperties(ui::ProgressBar{});
    CHECK(std::find(bar_bindable.begin(), bar_bindable.end(), "percent") != bar_bindable.end() &&
                 std::find(bar_bindable.begin(), bar_bindable.end(), "text") == bar_bindable.end());

    // Keys: in time order; one at a key's time replaces it; moving re-sorts.
    CHECK(doc.SetKey("Intro", 0, 0.5f, 0.5f) && doc.SetKey("Intro", 0, 0.0f, 0.0f) && doc.SetKey("Intro", 0, 1.0f, 1.0f));
    CHECK(doc.SetKey("Intro", 0, 0.5f, 0.7f, ui::Ease::EaseOut) && !doc.SetKey("Intro", 0, -1.0f, 0.0f) && !doc.SetKey("Intro", 3, 0.0f, 0.0f));
    const auto& keys = [&]() -> const std::vector<ui::UIKey>& { return doc.FindAnimation("Intro")->tracks[0].keys; };
    CHECK(keys().size() == 3 && keys()[1].value == 0.7f && keys()[1].ease == ui::Ease::EaseOut && Near(doc.FindAnimation("Intro")->Duration(), 1.0f));
    CHECK(doc.MoveKey("Intro", 0, 0, 2.0f) == 2 && keys()[2].time == 2.0f && keys()[2].value == 0.0f && keys()[0].time == 0.5f);
    CHECK(doc.MoveKey("Intro", 0, 0, 1.0f) == 0 && keys().size() == 2); // landed on the 1 s key
    CHECK(doc.RemoveKey("Intro", 0, 0) && keys().size() == 1 && !doc.RemoveKey("Intro", 0, 5) && doc.MoveKey("Intro", 0, 7, 0) == -1);
    CHECK(doc.RenameAnimation("Intro", "Show") && doc.FindAnimation("Show") != nullptr && !doc.RenameAnimation("Show", "", &error));
    doc.AddAnimation("Empty");
    CHECK(!doc.RenameAnimation("Show", "Empty", &error) && error.find("already") != std::string::npos);

    // Diagnostics.
    CHECK(doc.ErrorCount() == 0 && Has(doc.Diagnostics(), "UD007")); // "Empty" has no keys: a warning
    doc.Edit("break it", [](ui::LayoutDocument& d) {
        d.root->Child(1)->name = "Begin";                         // UD001
        d.bindings.push_back({"Ghost", "text", "x.y"});           // UD002
        d.bindings.push_back({"Text3", "percent", "x.y"});        // UD003
        d.bindings.push_back({"Text3", "text", ""});              // UD004
        d.animations[0].tracks.push_back({"Ghost", "opacity", {{0, 1}}}); // UD005
        d.animations[0].tracks.push_back(d.animations[0].tracks[0]);     // UD008
    });
    const auto& diagnostics = doc.Diagnostics();
    for (const char* code : {"UD001", "UD002", "UD003", "UD004", "UD005", "UD007", "UD008"}) CHECK(Has(diagnostics, code));
    CHECK(doc.ErrorCount() == 5);
    const auto dup = std::find_if(diagnostics.begin(), diagnostics.end(), [](const LayoutDiagnostic& d) { return d.code == "UD001"; });
    CHECK(dup->path == WidgetPath{1});
    CHECK(doc.Undo() && doc.ErrorCount() == 0);

    // Deleting drops what referred to it; nested selections delete once; not the root.
    CHECK(!doc.DeleteWidgets({{}}, &error) && error.find("root") != std::string::npos);
    CHECK(doc.DeleteWidgets({{0}, {0, 0}}) && doc.Bindings().empty() && doc.FindAnimation("Show")->tracks.empty() && doc.PathOf("Begin") == std::nullopt);
    CHECK(doc.Undo() && doc.Bindings().size() == 1 && doc.PathOf("Begin") == WidgetPath{0});

    // A preview copy is its own tree.
    std::unique_ptr<ui::Widget> preview = doc.CopyTree();
    preview->Child(0)->name = "Changed";
    CHECK(doc.At({0})->name == "Begin" && preview->ChildCount() == doc.Root().ChildCount());

    // Files.
    CHECK(!doc.Save(&error) && error.find("Save As") != std::string::npos);
    const std::filesystem::path file = std::filesystem::temp_directory_path() / "aether_designer_test.aui";
    CHECK(doc.SaveAs(file, &error) && !doc.Dirty() && doc.Name() == "aether_designer_test");
    UILayoutDocument loaded;
    CHECK(loaded.Load(file, &error) && loaded.Text() == doc.Text() && !loaded.CanUndo() && loaded.Bindings().size() == 1);
    CHECK(!loaded.Load(file.string() + ".missing", &error) && error.find("can't open") != std::string::npos);
    std::filesystem::remove(file);
}

AETHER_TEST(UIDesigner_Canvas) {
    HeadlessImGui imgui;
    UILayoutDocument doc;
    UIDesigner designer(doc);
    auto frame = [&] { imgui.Frame([&] { designer.Draw(); }); };
    ImGuiIO& io = ImGui::GetIO();
    auto move = [&](Vec2 p) {
        io.AddMousePosEvent(p.x, p.y);
        frame();
    };
    auto press = [&](bool down) {
        io.AddMouseButtonEvent(0, down);
        frame();
    };
    frame();
    frame();
    // Fitted to the canvas; the view maps both ways.
    CHECK(designer.Zoom() > 0.1f && designer.Zoom() < 1.0f && designer.Resolution() == Vec2{1920, 1080});
    const Vec2 there = designer.ScreenToLayout(designer.LayoutToScreen({300, 400}));
    CHECK(Near(there.x, 300, 0.01f) && Near(there.y, 400, 0.01f));

    // Placing snaps to the grid, gives the type its size, and selects it.
    const auto button = designer.PlaceWidget("Button", {101, 99});
    CHECK(button == WidgetPath{0} && designer.Selection() == std::vector<WidgetPath>{{0}});
    CHECK(doc.At({0})->slot.position == Vec2{104, 96} && doc.At({0})->slot.size == Vec2{160, 48} && doc.At({0})->ChildCount() == 1);
    CHECK(doc.UndoLabel() == "Add Button" && doc.Undo() && doc.Root().ChildCount() == 0 && doc.Redo());
    frame();
    CHECK(Near(designer.PreviewWidget({0})->Geometry(), {104, 96, 160, 48}));
    CHECK(designer.PickAt({110, 100}) == (WidgetPath{0, 0}) || designer.PickAt({110, 100}) == WidgetPath{0}); // its label, or it
    CHECK(designer.PickAt({1000, 900}).empty());

    // Dragging it with the mouse: snapped, one undo step.
    designer.Select({0});
    const Vec2 grab = designer.LayoutToScreen({125, 140}); // clear of its label and its handles
    move(grab);
    press(true);
    const f32 scale = designer.Zoom(); // pixels per layout unit (the scale is 1 at 1920 x 1080)
    move({grab.x + 20 * scale, grab.y});
    move({grab.x + 81 * scale, grab.y + 42 * scale});
    press(false);
    CHECK(doc.At({0})->slot.position == (Vec2{184, 136}) && doc.UndoLabel() == "Place Button1");
    CHECK(doc.Undo() && doc.At({0})->slot.position == (Vec2{104, 96}) && doc.Redo());
    frame();
    // Its bottom-right handle resizes it.
    const Vec2 corner = designer.LayoutToScreen({184 + 160, 136 + 48});
    move(corner);
    press(true);
    move({corner.x + 40 * scale, corner.y + 16 * scale});
    press(false);
    CHECK(Near(doc.GeometryOf({0}), {184, 136, 200, 64}));
    // Clicking empty space selects the root; Ctrl+click adds to the selection.
    const auto slider = designer.PlaceWidget("Slider", {800, 600});
    frame();
    move(designer.LayoutToScreen({1500, 1000}));
    press(true);
    press(false);
    CHECK(designer.Selection() == std::vector<WidgetPath>{WidgetPath{}});
    designer.Select({0});
    io.AddKeyEvent(ImGuiMod_Ctrl, true);
    move(designer.LayoutToScreen({850, 610}));
    press(true);
    press(false);
    io.AddKeyEvent(ImGuiMod_Ctrl, false);
    frame();
    CHECK(designer.Selection().size() == 2 && designer.Selection()[1] == *slider);
    // Nudging moves both.
    designer.Nudge({8, 0});
    CHECK(doc.At({0})->slot.position.x == 192.0f && doc.At(*slider)->slot.position.x == 808.0f);

    // Into a container under the point; past one that's full.
    const auto box = designer.PlaceWidget("VerticalBox", {1200, 200});
    frame();
    const auto inside = designer.PlaceWidget("Text", {1210, 210});
    CHECK(inside == (WidgetPath{2, 0}) && doc.At(*box)->ChildCount() == 1);
    frame();
    const auto past = designer.PlaceWidget("Text", {200, 150}); // on the button, which has its label
    CHECK(past == WidgetPath{3});
    designer.Select(*past);
    designer.DuplicateSelection();
    CHECK(designer.Selection() == std::vector<WidgetPath>{{4}} && doc.Root().ChildCount() == 5);
    designer.DeleteSelection();
    CHECK(doc.Root().ChildCount() == 4 && designer.Selection().empty());

    // Resolutions: a portrait phone puts the root inside its safe area.
    designer.SetResolution(6);
    frame();
    frame();
    CHECK(designer.Resolution() == Vec2{1080, 2340} && Near(designer.PreviewArea().y, 132) && Near(designer.PreviewWidget({})->Geometry().y, 132));
    designer.SetCustomResolution({3840, 2160});
    frame();
    CHECK(Near(designer.PreviewArea().w, 1920)); // the same layout, twice the pixels
    designer.SetResolution(0);
    frame();

    // A theme styles the preview, not the document.
    ui::Theme theme;
    ui::ControlStyle big;
    big.text_size = 40;
    theme.styles["Button"] = big;
    designer.SetTheme(&theme);
    frame();
    CHECK(static_cast<ui::Button*>(designer.PreviewWidget({0}))->style.text_size == 40.0f && static_cast<ui::Button*>(doc.At({0}))->style.text_size != 40.0f);

    // The timeline: key the current value at the playhead, scrub and play on the preview.
    designer.SelectAnimation(doc.AddAnimation("Pulse"));
    CHECK(designer.ActiveAnimation() == "Pulse" && doc.AddTrack("Pulse", "Button1", "opacity"));
    doc.SetProperty({0}, "opacity", 0.2f);
    designer.SetPlayhead(0.0f);
    CHECK(designer.KeyAtPlayhead(0));
    doc.SetProperty({0}, "opacity", 1.0f);
    designer.SetPlayhead(1.0f);
    CHECK(designer.KeyAtPlayhead(0) && !designer.KeyAtPlayhead(4));
    const auto& keys = doc.FindAnimation("Pulse")->tracks[0].keys;
    CHECK(keys.size() == 2 && Near(keys[0].value, 0.2f) && Near(keys[1].value, 1.0f));
    designer.SetPlayhead(0.5f);
    frame();
    CHECK(Near(designer.PreviewWidget({0})->opacity, 0.6f) && doc.At({0})->opacity == 1.0f);
    designer.SetPlayhead(0.0f);
    designer.Play();
    for (int i = 0; i < 30; ++i) frame();
    CHECK(designer.Playing() && Near(designer.Playhead(), 0.5f, 0.02f) && Near(designer.PreviewWidget({0})->opacity, 0.6f, 0.02f));
    for (int i = 0; i < 40; ++i) frame(); // loops
    CHECK(designer.Playhead() < 0.3f);
    designer.Stop();
    // Undoing past the animation drops it from the timeline.
    while (doc.FindAnimation("Pulse") != nullptr && doc.Undo()) {
    }
    frame();
    CHECK(designer.ActiveAnimation().empty());

    // Keys: Delete removes the selection, Ctrl+Z brings it back.
    while (doc.Redo()) {
    }
    designer.Select({1});
    frame();
    io.AddKeyEvent(ImGuiKey_Delete, true);
    frame();
    io.AddKeyEvent(ImGuiKey_Delete, false);
    frame();
    CHECK(doc.Root().ChildCount() == 3);
    io.AddKeyEvent(ImGuiMod_Ctrl, true);
    io.AddKeyEvent(ImGuiKey_Z, true);
    frame();
    io.AddKeyEvent(ImGuiKey_Z, false);
    io.AddKeyEvent(ImGuiMod_Ctrl, false);
    frame();
    CHECK(doc.Root().ChildCount() == 4);
    // Selections of widgets that went away are dropped.
    designer.Select({3});
    doc.DeleteWidgets({{3}});
    frame();
    CHECK(designer.Selection().empty() && !designer.Primary());
}
