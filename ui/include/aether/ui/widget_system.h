#pragma once

#include "aether/ecs/world.h"
#include "aether/reflection/reflection.h"
#include "aether/ui/input.h"
#include "aether/ui/layout_file.h"

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace aether {

// Shows a UI layout (.aui) for its entity: a menu, a HUD (Phase 18 step 4,
// §18.4). With a Blueprint on the same entity it's a Widget Blueprint: the
// layout's controls raise its events (OnWidgetClicked and so on), and the UI
// library's nodes change it. Its bindings can read the entity's own
// components by type name ("Health.current").
struct WidgetComponent {
    std::string layout;
    i32 z = 0;
    bool visible = true;
};

// Blueprint function library for widgets, acting on the active UISystem.
// `target` is the entity whose WidgetComponent shows the layout; `widget`
// names a widget in it.
struct UI {
    static Entity CreateWidget(const std::string& layout, i32 z);
    static void RemoveWidget(const Entity& target);
    static void SetText(const Entity& target, const std::string& widget, const std::string& text);
    static std::string GetText(const Entity& target, const std::string& widget);
    static void SetVisible(const Entity& target, const std::string& widget, bool visible);
    static void SetEnabled(const Entity& target, const std::string& widget, bool enabled);
    static void SetValue(const Entity& target, const std::string& widget, f32 value); // a slider's value, a bar's percent, a toggle (0/1)
    static f32 GetValue(const Entity& target, const std::string& widget);
    static void PlayAnimation(const Entity& target, const std::string& animation);
    static void StopAnimation(const Entity& target, const std::string& animation);
    static void SetFocus(const Entity& target, const std::string& widget);
};

} // namespace aether

namespace aether::ui {

struct WidgetEvent {
    enum class Kind : u8 { Clicked, ValueChanged, CheckChanged, TextCommitted, SelectionChanged, AnimationFinished };
    Entity entity;
    Kind kind = Kind::Clicked;
    std::string widget; // or the animation's name
    f32 value = 0.0f;   // a slider's value, 1/0 for a check, a selection's index
    std::string text;   // committed text
};

// Runs the WidgetComponents of a world on a viewport: shows each one's
// layout (themed), keeps its bindings and animations going, and collects
// control events for Blueprints. Call Update each frame before drawing.
class UISystem {
public:
    using LayoutLoader = std::function<bool(const std::string& layout, LayoutDocument& out, std::string* error)>;
    UISystem(World& world, Viewport& viewport, UIInputRouter& router, LayoutLoader loader, const Theme* theme = nullptr);
    ~UISystem();
    UISystem(const UISystem&) = delete;
    UISystem& operator=(const UISystem&) = delete;

    void Update(f32 dt);

    // What the UI library calls.
    Entity CreateWidget(const std::string& layout, i32 z = 0); // a new entity showing it, at once
    // Takes the entity's WidgetComponent away (and its widgets, at once).
    void RemoveWidget(Entity entity);
    bool FocusWidget(Entity entity, const std::string& widget);
    Widget* RootOf(Entity entity) const;
    Widget* FindWidget(Entity entity, const std::string& widget) const;
    UIAnimator* AnimatorOf(Entity entity) const;
    DataBinder* BinderOf(Entity entity) const;
    // Sources every widget's bindings can read besides the entity's components ("game.score").
    void AddGlobalSource(const std::string& name, void* object, const reflect::TypeInfo& type);

    // Control events and finished animations from the last Update (and from calls since).
    const std::vector<WidgetEvent>& Events() const { return events_; }
    static const char* BlueprintEventName(WidgetEvent::Kind kind);
    // Missing layouts, bad bindings and animations, reported once each.
    const std::vector<std::string>& Problems() const { return problems_; }

    static UISystem* Active();
    void MakeActive();

private:
    struct Instance {
        Entity entity;
        std::string layout;
        i32 z = 0;
        Widget* root = nullptr; // owned by the viewport
        std::unique_ptr<UIAnimator> animator;
        std::unique_ptr<DataBinder> binder;
        std::vector<Binding> bindings;
        std::vector<std::string> sources; // component types its bindings read
        u64 seen = 0;
    };
    bool Create(Instance& in);
    Instance* Ensure(Entity e, const WidgetComponent& wc); // the entity's instance, made or remade as needed
    void Hook(Instance& in, Widget& w);
    void Refresh(Instance& in); // re-point sources at the entity's components (they move)
    void Destroy(Instance& in);
    Instance* Find(Entity e);
    const Instance* Find(Entity e) const;

    World& world_;
    Viewport& viewport_;
    UIInputRouter& router_;
    LayoutLoader loader_;
    const Theme* theme_;
    std::map<u32, std::unique_ptr<Instance>> instances_; // by entity index
    std::map<std::string, std::pair<void*, const reflect::TypeInfo*>> globals_;
    std::vector<WidgetEvent> events_, pending_;
    std::vector<std::string> problems_;
    std::map<std::string, bool> reported_;
    u64 generation_ = 0;
};

void RegisterWidgetComponents();

} // namespace aether::ui

AETHER_REFLECT(aether::WidgetComponent, 1,
    AETHER_FIELD(layout, Field_EditAnywhere, {.tooltip = "The UI layout (.aui) to show"}),
    AETHER_FIELD(z, Field_EditAnywhere, {.tooltip = "Layer order: higher is in front"}),
    AETHER_FIELD(visible, Field_EditAnywhere)
)

AETHER_REFLECT(aether::UI, 1,
    AETHER_METHOD(CreateWidget, Fn_BlueprintCallable, {"layout", "z"}),
    AETHER_METHOD(RemoveWidget, Fn_BlueprintCallable, {"target"}),
    AETHER_METHOD(SetText, Fn_BlueprintCallable, {"target", "widget", "text"}),
    AETHER_METHOD(GetText, Fn_BlueprintCallable | Fn_Pure, {"target", "widget"}),
    AETHER_METHOD(SetVisible, Fn_BlueprintCallable, {"target", "widget", "visible"}),
    AETHER_METHOD(SetEnabled, Fn_BlueprintCallable, {"target", "widget", "enabled"}),
    AETHER_METHOD(SetValue, Fn_BlueprintCallable, {"target", "widget", "value"}),
    AETHER_METHOD(GetValue, Fn_BlueprintCallable | Fn_Pure, {"target", "widget"}),
    AETHER_METHOD(PlayAnimation, Fn_BlueprintCallable, {"target", "animation"}),
    AETHER_METHOD(StopAnimation, Fn_BlueprintCallable, {"target", "animation"}),
    AETHER_METHOD(SetFocus, Fn_BlueprintCallable, {"target", "widget"})
)
