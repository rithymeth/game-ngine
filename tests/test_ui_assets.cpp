#include "aether/ui/basic.h"
#include "aether/ui/binding.h"
#include "aether/ui/input.h"
#include "aether/ui/layout_file.h"
#include "aether/ui/panels.h"
#include "aether/ui/theme.h"
#include "test_framework.h"

#include <cmath>

// Phase 18 step 3: themes (.atheme), layout files (.aui) and data binding.

namespace uitest {
struct Stats {
    aether::f32 mana = 50.0f;
    aether::i32 level = 3;
};
struct Player {
    std::string name = "Aria";
    aether::f32 health = 75.0f;
    aether::f32 max_health = 100.0f;
    bool alive = true;
    aether::u32 gold = 1234;
    Stats stats;
};
} // namespace uitest

AETHER_REFLECT(uitest::Stats, 1, AETHER_FIELD(mana), AETHER_FIELD(level))
AETHER_REFLECT(uitest::Player, 1, AETHER_FIELD(name), AETHER_FIELD(health), AETHER_FIELD(max_health), AETHER_FIELD(alive), AETHER_FIELD(gold),
               AETHER_FIELD(stats))

using namespace aether;
using namespace aether::ui;

namespace {

bool Near(f32 a, f32 b, f32 tol = 1e-3f) { return std::fabs(a - b) <= tol; }
bool Same(const Color& a, const Color& b) { return Near(a.r, b.r) && Near(a.g, b.g) && Near(a.b, b.b) && Near(a.a, b.a); }

const BuiltinFont kFont;

// A game's own widget type, saved in layouts once registered.
class HealthOrb final : public Widget {
public:
    const char* TypeName() const override { return "HealthOrb"; }
    f32 radius = 32.0f;
};

} // namespace

AETHER_TEST(UI_ThemesLoadExtendAndApply) {
    // Colours: hex, palette names, arrays; bad ones say why.
    Color c;
    AETHER_CHECK(ColorFromJson("#FF8000", c) && Same(c, {1, 128 / 255.0f, 0, 1}));
    AETHER_CHECK(ColorFromJson("#11223344", c) && Near(c.a, 0x44 / 255.0f) && Near(c.r, 0x11 / 255.0f));
    AETHER_CHECK(ColorFromJson(nlohmann::json::array({0.1, 0.2, 0.3}), c) && Near(c.a, 1.0f));
    AETHER_CHECK(ColorFromJson("@accent", c, {{"accent", {0, 1, 0, 1}}}) && Same(c, {0, 1, 0, 1}));
    std::string error;
    AETHER_CHECK(!ColorFromJson("#12", c, {}, &error) && !error.empty());
    AETHER_CHECK(!ColorFromJson("@nope", c, {}, &error) && error.find("nope") != std::string::npos);
    // Brushes: a bare colour is solid; images resolve to texture ids; they round-trip.
    Brush b;
    AETHER_CHECK(BrushFromJson("#FFFFFF", b) && b.kind == Brush::Kind::Color);
    const TextureResolver textures = [](const std::string& image) { return image == "ui/panel.png" ? 42u : 0u; };
    AETHER_CHECK(BrushFromJson(nlohmann::json::parse(R"({"kind":"Box","image":"ui/panel.png","image_size":[64,64],"slice":[8,8,8,8],"tint":"#FF0000"})"), b, {},
                               textures));
    AETHER_CHECK(b.kind == Brush::Kind::Box && b.texture == 42 && b.slice.left == 8 && Same(b.tint, {1, 0, 0, 1}) && b.image == "ui/panel.png");
    Brush again;
    AETHER_CHECK(BrushFromJson(BrushToJson(b), again) && BrushToJson(again) == BrushToJson(b));
    AETHER_CHECK(!BrushFromJson(nlohmann::json::parse(R"({"kind":"Blob"})"), b, {}, {}, &error));

    // A theme: a palette, a base button, a primary one that extends it (listed first), text styles.
    const std::string text = R"({
        "version": 1, "name": "Neon",
        "colors": { "accent": "#00FFAA", "panel": [0.1, 0.1, 0.15, 0.9] },
        "styles": {
            "Button.Primary": { "extends": "Button", "accent": "@accent", "text_size": 28 },
            "Button": { "normal": "@panel", "hovered": "#333344", "text": "#FFFFFF" },
            "Default": { "normal": "#222222" },
            "ProgressBar": { "normal": "#000000", "accent": "@accent" },
            "Border.Panel": { "normal": { "kind": "Box", "image": "ui/panel.png", "slice": [8, 8, 8, 8], "tint": "@panel" } }
        },
        "text": { "Default": { "color": "#CCCCCC", "size": 18 }, "Title": { "color": "@accent", "size": 64 } }
    })";
    Theme theme;
    AETHER_CHECK(LoadTheme(text, theme, textures, &error));
    const ControlStyle& primary = theme.StyleFor("Button", "Primary");
    AETHER_CHECK(Same(primary.accent.tint, {0, 1, 170 / 255.0f, 1}) && Near(primary.text_size, 28) && Same(primary.normal.tint, {0.1f, 0.1f, 0.15f, 0.9f}));
    AETHER_CHECK(Same(primary.text, {1, 1, 1, 1})); // inherited from Button
    AETHER_CHECK(&theme.StyleFor("Button", "Nope") == &theme.styles.at("Button") && &theme.StyleFor("Toggle", "") == &theme.styles.at("Default"));
    AETHER_CHECK(theme.styles.at("Border.Panel").normal.texture == 42);
    AETHER_CHECK(Near(theme.TextFor("Title")->size, 64) && Near(theme.TextFor("Other")->size, 18));
    // It saves and loads to the same thing.
    Theme reloaded;
    AETHER_CHECK(LoadTheme(SaveTheme(theme), reloaded, textures, &error) && SaveTheme(reloaded) == SaveTheme(theme));
    // Broken themes.
    AETHER_CHECK(!LoadTheme(R"({"styles":{"A":{"extends":"B"},"B":{"extends":"A"}}})", reloaded, {}, &error) && error.find("loop") != std::string::npos);
    AETHER_CHECK(!LoadTheme(R"({"styles":{"A":{"extends":"Nope"}}})", reloaded, {}, &error) && error.find("Nope") != std::string::npos);
    AETHER_CHECK(!LoadTheme(R"({"styles":{"A":{"normal":"@missing"}}})", reloaded, {}, &error) && error.find("A") != std::string::npos);
    AETHER_CHECK(!LoadTheme(R"({"version":7})", reloaded, {}, &error) && error.find("newer") != std::string::npos);
    AETHER_CHECK(!LoadTheme("{nope", reloaded, {}, &error));

    // Applying it styles the tree by type and class.
    VerticalBox root;
    Button* play = root.Add<Button>();
    play->style_class = "Primary";
    Button* quit = root.Add<Button>();
    Text* title = root.Add<Text>("Game");
    title->style_class = "Title";
    Text* body = root.Add<Text>("Hello");
    ProgressBar* bar = root.Add<ProgressBar>();
    Border* panel = root.Add<Border>();
    panel->style_class = "Panel";
    Border* plain = root.Add<Border>();
    plain->background = Brush::Solid({1, 0, 1, 1});
    ApplyTheme(theme, root);
    AETHER_CHECK(Near(play->style.text_size, 28) && Same(quit->style.normal.tint, {0.1f, 0.1f, 0.15f, 0.9f}) && Near(quit->style.text_size, 20));
    AETHER_CHECK(Near(title->size, 64) && Same(title->color, {0, 1, 170 / 255.0f, 1}) && Near(body->size, 18));
    AETHER_CHECK(Same(bar->bar.tint, {0, 1, 170 / 255.0f, 1}) && panel->background.kind == Brush::Kind::Box && Same(plain->background.tint, {1, 0, 1, 1}));
}

AETHER_TEST(UI_LayoutFilesRoundTripAndReport) {
    // A menu with a bit of everything.
    auto root = std::make_unique<Canvas>();
    root->name = "Menu";
    auto* column = root->Add<VerticalBox>();
    column->name = "Column";
    column->spacing = 10;
    column->slot.anchors = Anchors::Point(0.5f, 0.5f);
    column->slot.alignment = {0.5f, 0.5f};
    column->slot.auto_size = true;
    auto* title = column->Add<Text>("MY GAME");
    title->name = "Title";
    title->style_class = "Title";
    title->justify = TextAlign::Center;
    auto* play = column->Add<Button>();
    play->name = "Play";
    play->tooltip = "Start a new game";
    play->navigation.down = "Volume";
    play->Add<Text>("Play");
    auto* volume = column->Add<Slider>();
    volume->name = "Volume";
    volume->min = 0, volume->max = 10, volume->step = 1, volume->value = 7;
    auto* sound = column->Add<Toggle>();
    sound->checked = true;
    auto* quality = column->Add<Dropdown>();
    quality->options = {"Low", "High"};
    quality->selected = 1;
    auto* name = column->Add<TextInput>();
    name->hint = "Name";
    name->max_length = 12;
    name->password = true;
    auto* hp = column->Add<ProgressBar>();
    hp->percent = 0.4f;
    hp->fill = ProgressBar::Fill::RightToLeft;
    auto* icon = column->Add<Image>();
    icon->brush.kind = Brush::Kind::Image;
    icon->brush.image = "ui/icon.png";
    icon->desired_size = {32, 32};
    icon->visibility = Visibility::Hidden;
    icon->opacity = 0.5f;
    auto* grid = column->Add<Grid>();
    grid->column_fill = {0, 1};
    grid->Add<Spacer>(Vec2{10, 10})->slot.column = 1;
    auto* size = column->Add<SizeBox>();
    size->max_width = 300;
    auto* frame = size->Add<Border>();
    frame->padding = Margin::All(4);
    frame->enabled = false;
    auto* scroll = column->Add<ScrollBox>();
    scroll->horizontal = true;
    auto* list = column->Add<ListView>();
    list->item_height = 30;
    std::vector<Binding> bindings = {{"Title", "text", "game.title"}, {"Volume", "value", "settings.volume", "", "", 0, false, true}};
    const std::string saved = SaveLayout(*root, bindings);

    u32 resolved = 0;
    const TextureResolver textures = [&](const std::string& image) {
        ++resolved;
        return image == "ui/icon.png" ? 7u : 0u;
    };
    LayoutDocument doc;
    std::string error;
    AETHER_CHECK(LoadLayout(saved, doc, textures, &error));
    AETHER_CHECK(SaveLayout(*doc.root, doc.bindings) == saved); // the same file again
    Widget& menu = *doc.root;
    AETHER_CHECK(menu.Find("Play") != nullptr && menu.Find("Play")->tooltip == "Start a new game" && menu.Find("Play")->navigation.down == "Volume");
    AETHER_CHECK(menu.Find("Column")->slot.auto_size && Near(menu.Find("Column")->slot.anchors.min_x, 0.5f));
    auto* v = menu.FindAs<Slider>("Volume");
    AETHER_CHECK(v != nullptr && Near(v->value, 7) && Near(v->max, 10) && Near(v->step, 1));
    AETHER_CHECK(resolved == 1 && doc.bindings.size() == 2 && doc.bindings[1].two_way && doc.bindings[0].source == "game.title");
    // Everything laid out works like the original.
    Viewport vp;
    vp.Add(std::move(doc.root));
    vp.Layout(kFont);
    AETHER_CHECK(vp.Find("Play")->Geometry().w > 0.0f);
    // Default slots aren't written.
    Text plain_text("x");
    AETHER_CHECK(!WidgetToJson(plain_text).contains("slot"));

    // Errors say where.
    const auto fails = [&](const std::string& json, const std::string& expect) {
        LayoutDocument d;
        std::string e;
        const bool ok = LoadLayout(json, d, {}, &e);
        const bool found = e.find(expect) != std::string::npos;
        if (ok || !found) std::printf("    layout error was: %s\n", e.c_str());
        return !ok && found;
    };
    AETHER_CHECK(fails(R"({"root":{"type":"Canvas","name":"Menu","children":[{"type":"VerticalBox","children":[{"type":"Buton"}]}]}})", "Menu/VerticalBox[0]/Buton[0]"));
    AETHER_CHECK(fails(R"({"root":{"type":"SizeBox","children":[{"type":"Spacer"},{"type":"Spacer"}]}})", "at most 1 child"));
    AETHER_CHECK(fails(R"({"root":{"type":"Text","children":[{"type":"Spacer"}]}})", "takes no children"));
    AETHER_CHECK(fails(R"({"root":{"type":"Text","visibility":"Invisible"}})", "Invisible"));
    AETHER_CHECK(fails(R"({"root":{"type":"Canvas","slot":{"h_align":"Middle"}}})", "Middle"));
    AETHER_CHECK(fails(R"({"root":{"type":"Canvas"},"bindings":[{"property":"text"}]})", "malformed binding"));
    AETHER_CHECK(fails(R"({"version":3,"root":{"type":"Canvas"}})", "newer"));
    AETHER_CHECK(fails(R"({"nope":1})", "root"));
    AETHER_CHECK(fails("[1,", "valid JSON"));

    // A game's own widget type.
    RegisterWidgetType({"HealthOrb", [] { return std::make_unique<HealthOrb>(); },
                        [](const Widget& w, nlohmann::json& j) { j["radius"] = static_cast<const HealthOrb&>(w).radius; },
                        [](Widget& w, const nlohmann::json& j, const TextureResolver&, std::string*) {
                            static_cast<HealthOrb&>(w).radius = j.value("radius", 32.0f);
                            return true;
                        }});
    HealthOrb orb;
    orb.radius = 50;
    std::unique_ptr<Widget> back = WidgetFromJson(WidgetToJson(orb));
    AETHER_CHECK(back != nullptr && Near(static_cast<HealthOrb&>(*back).radius, 50));
    const auto names = WidgetTypeNames();
    AETHER_CHECK(std::find(names.begin(), names.end(), "HealthOrb") != names.end() && FindWidgetType("Button") != nullptr);
}

AETHER_TEST(UI_DataBindingFollowsFields) {
    uitest::Player player;
    Viewport vp;
    UIInputRouter router(vp, kFont);
    auto* root = static_cast<VerticalBox*>(vp.Add(std::make_unique<VerticalBox>()));
    auto make_text = [&](const char* n) {
        Text* t = root->Add<Text>();
        t->name = n;
        return t;
    };
    Text* name = make_text("Name");
    Text* hp = make_text("HP");
    Text* mana = make_text("Mana");
    Text* gold = make_text("Gold");
    Text* alive_text = make_text("AliveText");
    ProgressBar* bar = root->Add<ProgressBar>();
    bar->name = "Bar";
    Image* skull = root->Add<Image>();
    skull->name = "Skull";
    Slider* slider = root->Add<Slider>();
    slider->name = "ManaSlider";
    slider->max = 100;
    int slider_calls = 0;
    slider->on_changed = [&](f32) { ++slider_calls; };
    Toggle* toggle = root->Add<Toggle>();
    toggle->name = "AliveToggle";
    TextInput* input = root->Add<TextInput>();
    input->name = "NameInput";
    Dropdown* level = root->Add<Dropdown>();
    level->name = "Level";
    level->options = {"0", "1", "2", "3"};
    vp.Layout(kFont);

    DataBinder binder;
    binder.AddSource("player", player);
    const std::vector<Binding> bindings = {
        {"Name", "text", "player.name"},
        {"HP", "text", "player.health", "", "HP {} / 100"},
        {"Mana", "text", "player.stats.mana", "", "", 1},
        {"Gold", "text", "player.gold"},
        {"AliveText", "text", "player.alive"},
        {"Bar", "percent", "player.health", "player.max_health"},
        {"Skull", "visible", "player.alive", "", "", 0, true},
        {"ManaSlider", "value", "player.stats.mana", "", "", 0, false, true},
        {"AliveToggle", "checked", "player.alive", "", "", 0, false, true},
        {"NameInput", "text", "player.name", "", "", 0, false, true},
        {"Level", "selected", "player.stats.level", "", "", 0, false, true},
    };
    const std::vector<std::string> problems = binder.Bind(*root, bindings);
    for (const std::string& p : problems) std::printf("    %s\n", p.c_str());
    AETHER_CHECK(problems.empty());
    binder.Update();
    AETHER_CHECK(name->text == "Aria" && hp->text == "HP 75 / 100" && mana->text == "50.0" && gold->text == "1234" && alive_text->text == "true");
    AETHER_CHECK(Near(bar->percent, 0.75f) && skull->visibility == Visibility::Collapsed && Near(slider->value, 50) && toggle->checked);
    AETHER_CHECK(input->text == "Aria" && level->selected == 3 && slider_calls == 0); // updates don't call on_changed
    // Changes follow; unchanged values leave the widget alone.
    player.health = 50;
    player.stats.mana = 12.25f;
    binder.Update();
    AETHER_CHECK(hp->text == "HP 50 / 100" && Near(bar->percent, 0.5f) && mana->text == "12.2" && Near(slider->value, 12.25f));
    name->text = "edited";
    binder.Update();
    AETHER_CHECK(name->text == "edited");
    // Two-way: controls write back (and still call what they had).
    slider->SetValue(80);
    AETHER_CHECK(Near(player.stats.mana, 80) && slider_calls == 1);
    toggle->SetChecked(false);
    binder.Update();
    AETHER_CHECK(!player.alive && skull->visibility == Visibility::Visible && alive_text->text == "false");
    level->Select(1);
    AETHER_CHECK(player.stats.level == 1);
    router.SetFocus(input);
    router.Key(EditKey::End);
    router.Text("na");
    AETHER_CHECK(player.name == "Ariana");
    player.name = "Zed";
    binder.Update();
    AETHER_CHECK(input->text == "Ariana"); // not while it's being typed in
    router.ClearFocus();
    binder.Update();
    AETHER_CHECK(input->text == "Zed" && name->text == "Zed");

    // Problems, and the binding skipped.
    const std::vector<std::string> bad = binder.Bind(*root, {{"Nobody", "text", "player.name"},
                                                             {"HP", "text", "player.nope"},
                                                             {"HP", "text", "enemy.health"},
                                                             {"HP", "percent", "player.health"},
                                                             {"HP", "text", "player"},
                                                             {"HP", "text", "player.health", "", "", 0, false, true}});
    AETHER_CHECK(bad.size() == 6);
    AETHER_CHECK(bad[0].find("Nobody") != std::string::npos && bad[1].find("nope") != std::string::npos && bad[2].find("enemy") != std::string::npos);
    AETHER_CHECK(bad[3].find("percent") != std::string::npos && bad[5].find("can't write back") != std::string::npos);
    // Paths directly.
    f64 n = 0;
    std::string s;
    AETHER_CHECK(binder.ReadNumber("player.gold", n) && n == 1234 && binder.WriteNumber("player.gold", 99.6) && player.gold == 100);
    AETHER_CHECK(binder.ReadText("player.name", s) && s == "Zed" && binder.WriteText("player.name", "Q") && player.name == "Q");
    AETHER_CHECK(!binder.WriteText("player.health", "x") && !binder.ReadNumber("player.stats.nope", n));
    // Removing a source leaves the widgets as they are.
    binder.RemoveSource("player");
    player.health = 1;
    binder.Update();
    AETHER_CHECK(hp->text != "HP 1 / 100");
}
