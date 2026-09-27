#include "aether/ui/basic.h"
#include "aether/ui/panels.h"
#include "aether/ui/viewport.h"
#include "test_framework.h"

#include <cmath>

using namespace aether;
using namespace aether::ui;

// Phase 18 step 1: the widget tree, layout panels, text layout, the draw
// list (9-slice, clipping), resolution scaling, safe areas and hit testing.

namespace {

bool Near(f32 a, f32 b, f32 tol = 1e-3f) { return std::fabs(a - b) <= tol; }
bool Near(const Rect& a, const Rect& b, f32 tol = 1e-3f) { return Near(a.x, b.x, tol) && Near(a.y, b.y, tol) && Near(a.w, b.w, tol) && Near(a.h, b.h, tol); }

const BuiltinFont kFont; // advance 0.6 x size, line 1.25 x size, ascent 0.95 x size

// Lays out `root` in a rect as a viewport would.
void Layout(Widget& root, const Rect& area) {
    LayoutContext ctx{&kFont};
    root.Measure(ctx);
    root.Arrange(area, ctx);
}

std::unique_ptr<Canvas> MakeCanvas() { return std::make_unique<Canvas>(); }

} // namespace

AETHER_TEST(UI_TextLayoutAndDrawList) {
    // Text: fixed-width glyphs, lines at '\\n', words wrapped greedily.
    TextLayout t = LayoutText(kFont, "Hello", 20.0f);
    AETHER_CHECK(t.lines.size() == 1 && Near(t.size.x, 60.0f) && Near(t.size.y, 25.0f));
    t = LayoutText(kFont, "a\nbcd", 20.0f);
    AETHER_CHECK(t.lines.size() == 2 && Near(t.size.x, 36.0f) && Near(t.size.y, 50.0f));
    t = LayoutText(kFont, "aaa bbb ccc", 10.0f, 45.0f); // 6 per glyph: "aaa bbb" is 42
    AETHER_CHECK(t.lines.size() == 2 && Near(t.lines[0].width, 42.0f) && t.lines[1].begin == 8 && t.lines[1].end == 11);
    t = LayoutText(kFont, "aaa bbb ccc", 10.0f, 20.0f);
    AETHER_CHECK(t.lines.size() == 3 && Near(t.size.x, 18.0f));
    t = LayoutText(kFont, "abcdefghij xy", 10.0f, 20.0f); // a long word gets its own line
    AETHER_CHECK(t.lines.size() == 2 && Near(t.lines[0].width, 60.0f) && Near(t.lines[1].width, 12.0f));
    t = LayoutText(kFont, "", 10.0f);
    AETHER_CHECK(t.lines.size() == 1 && Near(t.size.y, 12.5f));
    // UTF-8: "é" is one glyph; a stray byte reads as one replacement character.
    AETHER_CHECK(Near(LayoutText(kFont, "\xC3\xA9", 10.0f).size.x, 6.0f) && Near(LayoutText(kFont, "\xFF" "a", 10.0f).size.x, 12.0f));
    usize i = 0;
    AETHER_CHECK(DecodeUtf8("\xE2\x82\xAC", i) == 0x20AC && i == 3); // €

    // Draw list: clipping and culling.
    DrawList list;
    list.AddQuad({0, 0, 10, 10}, {1, 0, 0, 1});
    list.PushClip({0, 0, 50, 50});
    list.PushClip({25, 25, 100, 100});
    AETHER_CHECK(Near(list.clips[list.CurrentClip()], {25, 25, 25, 25}));
    list.AddQuad({0, 0, 10, 10}, {}); // outside: dropped
    list.AddQuad({30, 30, 10, 10}, {});
    list.AddQuad({30, 30, 0, 10}, {});        // empty: dropped
    list.AddQuad({30, 30, 10, 10}, {1, 1, 1, 0}); // transparent: dropped
    list.PopClip();
    list.PopClip();
    list.PopClip(); // (never below the root)
    AETHER_CHECK(list.quads.size() == 2 && list.quads[0].clip == 0 && list.quads[1].clip == 2 && list.CurrentClip() == 0);
    // 9-slice: corners keep their size, the middle stretches; a frame has no middle.
    Brush box;
    box.kind = Brush::Kind::Box;
    box.image_size = {40, 40};
    box.slice = Margin::All(10);
    box.texture = 7;
    DrawList nine;
    nine.AddBrush({0, 0, 100, 50}, box);
    AETHER_CHECK(nine.quads.size() == 9);
    AETHER_CHECK(Near(nine.quads[0].rect, {0, 0, 10, 10}) && Near(nine.quads[4].rect, {10, 10, 80, 30}) && Near(nine.quads[8].rect, {90, 40, 10, 10}));
    AETHER_CHECK(Near(nine.quads[0].uv, {0, 0, 0.25f, 0.25f}) && Near(nine.quads[4].uv, {0.25f, 0.25f, 0.5f, 0.5f}) && nine.quads[4].texture == 7);
    box.kind = Brush::Kind::Frame;
    DrawList frame;
    frame.AddBrush({0, 0, 100, 50}, box);
    AETHER_CHECK(frame.quads.size() == 8);
    DrawList small; // too small for the corners: they shrink to fit
    box.kind = Brush::Kind::Box;
    small.AddBrush({0, 0, 10, 10}, box);
    AETHER_CHECK(small.quads.size() == 4 && Near(small.quads[0].rect, {0, 0, 5, 5}));
    box.slice_scale = 2.0f;
    DrawList scaled;
    scaled.AddBrush({0, 0, 100, 100}, box);
    AETHER_CHECK(Near(scaled.quads[0].rect, {0, 0, 20, 20}));
    // Text quads: one per visible glyph, placed from the baseline and aligned.
    DrawList text;
    text.AddText(kFont, "a b", 10.0f, {0, 0, 100, 20}, {}, TextAlign::Right);
    AETHER_CHECK(text.quads.size() == 2 && Near(text.quads[0].rect.x, 100.0f - 18.0f + 0.5f) && Near(text.quads[0].rect.y, 9.5f - 7.0f));
    // Scaling to pixels scales quads and clips.
    list.Scale(2.0f);
    AETHER_CHECK(Near(list.quads[1].rect, {60, 60, 20, 20}) && Near(list.clips[2], {50, 50, 50, 50}));
}

AETHER_TEST(UI_CanvasAnchorsAndPanels) {
    const Rect screen{0, 0, 1920, 1080};
    auto canvas = MakeCanvas();
    // Point anchor at the centre, pivot in the middle, nudged up.
    Image* play = canvas->Add<Image>();
    play->slot.anchors = Anchors::Point(0.5f, 0.5f);
    play->slot.alignment = {0.5f, 0.5f};
    play->slot.size = {200, 100};
    play->slot.position = {0, -40};
    // Stretched both ways, inset.
    Image* backdrop = canvas->Add<Image>();
    backdrop->slot.anchors = Anchors::Stretch();
    backdrop->slot.margins = Margin::All(10);
    // Stretched across the bottom.
    Image* bar = canvas->Add<Image>();
    bar->slot.anchors = {0.0f, 1.0f, 1.0f, 1.0f};
    bar->slot.margins = {20, 0, 20, 0};
    bar->slot.position = {0, -10};
    bar->slot.size = {0, 50};
    bar->slot.alignment = {0, 1};
    // Sized by its content.
    Text* title = canvas->Add<Text>("Play");
    title->size = 20.0f;
    title->slot.auto_size = true;
    title->slot.position = {100, 100};
    Layout(*canvas, screen);
    AETHER_CHECK(Near(play->Geometry(), {860, 450, 200, 100}));
    AETHER_CHECK(Near(backdrop->Geometry(), {10, 10, 1900, 1060}));
    AETHER_CHECK(Near(bar->Geometry(), {20, 1020, 1880, 50}));
    AETHER_CHECK(Near(title->Geometry(), {100, 100, 48, 25}));
    // A canvas inside a smaller rect anchors to that rect.
    Layout(*canvas, {100, 200, 400, 300});
    AETHER_CHECK(Near(play->Geometry(), {200, 260, 200, 100}));
    // z order: higher z draws later and is hit first.
    backdrop->slot.z = -1;
    play->slot.z = 5;
    Layout(*canvas, screen);
    AETHER_CHECK(canvas->HitTest({900, 480}) == play);
    AETHER_CHECK(canvas->HitTest({50, 50}) == backdrop);
    AETHER_CHECK(canvas->HitTest({110, 110}) == backdrop); // Text doesn't catch clicks

    // Horizontal box: fixed children keep their size; the rest share by weight.
    HorizontalBox row;
    row.spacing = 10;
    row.Add<Spacer>(Vec2{50, 10});
    Image* a = row.Add<Image>();
    a->slot.fill = 1;
    Image* b = row.Add<Image>();
    b->slot.fill = 2;
    b->slot.padding = {5, 0, 5, 0};
    Layout(row, {0, 0, 300, 40});
    // Left: 300 - 50 - 2 x 10 - 10 (padding) = 220, split 1:2.
    AETHER_CHECK(Near(a->Geometry(), {60, 0, 220.0f / 3, 40}) && Near(b->Geometry(), {60 + 220.0f / 3 + 10 + 5, 0, 440.0f / 3, 40}));
    AETHER_CHECK(Near(row.DesiredSize().x, 50 + 10 + 0 + 10 + 10) && Near(row.DesiredSize().y, 10));
    // Vertical box: across it, children align; collapsed ones take no space, hidden ones do.
    VerticalBox col;
    Image* centred = col.Add<Image>();
    centred->desired_size = {40, 20};
    centred->slot.h_align = HAlign::Center;
    Image* gone = col.Add<Image>();
    gone->desired_size = {10, 100};
    gone->visibility = Visibility::Collapsed;
    Image* hidden = col.Add<Image>();
    hidden->desired_size = {10, 30};
    hidden->visibility = Visibility::Hidden;
    Image* last = col.Add<Image>();
    last->desired_size = {10, 10};
    last->slot.h_align = HAlign::Right;
    Layout(col, {0, 0, 200, 500});
    AETHER_CHECK(Near(centred->Geometry(), {80, 0, 40, 20}) && Near(last->Geometry(), {190, 50, 10, 10}));
    AETHER_CHECK(Near(col.DesiredSize().y, 60) && Near(col.DesiredSize().x, 40));

    // Grid: an auto column and a weighted one; a child spanning both.
    Grid grid;
    grid.column_fill = {0, 1};
    grid.column_spacing = 4;
    grid.row_spacing = 2;
    Image* label = grid.Add<Image>();
    label->desired_size = {80, 20};
    Image* field = grid.Add<Image>();
    field->desired_size = {50, 30};
    field->slot.column = 1;
    Image* wide = grid.Add<Image>();
    wide->desired_size = {500, 10};
    wide->slot.row = 1;
    wide->slot.column_span = 2;
    Layout(grid, {0, 0, 400, 100});
    AETHER_CHECK(grid.ColumnWidths().size() == 2 && Near(grid.ColumnWidths()[0], 80) && Near(grid.ColumnWidths()[1], 316));
    AETHER_CHECK(Near(label->Geometry(), {0, 0, 80, 30}) && Near(field->Geometry(), {84, 0, 316, 30}) && Near(wide->Geometry(), {0, 32, 400, 10}));
    AETHER_CHECK(Near(grid.DesiredSize().x, 80 + 4 + 50) && Near(grid.DesiredSize().y, 30 + 2 + 10)); // spanning children don't size tracks

    // Overlay: children share the area, each aligned.
    Overlay stack;
    Image* bg = stack.Add<Image>();
    Image* badge = stack.Add<Image>();
    badge->desired_size = {16, 16};
    badge->slot.h_align = HAlign::Right;
    badge->slot.v_align = VAlign::Top;
    badge->slot.padding = Margin::All(2);
    Layout(stack, {0, 0, 100, 100});
    AETHER_CHECK(Near(bg->Geometry(), {0, 0, 100, 100}) && Near(badge->Geometry(), {82, 2, 16, 16}));
    AETHER_CHECK(stack.HitTest({90, 10}) == badge && stack.HitTest({10, 90}) == bg);

    // SizeBox: overrides and limits; one child only.
    SizeBox box;
    Image* inner = box.Add<Image>();
    inner->desired_size = {500, 20};
    box.max_width = 300;
    box.min_height = 50;
    AETHER_CHECK(box.Add<Image>() == nullptr);
    Layout(box, {0, 0, 1000, 1000});
    AETHER_CHECK(Near(box.DesiredSize().x, 300) && Near(box.DesiredSize().y, 50));
    box.width = 120;
    Layout(box, {0, 0, 1000, 1000});
    AETHER_CHECK(Near(box.DesiredSize().x, 120));

    // Border: padding around its child, background painted first and hit.
    Border border;
    border.background = Brush::Solid({0, 0, 0, 0.5f});
    border.padding = Margin::All(8);
    Text* inside = border.Add<Text>("OK");
    inside->size = 10;
    Layout(border, {0, 0, 200, 100});
    AETHER_CHECK(Near(border.DesiredSize().x, 12 + 16) && Near(inside->Geometry(), {8, 8, 184, 84}));
    AETHER_CHECK(border.HitTest({2, 2}) == &border);
    DrawList painted;
    border.Paint(painted, {&kFont, 1.0f});
    AETHER_CHECK(painted.quads.size() == 3 && Near(painted.quads[0].rect, {0, 0, 200, 100}));

    // ScrollBox: taller content scrolls, is clipped, and shows a scroll bar.
    ScrollBox scroll;
    VerticalBox* list = scroll.Add<VerticalBox>();
    std::vector<Image*> items;
    for (int k = 0; k < 10; ++k) {
        items.push_back(list->Add<Image>());
        items.back()->desired_size = {100, 50};
    }
    Layout(scroll, {0, 0, 200, 200});
    AETHER_CHECK(Near(scroll.MaxOffset(), 300) && Near(items[1]->Geometry().y, 50));
    scroll.ScrollBy(1000);
    AETHER_CHECK(Near(scroll.Offset(), 300));
    Layout(scroll, {0, 0, 200, 200});
    AETHER_CHECK(Near(items[9]->Geometry().y, 150) && Near(items[0]->Geometry().y, -300));
    scroll.ScrollIntoView(items[0]->Geometry());
    AETHER_CHECK(Near(scroll.Offset(), 0));
    Layout(scroll, {0, 0, 200, 200});
    scroll.ScrollIntoView(items[5]->Geometry()); // 250..300: scroll by 100 to show its bottom
    AETHER_CHECK(Near(scroll.Offset(), 100));
    Layout(scroll, {0, 0, 200, 200});
    AETHER_CHECK(scroll.HitTest({50, 250}) == nullptr && scroll.HitTest({50, 10}) == items[2]);
    DrawList clipped;
    scroll.Paint(clipped, {&kFont, 1.0f});
    // Items 2..5 are in view (4 whole ones); all 10 are emitted but only those intersecting the clip survive, plus the bar.
    AETHER_CHECK(clipped.quads.size() == 5 && Near(clipped.quads.back().rect.x, 194) && clipped.quads.back().clip == 0);
}

AETHER_TEST(UI_ViewportScaleSafeAreaAndHitTest) {
    ScaleSettings s;
    AETHER_CHECK(Near(s.ScaleFor({3840, 2160}), 2.0f) && Near(s.ScaleFor({1280, 720}), 2.0f / 3.0f) && Near(s.ScaleFor({1080, 1920}), 1.0f));
    s.rule = ScaleSettings::Rule::Width;
    AETHER_CHECK(Near(s.ScaleFor({2560, 1080}), 2560.0f / 1920.0f));
    s.rule = ScaleSettings::Rule::LongestSide;
    AETHER_CHECK(Near(s.ScaleFor({1080, 3840}), 2.0f));
    s.rule = ScaleSettings::Rule::Curve;
    s.curve = {{720, 1.0f}, {1080, 1.5f}, {2160, 2.0f}};
    AETHER_CHECK(Near(s.ScaleFor({1280, 720}), 1.0f) && Near(s.ScaleFor({1920, 900}), 1.25f) && Near(s.ScaleFor({7680, 4320}), 2.0f));
    s.rule = ScaleSettings::Rule::None;
    s.max_scale = 0.5f;
    AETHER_CHECK(Near(s.ScaleFor({100, 100}), 0.5f)); // clamped

    // A 4K screen with a 1080p layout: everything doubles on the way to pixels; the safe area insets it.
    Viewport vp;
    vp.SetSize({3840, 2160});
    vp.SetSafeArea(Margin::All(40));
    AETHER_CHECK(Near(vp.Scale(), 2.0f) && Near(vp.LayoutSize().x, 1920) && Near(vp.SafeRect(), {20, 20, 1880, 1040}));
    auto hud = std::make_unique<Canvas>();
    hud->name = "HUD";
    Image* health = hud->Add<Image>(Brush::Solid({1, 0, 0, 1}));
    health->name = "Health";
    health->slot.position = {10, 10};
    health->slot.size = {300, 20};
    Widget* hud_root = vp.Add(std::move(hud), 0);
    auto menu = std::make_unique<Canvas>();
    Image* dim = menu->Add<Image>(Brush::Solid({0, 0, 0, 0.5f}));
    dim->slot.anchors = Anchors::Stretch();
    dim->opacity = 0.5f;
    Widget* menu_root = vp.Add(std::move(menu), 10);
    vp.Layout(kFont);
    AETHER_CHECK(Near(health->Geometry(), {30, 30, 300, 20}));
    DrawList list = vp.Paint(kFont);
    AETHER_CHECK(list.quads.size() == 2 && Near(list.quads[0].rect, {60, 60, 600, 40}) && Near(list.quads[1].color.a, 0.25f));
    AETHER_CHECK(vp.HitTest({100, 70}) == dim); // the menu layer is on top
    AETHER_CHECK(vp.Find("Health") == health && vp.Find("Nope") == nullptr && vp.LayerCount() == 2);
    // Layers keep z order whatever order they're added in.
    auto toast = std::make_unique<Canvas>();
    Widget* toast_root = vp.Add(std::move(toast), 5);
    AETHER_CHECK(vp.Layer(0) == hud_root && vp.Layer(1) == toast_root && vp.Layer(2) == menu_root);
    std::unique_ptr<Widget> taken = vp.Remove(menu_root);
    AETHER_CHECK(taken.get() == menu_root && vp.LayerCount() == 2 && vp.Remove(menu_root) == nullptr);
    vp.Layout(kFont);
    AETHER_CHECK(vp.HitTest({100, 70}) == health);

    // Visibility: hidden isn't drawn or hit; hit-test invisible is drawn but lets clicks through;
    // self-hit-test invisible passes clicks to its children only; opacity multiplies down.
    Overlay root;
    Border* panel = root.Add<Border>();
    panel->background = Brush::Solid({});
    Image* icon = panel->Add<Image>();
    icon->desired_size = {10, 10};
    icon->slot.h_align = HAlign::Left;
    icon->slot.v_align = VAlign::Top;
    Layout(root, {0, 0, 100, 100});
    AETHER_CHECK(root.HitTest({5, 5}) == icon && root.HitTest({50, 50}) == panel && root.HitTest({500, 5}) == nullptr);
    panel->visibility = Visibility::SelfHitTestInvisible;
    AETHER_CHECK(root.HitTest({5, 5}) == icon && root.HitTest({50, 50}) == nullptr);
    panel->visibility = Visibility::HitTestInvisible;
    AETHER_CHECK(root.HitTest({5, 5}) == nullptr);
    DrawList drawn;
    root.Paint(drawn, {&kFont, 1.0f});
    AETHER_CHECK(drawn.quads.size() == 2);
    panel->visibility = Visibility::Hidden;
    DrawList none;
    root.Paint(none, {&kFont, 1.0f});
    AETHER_CHECK(none.quads.empty() && root.HitTest({5, 5}) == nullptr);
    panel->visibility = Visibility::Visible;
    panel->opacity = 0.5f;
    icon->opacity = 0.5f;
    DrawList faded;
    root.Paint(faded, {&kFont, 1.0f});
    AETHER_CHECK(Near(faded.quads[0].color.a, 0.5f) && Near(faded.quads[1].color.a, 0.25f));

    // The tree: find, remove (ownership and parent back), and one-child panels refusing a second.
    AETHER_CHECK(icon->Parent() == panel && panel->Parent() == &root);
    icon->name = "Icon";
    AETHER_CHECK(root.Find("Icon") == icon && root.FindAs<Image>("Icon") == icon && root.FindAs<Border>("Icon") == nullptr);
    std::unique_ptr<Widget> removed = panel->RemoveChild(icon);
    AETHER_CHECK(removed.get() == icon && icon->Parent() == nullptr && panel->ChildCount() == 0 && root.Find("Icon") == nullptr);
    AETHER_CHECK(panel->AddChild(std::move(removed)) == icon && panel->Add<Image>() == nullptr);
    AETHER_CHECK(std::string(root.TypeName()) == "Overlay" && std::string(icon->TypeName()) == "Image");
}
