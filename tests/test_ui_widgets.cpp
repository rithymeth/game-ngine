#include "aether/blueprint/compiler.h"
#include "aether/blueprint/vm.h"
#include "aether/scene/components.h"
#include "aether/ui/basic.h"
#include "aether/ui/panels.h"
#include "aether/ui/widget_system.h"
#include "test_framework.h"

#include <cmath>
#include <map>

// Phase 18 step 4: render transforms, UI animations and tweens, and Widget
// Blueprints (WidgetComponent, UISystem, the UI node library, widget events).

namespace uitest4 {
struct Health {
    aether::f32 current = 80.0f;
    aether::f32 max = 100.0f;
};
} // namespace uitest4
AETHER_REFLECT(uitest4::Health, 1, AETHER_FIELD(current), AETHER_FIELD(max))

using namespace aether;
using namespace aether::ui;

namespace {

bool Near(f32 a, f32 b, f32 tol = 1e-3f) { return std::fabs(a - b) <= tol; }
bool Near(const Rect& a, const Rect& b, f32 tol = 1e-3f) { return Near(a.x, b.x, tol) && Near(a.y, b.y, tol) && Near(a.w, b.w, tol) && Near(a.h, b.h, tol); }
const BuiltinFont kFont;

} // namespace

AETHER_TEST(UI_RenderTransformsAndAnimations) {
    // A render transform scales about its pivot and moves, for drawing and hits, without moving the layout.
    Canvas canvas;
    Image* card = canvas.Add<Image>();
    card->slot.position = {100, 100};
    card->slot.size = {100, 50};
    Border* frame = canvas.Add<Border>();
    frame->background = Brush::Solid({});
    frame->slot.position = {400, 100};
    frame->slot.size = {100, 50};
    Image* inner = frame->Add<Image>();
    inner->desired_size = {10, 10};
    inner->slot.h_align = HAlign::Left;
    inner->slot.v_align = VAlign::Top;
    card->render_scale = {2, 2};
    card->render_offset = {10, 0};
    frame->render_scale = {2, 2};
    frame->render_pivot = {0, 0};
    LayoutContext ctx{&kFont};
    canvas.Measure(ctx);
    canvas.Arrange({0, 0, 1920, 1080}, ctx);
    AETHER_CHECK(Near(card->Geometry(), {100, 100, 100, 50})); // layout unchanged
    DrawList list;
    canvas.Paint(list, {&kFont, 1.0f});
    AETHER_CHECK(Near(list.quads[0].rect, {60, 75, 200, 100}));                            // card: about its centre, then moved
    AETHER_CHECK(Near(list.quads[1].rect, {400, 100, 200, 100}) && Near(list.quads[2].rect, {400, 100, 20, 20})); // frame and its child, from the top-left
    AETHER_CHECK(canvas.HitTest({65, 80}) == card && canvas.HitTest({250, 170}) == card && canvas.HitTest({265, 170}) == nullptr);
    AETHER_CHECK(canvas.HitTest({415, 115}) == inner && canvas.HitTest({590, 190}) == frame);

    // Easing curves.
    AETHER_CHECK(Near(ApplyEase(Ease::Linear, 0.5f), 0.5f) && Near(ApplyEase(Ease::EaseIn, 0.5f), 0.125f) && Near(ApplyEase(Ease::EaseOut, 0.5f), 0.875f));
    AETHER_CHECK(Near(ApplyEase(Ease::EaseInOut, 0.25f), 0.0625f) && Near(ApplyEase(Ease::EaseInOut, 0.75f), 0.9375f) && ApplyEase(Ease::Constant, 0.99f) == 0.0f);
    AETHER_CHECK(ApplyEase(Ease::Back, 0.8f) > 1.0f && Near(ApplyEase(Ease::Back, 1.0f), 1.0f) && Near(ApplyEase(Ease::Back, 0.0f), 0.0f));
    // A track: each key's ease shapes the way to the next.
    UITrack track{"x", "opacity", {{0.0f, 0.0f, Ease::EaseIn}, {1.0f, 1.0f, Ease::Linear}, {2.0f, 0.0f, Ease::Linear}}};
    AETHER_CHECK(Near(track.Evaluate(-1), 0) && Near(track.Evaluate(0.5f), 0.125f) && Near(track.Evaluate(1.5f), 0.5f) && Near(track.Evaluate(9), 0));

    // The animator.
    VerticalBox root;
    Border* panel = root.Add<Border>();
    panel->name = "Panel";
    Text* title = panel->Add<Text>("Hi");
    title->name = "Title";
    Slider* volume = root.Add<Slider>();
    volume->name = "Volume";
    volume->max = 100;
    UIAnimator anim(root);
    UIAnimation fade;
    fade.name = "FadeIn";
    fade.tracks.push_back({"Panel", "opacity", {{0.0f, 0.0f, Ease::EaseOut}, {0.5f, 1.0f}}});
    fade.tracks.push_back({"Title", "offset_y", {{0.0f, -40.0f, Ease::Linear}, {0.5f, 0.0f}}});
    anim.Add(fade);
    AETHER_CHECK(anim.Validate(fade).empty() && anim.Play("FadeIn") && Near(panel->opacity, 0.0f) && Near(title->render_offset.y, -40));
    anim.Tick(0.25f);
    AETHER_CHECK(Near(panel->opacity, 0.875f) && Near(title->render_offset.y, -20) && anim.IsPlaying("FadeIn") && Near(anim.TimeOf("FadeIn"), 0.25f));
    anim.Tick(0.3f);
    AETHER_CHECK(Near(panel->opacity, 1.0f) && !anim.IsPlaying("FadeIn") && anim.Finished() == std::vector<std::string>{"FadeIn"});
    anim.Tick(0.1f);
    AETHER_CHECK(anim.Finished().empty() && anim.TimeOf("FadeIn") < 0.0f);
    // Reversed, twice as fast.
    anim.Play("FadeIn", {2.0f, 1, true});
    AETHER_CHECK(Near(panel->opacity, 1.0f));
    anim.Tick(0.2f);
    AETHER_CHECK(Near(title->render_offset.y, -32) && anim.IsPlaying("FadeIn"));
    anim.Tick(0.1f);
    AETHER_CHECK(Near(panel->opacity, 0.0f) && !anim.IsPlaying("FadeIn"));
    // Looping: twice, then forever; stopping leaves it where it is.
    anim.Play("FadeIn", {1.0f, 2});
    anim.Tick(0.6f);
    AETHER_CHECK(anim.IsPlaying("FadeIn") && Near(anim.TimeOf("FadeIn"), 0.1f));
    anim.Tick(0.5f);
    AETHER_CHECK(!anim.IsPlaying("FadeIn"));
    anim.Play("FadeIn", {1.0f, 0});
    for (int i = 0; i < 20; ++i) anim.Tick(0.13f);
    AETHER_CHECK(anim.IsPlaying("FadeIn"));
    const f32 held = panel->opacity;
    anim.Stop("FadeIn");
    anim.Tick(0.2f);
    AETHER_CHECK(!anim.IsPlaying("FadeIn") && panel->opacity == held && anim.Finished().empty());
    // Tweens from the current value.
    volume->value = 20;
    const std::string tw = anim.Tween("Volume", "value", 80, 1.0f, Ease::Linear);
    anim.Tick(0.5f);
    AETHER_CHECK(Near(volume->value, 50) && anim.IsPlaying(tw));
    anim.Tween("Volume", "value", 0, 0.5f, Ease::Linear); // replaces it, from 50
    anim.Tick(0.25f);
    AETHER_CHECK(Near(volume->value, 25));
    anim.Tick(1.0f);
    AETHER_CHECK(Near(volume->value, 0) && anim.Finished().size() == 1);
    // Other properties.
    AETHER_CHECK(SetProperty(*title, "tint_a", 0.5f) && Near(title->color.a, 0.5f) && SetProperty(*panel, "position_x", 12) && Near(panel->slot.position.x, 12));
    AETHER_CHECK(SetProperty(*title, "scale", 3) && Near(title->render_scale.y, 3) && !SetProperty(*title, "value", 1) && !SetProperty(*volume, "tint_r", 1));
    f32 v = 0;
    AETHER_CHECK(GetProperty(*volume, "value", v) && Near(v, 0) && !GetProperty(*title, "percent", v));
    UIAnimation bad;
    bad.name = "Bad";
    bad.tracks.push_back({"Nobody", "opacity", {{0, 1}}});
    bad.tracks.push_back({"Title", "percent", {{0, 1}}});
    bad.tracks.push_back({"Panel", "opacity", {}});
    AETHER_CHECK(anim.Validate(bad).size() == 3 && !anim.Play("Missing"));

    // JSON, alone and in a layout file.
    UIAnimation back;
    std::string error;
    AETHER_CHECK(AnimationFromJson(AnimationToJson(fade), back, &error) && AnimationToJson(back) == AnimationToJson(fade));
    AETHER_CHECK(!AnimationFromJson(nlohmann::json::parse(R"({"name":"A","tracks":[{"widget":"x","property":"wobble","keys":[[0,1]]}]})"), back, &error));
    AETHER_CHECK(!AnimationFromJson(nlohmann::json::parse(R"({"name":"A","tracks":[{"widget":"x","property":"opacity","keys":[[0,1,"Bouncy"]]}]})"), back, &error) &&
                 error.find("Bouncy") != std::string::npos);
    AETHER_CHECK(!AnimationFromJson(nlohmann::json::parse(R"({"tracks":[]})"), back, &error));
    const std::string file = SaveLayout(root, {}, {fade});
    LayoutDocument doc;
    AETHER_CHECK(LoadLayout(file, doc, {}, &error) && doc.animations.size() == 1 && doc.animations[0].tracks.size() == 2 && SaveLayout(*doc.root, {}, doc.animations) == file);
}

AETHER_TEST(UI_WidgetBlueprintsAndSystem) {
    // Layouts by name: a HUD bound to the entity's Health, and a menu.
    const std::map<std::string, std::string> files = {
        {"HUD", R"({"version":1,
            "root":{"type":"Canvas","children":[
                {"type":"Text","name":"HPText","slot":{"position":[20,20],"auto_size":true}},
                {"type":"ProgressBar","name":"HPBar","slot":{"position":[20,60],"size":[200,20]}},
                {"type":"Text","name":"Status","text":"","slot":{"position":[20,100],"auto_size":true}},
                {"type":"Button","name":"Pause","class":"Primary","slot":{"position":[1700,20],"size":[200,60]},"children":[{"type":"Text","text":"Pause"}]},
                {"type":"Slider","name":"Volume","max":100,"slot":{"position":[1600,200],"size":[200,24]}}]},
            "bindings":[{"widget":"HPText","property":"text","source":"Health.current","format":"HP {}"},
                        {"widget":"HPBar","property":"percent","source":"Health.current","divide_by":"Health.max"}],
            "animations":[{"name":"Flash","tracks":[{"widget":"Status","property":"opacity","keys":[[0,0],[0.2,1]]}]}]})"},
        {"Menu", R"({"root":{"type":"Canvas","children":[{"type":"Text","name":"Title","text":"Menu"}]}})"},
    };
    UISystem::LayoutLoader loader = [&](const std::string& name, LayoutDocument& out, std::string* error) {
        const auto it = files.find(name);
        if (it == files.end()) {
            if (error != nullptr) *error = "no such file";
            return false;
        }
        return LoadLayout(it->second, out, {}, error);
    };
    Theme theme;
    ControlStyle primary;
    primary.text_size = 30;
    theme.styles["Button.Primary"] = primary;

    World world;
    (void)GetComponentId<uitest4::Health>();
    Viewport vp;
    UIInputRouter router(vp, kFont);
    UISystem sys(world, vp, router, loader, &theme);
    AETHER_CHECK(UISystem::Active() == &sys);
    WidgetComponent hud;
    hud.layout = "HUD";
    const Entity player = world.CreateEntity(uitest4::Health{}, hud);
    sys.Update(0.016f);
    for (const std::string& p : sys.Problems()) std::printf("    %s\n", p.c_str());
    AETHER_CHECK(sys.Problems().empty() && sys.RootOf(player) != nullptr && vp.LayerCount() == 1);
    auto* hp_text = static_cast<Text*>(sys.FindWidget(player, "HPText"));
    auto* hp_bar = static_cast<ProgressBar*>(sys.FindWidget(player, "HPBar"));
    AETHER_CHECK(hp_text->text == "HP 80" && Near(hp_bar->percent, 0.8f));
    AETHER_CHECK(Near(static_cast<Button*>(sys.FindWidget(player, "Pause"))->style.text_size, 30)); // themed
    // Bindings follow the component, even after the entity changes archetype (its storage moves).
    world.GetComponent<uitest4::Health>(player)->current = 40;
    world.AddComponent(player, Transform{});
    sys.Update(0.016f);
    AETHER_CHECK(hp_text->text == "HP 40" && Near(hp_bar->percent, 0.4f));

    // Controls raise events for the next Update.
    const Rect pause = sys.FindWidget(player, "Pause")->Geometry();
    router.PointerMove({pause.x + 10, pause.y + 10});
    router.PointerDown({pause.x + 10, pause.y + 10});
    router.PointerUp({pause.x + 10, pause.y + 10});
    static_cast<Slider*>(sys.FindWidget(player, "Volume"))->SetValue(30);
    sys.Update(0.016f);
    AETHER_CHECK(sys.Events().size() == 2 && sys.Events()[0].kind == WidgetEvent::Kind::Clicked && sys.Events()[0].widget == "Pause");
    AETHER_CHECK(sys.Events()[1].kind == WidgetEvent::Kind::ValueChanged && Near(sys.Events()[1].value, 30) && sys.Events()[0].entity == player);
    sys.Update(0.016f);
    AETHER_CHECK(sys.Events().empty());
    // Animations run each Update and report when they end.
    sys.AnimatorOf(player)->Play("Flash");
    sys.Update(0.1f);
    AETHER_CHECK(Near(sys.FindWidget(player, "Status")->opacity, 0.5f) && sys.Events().empty());
    sys.Update(0.15f);
    AETHER_CHECK(sys.Events().size() == 1 && sys.Events()[0].kind == WidgetEvent::Kind::AnimationFinished && sys.Events()[0].widget == "Flash");
    AETHER_CHECK(std::string(UISystem::BlueprintEventName(WidgetEvent::Kind::Clicked)) == "Event.OnWidgetClicked");

    // A Widget Blueprint on the entity: BeginPlay makes a menu and titles it; OnWidgetClicked
    // prints the widget, sets the status and plays the flash.
    bp::Blueprint blueprint;
    bp::Graph events;
    events.name = "EventGraph";
    blueprint.graphs.push_back(events);
    bp::GraphBuilder b(*blueprint.FindGraph("EventGraph"));
    const bp::NodeId begin = b.Add("Event.BeginPlay"), create = b.Add("Call.Native:UI.CreateWidget"), title = b.Add("Call.Native:UI.SetText");
    b.Default(create, "layout", "Menu").Default(create, "z", 5);
    b.Default(title, "widget", "Title").Default(title, "text", "Paused");
    b.Connect(begin, "then", create, "exec").Connect(create, "then", title, "exec").Connect(create, "return", title, "target");
    const bp::NodeId clicked = b.Add("Event.OnWidgetClicked"), print = b.Add("Debug.Print"), self = b.Add("Entity.Self");
    const bp::NodeId status = b.Add("Call.Native:UI.SetText"), flash = b.Add("Call.Native:UI.PlayAnimation");
    b.Default(status, "widget", "Status").Default(status, "text", "Game paused");
    b.Default(flash, "animation", "Flash");
    b.Connect(clicked, "then", print, "exec").Connect(clicked, "widget", print, "text");
    b.Connect(print, "then", status, "exec").Connect(self, "self", status, "target");
    b.Connect(status, "then", flash, "exec").Connect(self, "self", flash, "target");
    bp::CompileResult compiled = bp::CompileBlueprint(blueprint);
    for (const bp::Diagnostic& d : compiled.diagnostics.diagnostics) std::printf("    %s: %s\n", d.code.c_str(), d.message.c_str());
    AETHER_CHECK(compiled.Ok());
    bp::BlueprintVM vm(world);
    std::vector<std::string> printed;
    vm.SetPrintHandler([&](Entity, const std::string& text) { printed.push_back(text); });
    AETHER_CHECK(vm.Attach(player, compiled.blueprint));
    vm.BeginPlay();
    AETHER_CHECK(vp.LayerCount() == 2 && vp.Layer(1)->Find("Title") != nullptr && static_cast<Text*>(vp.Layer(1)->Find("Title"))->text == "Paused");
    router.PointerMove({pause.x + 10, pause.y + 10});
    router.PointerDown({pause.x + 10, pause.y + 10});
    router.PointerUp({pause.x + 10, pause.y + 10});
    sys.Update(0.016f);
    for (const WidgetEvent& ev : sys.Events()) {
        if (ev.kind != WidgetEvent::Kind::Clicked) continue;
        const bp::VmValue args[] = {ev.widget};
        vm.Dispatch(ev.entity, UISystem::BlueprintEventName(ev.kind), args);
    }
    AETHER_CHECK(printed == std::vector<std::string>{"Pause"});
    AETHER_CHECK(static_cast<Text*>(sys.FindWidget(player, "Status"))->text == "Game paused" && sys.AnimatorOf(player)->IsPlaying("Flash"));

    // The library directly.
    UI::SetValue(player, "Volume", 250);
    AETHER_CHECK(Near(UI::GetValue(player, "Volume"), 100) && Near(UI::GetValue(player, "HPBar"), 0.4f)); // clamped to the slider
    UI::SetText(player, "Pause", "Resume"); // a button's label
    AETHER_CHECK(UI::GetText(player, "Pause").empty() && static_cast<Text*>(sys.FindWidget(player, "Pause")->Child(0))->text == "Resume");
    // A string table key (§29.3): set by key, cleared by setting text.
    UI::SetTextKey(player, "Pause", "pause.label", "Pause");
    AETHER_CHECK(static_cast<Text*>(sys.FindWidget(player, "Pause")->Child(0))->text_key == "pause.label" &&
                 static_cast<Text*>(sys.FindWidget(player, "Pause")->Child(0))->text == "Pause");
    UI::SetText(player, "Pause", "Resume");
    AETHER_CHECK(static_cast<Text*>(sys.FindWidget(player, "Pause")->Child(0))->text_key.empty());
    UI::SetVisible(player, "Status", false);
    UI::SetEnabled(player, "Pause", false);
    AETHER_CHECK(sys.FindWidget(player, "Status")->visibility == Visibility::Collapsed && !sys.FindWidget(player, "Pause")->enabled);
    UI::SetEnabled(player, "Pause", true);
    UI::SetFocus(player, "Pause");
    AETHER_CHECK(router.Focused() == sys.FindWidget(player, "Pause"));
    UI::StopAnimation(player, "Flash");
    AETHER_CHECK(!sys.AnimatorOf(player)->IsPlaying("Flash"));

    // Hiding, removing, changing layouts, missing layouts.
    world.GetComponent<WidgetComponent>(player)->visible = false;
    sys.Update(0.016f);
    AETHER_CHECK(sys.RootOf(player)->visibility == Visibility::Collapsed);
    const Entity menu = world.CreateEntity(WidgetComponent{"Menu", 5, true});
    sys.Update(0.016f);
    AETHER_CHECK(vp.LayerCount() == 3);
    UI::RemoveWidget(menu);
    AETHER_CHECK(vp.LayerCount() == 2 && world.GetComponent<WidgetComponent>(menu) == nullptr && world.IsAlive(menu));
    world.GetComponent<WidgetComponent>(player)->layout = "Menu";
    sys.Update(0.016f);
    AETHER_CHECK(sys.FindWidget(player, "Title") != nullptr && sys.FindWidget(player, "HPText") == nullptr && vp.LayerCount() == 2);
    world.DestroyEntity(player);
    sys.Update(0.016f);
    AETHER_CHECK(vp.LayerCount() == 1 && sys.RootOf(player) == nullptr);
    world.CreateEntity(WidgetComponent{"Nope", 0, true});
    sys.Update(0.016f);
    sys.Update(0.016f);
    AETHER_CHECK(sys.Problems().size() == 1 && sys.Problems()[0].find("Nope") != std::string::npos);
}
