#include "aether/ui/basic.h"
#include "aether/ui/controls.h"
#include "aether/ui/input.h"
#include "aether/ui/panels.h"
#include "test_framework.h"

#include <cmath>

using namespace aether;
using namespace aether::ui;
using input::InputState;
using input::Key;

// Phase 18 step 2: interactive widgets and UI input - pointers, capture,
// focus and navigation, the device bridge, text editing, popups, a
// virtualized list and tooltips.

namespace {

bool Near(f32 a, f32 b, f32 tol = 1e-3f) { return std::fabs(a - b) <= tol; }

const BuiltinFont kFont; // text size 20: 12 per glyph

struct Screen {
    Viewport viewport;
    UIInputRouter router{viewport, kFont};
    Canvas* canvas = nullptr;
    Screen() {
        viewport.SetSize({1920, 1080});
        canvas = static_cast<Canvas*>(viewport.Add(std::make_unique<Canvas>()));
    }
    template <typename T>
    T* Place(Rect r) {
        T* w = canvas->Add<T>();
        w->slot.position = {r.x, r.y};
        w->slot.size = {r.w, r.h};
        return w;
    }
    void Layout() { viewport.Layout(kFont); }
    // A click at a point.
    void Click(Vec2 p, int button = 0) {
        router.PointerMove(p);
        router.PointerDown(p, button);
        router.PointerUp(p, button);
    }
};

} // namespace

AETHER_TEST(UI_PointerButtonsTogglesAndSliders) {
    Screen s;
    Button* play = s.Place<Button>({100, 100, 200, 50});
    play->Add<Text>("Play");
    int clicks = 0, pressed = 0;
    play->on_clicked = [&] { ++clicks; };
    play->on_pressed = [&] { ++pressed; };
    s.Layout();
    // Hover, press (focus and capture), release inside: a click.
    AETHER_CHECK(s.router.PointerMove({150, 120}) && s.router.Hovered() == play && play->IsHovered());
    AETHER_CHECK(!s.router.PointerMove({900, 900}) && s.router.Hovered() == nullptr && !play->IsHovered()); // empty canvas: not the UI's
    s.router.PointerMove({150, 120});
    AETHER_CHECK(s.router.PointerDown({150, 120}) && play->IsPressed() && pressed == 1 && s.router.Focused() == play && s.router.Captured() == play);
    AETHER_CHECK(s.router.PointerUp({150, 120}) && clicks == 1 && !play->IsPressed() && s.router.Captured() == nullptr);
    // Pressed, dragged off, released outside: no click; while off it, it isn't hovered.
    s.router.PointerDown({150, 120});
    s.router.PointerMove({600, 600});
    AETHER_CHECK(s.router.Hovered() == nullptr && s.router.Captured() == play);
    s.router.PointerUp({600, 600});
    AETHER_CHECK(clicks == 1);
    // The right button doesn't click; Accept does, while focused.
    s.Click({150, 120}, 1);
    AETHER_CHECK(clicks == 1);
    AETHER_CHECK(s.router.Accept() && clicks == 2);
    // Disabled (itself or a parent): no click, no focus.
    s.router.ClearFocus();
    play->enabled = false;
    s.Click({150, 120});
    AETHER_CHECK(clicks == 2 && s.router.Focused() == nullptr && !s.router.Accept());
    play->enabled = true;
    s.canvas->enabled = false;
    s.Click({150, 120});
    AETHER_CHECK(clicks == 2);
    s.canvas->enabled = true;
    // Clicks on a child of the button reach the button.
    s.Click({110, 110});
    AETHER_CHECK(clicks == 3);

    // Toggle: click and accept flip it.
    Toggle* sound = s.Place<Toggle>({400, 100, 24, 24});
    int changes = 0;
    sound->on_changed = [&](bool) { ++changes; };
    s.Layout();
    s.Click({410, 110});
    AETHER_CHECK(sound->checked && changes == 1 && s.router.Focused() == sound);
    s.router.Accept();
    AETHER_CHECK(!sound->checked && changes == 2);
    sound->SetChecked(false);
    AETHER_CHECK(changes == 2); // no change, no call

    // Slider: press sets, drag follows, it clamps and snaps; left/right step it while focused.
    Slider* volume = s.Place<Slider>({100, 300, 200, 24});
    f32 last = -1.0f;
    volume->on_changed = [&](f32 v) { last = v; };
    s.Layout();
    s.router.PointerMove({200, 310});
    s.router.PointerDown({200, 310});
    AETHER_CHECK(Near(volume->value, 0.5f) && Near(last, 0.5f));
    s.router.PointerMove({250, 310});
    AETHER_CHECK(Near(volume->value, 0.75f));
    s.router.PointerMove({900, 310});
    AETHER_CHECK(Near(volume->value, 1.0f));
    s.router.PointerUp({900, 310});
    volume->step = 0.1f;
    volume->SetValue(0.33f);
    AETHER_CHECK(Near(volume->value, 0.3f));
    s.router.SetFocus(volume);
    AETHER_CHECK(s.router.Navigate(NavDirection::Right) && Near(volume->value, 0.4f));
    s.router.Navigate(NavDirection::Left);
    s.router.Navigate(NavDirection::Left);
    AETHER_CHECK(Near(volume->value, 0.2f) && s.router.Focused() == volume);
    s.router.Navigate(NavDirection::Up); // up/down leave it
    AETHER_CHECK(s.router.Focused() != volume);

    // Two fingers drag two sliders at once.
    Slider* a = s.Place<Slider>({100, 400, 200, 24});
    Slider* b = s.Place<Slider>({100, 500, 200, 24});
    s.Layout();
    s.router.PointerDown({120, 410}, 0, 1);
    s.router.PointerDown({120, 510}, 0, 2);
    s.router.PointerMove({300, 410}, 1);
    s.router.PointerMove({200, 510}, 2);
    AETHER_CHECK(Near(a->value, 1.0f) && Near(b->value, 0.5f) && s.router.Captured(1) == a && s.router.Captured(2) == b);
    s.router.PointerUp({300, 410}, 0, 1);
    s.router.PointerUp({200, 510}, 0, 2);

    // Progress bars fill from any side.
    ProgressBar* hp = s.Place<ProgressBar>({100, 600, 200, 20});
    hp->percent = 0.25f;
    ProgressBar* charge = s.Place<ProgressBar>({400, 600, 20, 200});
    charge->percent = 0.5f;
    charge->fill = ProgressBar::Fill::BottomToTop;
    s.Layout();
    DrawList hp_list, charge_list;
    hp->Paint(hp_list, {&kFont, 1.0f});
    charge->Paint(charge_list, {&kFont, 1.0f});
    AETHER_CHECK(Near(hp_list.quads[1].rect.w, 50) && Near(charge_list.quads[1].rect.y, 700) && Near(charge_list.quads[1].rect.h, 100));

    // The wheel scrolls the scroll box under it; a box with nothing to scroll passes it on.
    ScrollBox* outer = s.Place<ScrollBox>({1000, 100, 300, 300});
    VerticalBox* page = outer->Add<VerticalBox>();
    ScrollBox* inner = page->Add<ScrollBox>();
    inner->Add<Spacer>(Vec2{100, 50}); // fits: nothing to scroll
    page->Add<Spacer>(Vec2{100, 1000});
    s.Layout();
    AETHER_CHECK(s.router.Wheel({1100, 120}, -1.0f) && Near(outer->Offset(), 60) && Near(inner->Offset(), 0));
}

AETHER_TEST(UI_FocusNavigationAndDeviceBridge) {
    Screen s;
    // A 3 x 2 grid of buttons.
    Button* grid[2][3];
    int accepted = -1;
    for (int r = 0; r < 2; ++r) {
        for (int c = 0; c < 3; ++c) {
            grid[r][c] = s.Place<Button>({100.0f + 250 * c, 100.0f + 100 * r, 200, 60});
            grid[r][c]->name = "b" + std::to_string(r) + std::to_string(c);
            const int id = r * 3 + c;
            grid[r][c]->on_clicked = [&accepted, id] { accepted = id; };
        }
    }
    s.Layout();
    // First navigation focuses the first; then spatially; at an edge focus stays.
    AETHER_CHECK(s.router.Navigate(NavDirection::Down) && s.router.Focused() == grid[0][0] && grid[0][0]->IsFocused());
    s.router.Navigate(NavDirection::Right);
    AETHER_CHECK(s.router.Focused() == grid[0][1]);
    s.router.Navigate(NavDirection::Down);
    AETHER_CHECK(s.router.Focused() == grid[1][1]);
    s.router.Navigate(NavDirection::Left);
    AETHER_CHECK(s.router.Focused() == grid[1][0]);
    AETHER_CHECK(s.router.Navigate(NavDirection::Left) && s.router.Focused() == grid[1][0]); // nothing further left
    s.router.Navigate(NavDirection::Up);
    AETHER_CHECK(s.router.Focused() == grid[0][0] && !grid[1][0]->IsFocused());
    // Next / Previous in tree order, wrapping.
    s.router.Navigate(NavDirection::Previous);
    AETHER_CHECK(s.router.Focused() == grid[1][2]);
    s.router.Navigate(NavDirection::Next);
    AETHER_CHECK(s.router.Focused() == grid[0][0]);
    // Explicit targets win; disabled and hidden ones are skipped.
    grid[0][0]->navigation.right = "b12";
    s.router.Navigate(NavDirection::Right);
    AETHER_CHECK(s.router.Focused() == grid[1][2]);
    grid[1][1]->enabled = false;
    grid[1][0]->visibility = Visibility::Hidden;
    s.router.Navigate(NavDirection::Left);
    AETHER_CHECK(s.router.Focused() == grid[0][1]); // (1,1) disabled, (1,0) hidden: the nearest leftwards is up a row
    s.router.Accept();
    AETHER_CHECK(accepted == 1);
    // Focus moving into a scroll box brings the widget into view.
    ScrollBox* scroll = s.Place<ScrollBox>({100, 400, 300, 200});
    VerticalBox* items = scroll->Add<VerticalBox>();
    std::vector<Button*> rows;
    for (int i = 0; i < 8; ++i) {
        rows.push_back(items->Add<Button>());
        rows.back()->Add<Spacer>(Vec2{100, 44}); // 60 tall each
    }
    s.Layout();
    s.router.SetFocus(rows[6]);
    AETHER_CHECK(Near(scroll->Offset(), 7 * 60 - 200) && rows[6]->Geometry().Bottom() <= 600.01f);

    // The bridge: held keys repeat after a delay; the stick and d-pad navigate; Tab steps; Enter accepts.
    UIInputBridge bridge(s.router);
    InputState in;
    s.router.SetFocus(grid[0][0]);
    grid[1][1]->enabled = true;
    grid[1][0]->visibility = Visibility::Visible;
    s.Layout();
    in.SetButton(Key::Right, true);
    bridge.Update(in, 0.016f);
    AETHER_CHECK(s.router.Focused() == grid[1][2]); // the explicit target from above
    grid[0][0]->navigation.right.clear();
    s.router.SetFocus(grid[0][0]);
    bridge.Update(in, 0.2f); // held, not yet repeating
    AETHER_CHECK(s.router.Focused() == grid[0][0]);
    bridge.Update(in, 0.25f); // past the delay: one repeat
    AETHER_CHECK(s.router.Focused() == grid[0][1]);
    bridge.Update(in, 0.08f);
    AETHER_CHECK(s.router.Focused() == grid[0][2]);
    in.SetButton(Key::Right, false);
    bridge.Update(in, 0.016f);
    in.SetAxis(Key::GamepadLeftStickY, -0.9f); // stick down
    bridge.Update(in, 0.016f);
    AETHER_CHECK(s.router.Focused() == grid[1][2]);
    in.SetAxis(Key::GamepadLeftStickY, 0.0f);
    bridge.Update(in, 0.016f);
    in.SetButton(Key::GamepadDPadLeft, true);
    bridge.Update(in, 0.016f);
    in.SetButton(Key::GamepadDPadLeft, false);
    bridge.Update(in, 0.016f);
    AETHER_CHECK(s.router.Focused() == grid[1][1]);
    in.SetButton(Key::LeftShift, true);
    in.SetButton(Key::Tab, true);
    bridge.Update(in, 0.016f);
    AETHER_CHECK(s.router.Focused() == grid[1][0]);
    in.Clear();
    bridge.Update(in, 0.016f);
    in.SetButton(Key::GamepadA, true);
    bridge.Update(in, 0.016f);
    bridge.Update(in, 1.0f); // accept doesn't repeat
    AETHER_CHECK(accepted == 3);
    in.Clear();
    bridge.Update(in, 0.016f);
    in.SetButton(Key::Escape, true);
    bridge.Update(in, 0.016f); // nothing takes cancel here: no effect
    AETHER_CHECK(s.router.Focused() == grid[1][0]);

    // A modal UI context keeps gameplay's input away while a menu is up.
    input::InputSystem game;
    game.AddAction({"Jump", input::ActionValueType::Bool});
    input::InputMappingContext play;
    play.name = "Gameplay";
    input::InputBinding jump;
    jump.action = "Jump";
    jump.key = Key::Space;
    play.bindings.push_back(jump);
    game.AddContext(play, 0);
    InputState keys;
    keys.SetButton(Key::Space, true);
    UIInputBridge::SetModal(game, true);
    game.Update(keys, 0.016f);
    AETHER_CHECK(!game.IsTriggered("Jump") && game.HasContext(UIInputBridge::kModalContext));
    UIInputBridge::SetModal(game, false);
    keys.SetButton(Key::Space, false);
    game.Update(keys, 0.016f);
    keys.SetButton(Key::Space, true);
    game.Update(keys, 0.016f);
    AETHER_CHECK(game.IsTriggered("Jump") && !game.HasContext(UIInputBridge::kModalContext));
}

AETHER_TEST(UI_TextInputDropdownListAndTooltips) {
    Screen s;
    TextInput* name = s.Place<TextInput>({100, 100, 300, 40});
    name->hint = "Your name";
    int changes = 0;
    std::string committed;
    name->on_changed = [&](const std::string&) { ++changes; };
    name->on_committed = [&](const std::string& t) { committed = t; };
    s.Layout();
    // Not focused: typing goes nowhere.
    AETHER_CHECK(!s.router.Text("x") && name->text.empty());
    s.Click({110, 110});
    AETHER_CHECK(s.router.Focused() == name && s.router.Text("h\xC3\xA9llo") && name->text == "h\xC3\xA9llo" && name->Cursor() == 6 && changes == 1);
    // Keys move by code points, select with shift, and delete.
    s.router.Key(EditKey::Left);
    s.router.Key(EditKey::Left, true);
    AETHER_CHECK(name->Cursor() == 4 && name->SelectionStart() == 4 && name->SelectionEnd() == 5);
    s.router.Text("L"); // replaces the selection
    AETHER_CHECK(name->text == "h\xC3\xA9lLo");
    s.router.Key(EditKey::Home);
    s.router.Key(EditKey::Right);
    s.router.Key(EditKey::Delete); // é is two bytes
    AETHER_CHECK(name->text == "hlLo" && name->Cursor() == 1);
    s.router.Key(EditKey::Backspace);
    AETHER_CHECK(name->text == "lLo" && name->Cursor() == 0);
    s.router.Key(EditKey::SelectAll);
    s.router.Key(EditKey::Backspace);
    AETHER_CHECK(name->text.empty());
    s.router.Text("a\tb\nc"); // no control characters in one line
    AETHER_CHECK(name->text == "abc");
    AETHER_CHECK(s.router.Accept() && committed == "abc");
    // A length limit, and passwords shown as dots.
    name->max_length = 5;
    s.router.Text("defgh");
    AETHER_CHECK(name->text == "abcde");
    name->password = true;
    AETHER_CHECK(name->Shown() == "*****");
    name->password = false;
    // Clicking places the cursor (12 per glyph from the left padding of 8); dragging selects.
    s.router.PointerDown({100 + 8 + 25, 110});
    AETHER_CHECK(name->Cursor() == 2);
    s.router.PointerMove({100 + 8 + 49, 110});
    s.router.PointerUp({100 + 8 + 49, 110});
    AETHER_CHECK(name->SelectionStart() == 2 && name->SelectionEnd() == 4);
    // Long text scrolls to keep the cursor in view.
    name->max_length = 0;
    name->SetText(std::string(60, 'x'));
    s.Layout();
    DrawList painted;
    name->Paint(painted, {&kFont, 1.0f});
    // Background, glyphs, then the caret, then the four sides of the focus outline.
    const DrawQuad& caret = painted.quads[painted.quads.size() - 5];
    AETHER_CHECK(name->Cursor() == 60 && Near(caret.rect.w, 2.0f) && caret.rect.x >= 108.0f && caret.rect.Right() <= 392.01f);
    // The bridge types-friendly: Space doesn't accept, Left edits, while the box has focus.
    UIInputBridge bridge(s.router);
    InputState in;
    committed.clear();
    in.SetButton(Key::Space, true);
    bridge.Update(in, 0.016f);
    in.Clear();
    bridge.Update(in, 0.016f);
    in.SetButton(Key::Left, true);
    bridge.Update(in, 0.016f);
    AETHER_CHECK(committed.empty() && name->Cursor() == 59 && s.router.Focused() == name);
    in.Clear();
    bridge.Update(in, 0.016f);

    // Dropdown: a popup of options that traps focus; accept picks, cancel or a click outside closes.
    Dropdown* quality = s.Place<Dropdown>({100, 300, 200, 40});
    quality->options = {"Low", "Medium", "High"};
    quality->selected = 1;
    i32 picked = -1;
    quality->on_changed = [&](i32 i) { picked = i; };
    s.Layout();
    s.Click({150, 310});
    AETHER_CHECK(quality->IsOpen() && s.router.PopupOpen() && s.router.Focused() != nullptr && s.router.Focused()->name == "option1");
    AETHER_CHECK(s.router.Focused()->Geometry().y >= 340.0f - 0.01f); // just below the box
    s.router.Navigate(NavDirection::Down);
    AETHER_CHECK(s.router.Focused()->name == "option2");
    s.router.Navigate(NavDirection::Down);
    AETHER_CHECK(s.router.Focused()->name == "option2"); // focus stays in the popup
    s.router.Accept();
    AETHER_CHECK(picked == 2 && quality->selected == 2 && !quality->IsOpen() && !s.router.PopupOpen() && s.router.Focused() == quality);
    s.router.Accept(); // open again
    AETHER_CHECK(s.router.PopupOpen() && s.router.Cancel() && !s.router.PopupOpen() && s.router.Focused() == quality);
    s.Click({150, 310});
    s.Click({1500, 900}); // outside: closes, picks nothing
    AETHER_CHECK(!s.router.PopupOpen() && picked == 2);
    s.Click({150, 310});
    const Rect low = s.router.Popup()->Find("option0")->Geometry();
    s.Click({low.x + 10, low.y + 5});
    AETHER_CHECK(picked == 0 && !s.router.PopupOpen());
    // Near the bottom of the screen it opens upwards.
    Dropdown* low_box = s.Place<Dropdown>({100, 1030, 200, 40});
    low_box->options = {"A", "B", "C"};
    s.Layout();
    s.router.SetFocus(low_box);
    s.router.Accept();
    AETHER_CHECK(s.router.PopupOpen() && s.router.Popup()->Find("option2")->Geometry().Bottom() <= 1030.01f);
    s.router.Cancel();

    // A virtualized list: only the rows in view exist.
    ListView* list = s.Place<ListView>({600, 100, 300, 200});
    list->item_count = 1000;
    list->item_height = 40;
    int binds = 0;
    list->make_row = [] { return std::make_unique<Text>(); };
    list->bind_row = [&](Widget& row, usize item, bool) {
        static_cast<Text&>(row).text = "Item " + std::to_string(item);
        ++binds;
    };
    i64 activated = -1;
    list->on_activated = [&](i64 i) { activated = i; };
    s.Layout();
    AETHER_CHECK(list->RowWidgets() == 5 && binds == 5 && Near(list->MaxOffset(), 1000 * 40 - 200));
    list->ScrollTo(500); // items 12.5 .. 17.5: six rows
    s.Layout();
    AETHER_CHECK(list->RowWidgets() == 6 && list->ItemOfRow(list->Child(0)) == 12 && static_cast<Text*>(list->Child(0))->text == "Item 12");
    s.Click({700, 100 + 55}); // 55 + 500 = 555: item 13
    AETHER_CHECK(list->selected == 13 && s.router.Focused() == list);
    for (int i = 0; i < 10; ++i) s.router.Navigate(NavDirection::Down);
    s.Layout();
    AETHER_CHECK(list->selected == 23 && Near(list->Offset(), 24 * 40 - 200));
    AETHER_CHECK(s.router.Accept() && activated == 23);
    s.router.Wheel({700, 150}, 2.0f);
    AETHER_CHECK(Near(list->Offset(), 24 * 40 - 200 - 120));
    list->Select(0);
    AETHER_CHECK(!list->OnNavigate(NavDirection::Up, s.router)); // at the top, up leaves the list

    // Tooltips: after a moment's hover; they don't catch clicks; moving away or pressing hides them.
    Button* help = s.Place<Button>({1200, 100, 100, 40});
    help->tooltip = "Opens the help";
    s.Layout();
    s.router.PointerMove({1210, 110});
    s.router.Tick(0.3f);
    AETHER_CHECK(!s.router.TooltipShown());
    s.router.Tick(0.3f);
    AETHER_CHECK(s.router.TooltipShown() && s.router.TooltipText() == "Opens the help");
    AETHER_CHECK(s.viewport.HitTest({1230, 135}) == help); // under the tooltip, the button
    s.router.PointerMove({1500, 500});
    AETHER_CHECK(!s.router.TooltipShown());
    s.router.PointerMove({1210, 110});
    s.router.Tick(1.0f);
    s.router.PointerDown({1210, 110});
    AETHER_CHECK(!s.router.TooltipShown());
    s.router.PointerUp({1210, 110});

    // Widgets removed while hovered or focused are forgotten safely.
    s.router.PointerMove({1210, 110});
    s.router.SetFocus(help);
    s.viewport.Remove(s.canvas);
    AETHER_CHECK(!s.router.PointerMove({1210, 110}) && s.router.Hovered() == nullptr && !s.router.Accept() && s.router.Focused() == nullptr);
}
