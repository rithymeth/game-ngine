#include "aether/ui/widget_system.h"

#include "aether/ecs/component.h"
#include "aether/ui/basic.h"
#include "aether/ui/controls.h"
#include "aether/ui/world_ui.h"

#include <algorithm>

namespace aether {

namespace {
ui::UISystem* g_active = nullptr;

// The entity's screen widget, else its world-space one.
ui::Widget* Target(const Entity& e, const std::string& widget) {
    if (ui::Widget* w = g_active != nullptr ? g_active->FindWidget(e, widget) : nullptr) return w;
    ui::WorldUISystem* world = ui::WorldUISystem::Active();
    return world != nullptr ? world->FindWidget(e, widget) : nullptr;
}

ui::UIAnimator* AnimatorFor(const Entity& e) {
    if (ui::UIAnimator* a = g_active != nullptr ? g_active->AnimatorOf(e) : nullptr) return a;
    ui::WorldUISystem* world = ui::WorldUISystem::Active();
    return world != nullptr ? world->AnimatorOf(e) : nullptr;
}
} // namespace

Entity UI::CreateWidget(const std::string& layout, i32 z) { return g_active != nullptr ? g_active->CreateWidget(layout, z) : kNullEntity; }

void UI::RemoveWidget(const Entity& target) {
    if (g_active != nullptr) g_active->RemoveWidget(target);
}

void UI::SetText(const Entity& target, const std::string& widget, const std::string& text) {
    ui::Widget* w = Target(target, widget);
    if (auto* t = dynamic_cast<ui::Text*>(w)) {
        t->text = text;
    } else if (auto* ti = dynamic_cast<ui::TextInput*>(w)) {
        ti->SetText(text);
    } else if (w != nullptr) {
        // A button's (or a panel's) first Text inside it.
        for (usize i = 0; i < w->ChildCount(); ++i) {
            if (auto* inner = dynamic_cast<ui::Text*>(w->Child(i))) {
                inner->text = text;
                return;
            }
        }
    }
}

std::string UI::GetText(const Entity& target, const std::string& widget) {
    ui::Widget* w = Target(target, widget);
    if (auto* t = dynamic_cast<ui::Text*>(w)) return t->text;
    if (auto* ti = dynamic_cast<ui::TextInput*>(w)) return ti->text;
    if (auto* d = dynamic_cast<ui::Dropdown*>(w); d != nullptr && d->selected >= 0 && d->selected < static_cast<i32>(d->options.size())) {
        return d->options[static_cast<usize>(d->selected)];
    }
    return {};
}

void UI::SetVisible(const Entity& target, const std::string& widget, bool visible) {
    if (ui::Widget* w = Target(target, widget)) w->visibility = visible ? ui::Visibility::Visible : ui::Visibility::Collapsed;
}

void UI::SetEnabled(const Entity& target, const std::string& widget, bool enabled) {
    if (ui::Widget* w = Target(target, widget)) w->enabled = enabled;
}

void UI::SetValue(const Entity& target, const std::string& widget, f32 value) {
    ui::Widget* w = Target(target, widget);
    if (auto* s = dynamic_cast<ui::Slider*>(w)) s->value = std::clamp(value, std::min(s->min, s->max), std::max(s->min, s->max));
    else if (auto* p = dynamic_cast<ui::ProgressBar*>(w)) p->percent = value;
    else if (auto* t = dynamic_cast<ui::Toggle*>(w)) t->checked = value != 0.0f;
    else if (auto* d = dynamic_cast<ui::Dropdown*>(w)) d->selected = static_cast<i32>(value);
}

f32 UI::GetValue(const Entity& target, const std::string& widget) {
    ui::Widget* w = Target(target, widget);
    if (auto* s = dynamic_cast<ui::Slider*>(w)) return s->value;
    if (auto* p = dynamic_cast<ui::ProgressBar*>(w)) return p->percent;
    if (auto* t = dynamic_cast<ui::Toggle*>(w)) return t->checked ? 1.0f : 0.0f;
    if (auto* d = dynamic_cast<ui::Dropdown*>(w)) return static_cast<f32>(d->selected);
    return 0.0f;
}

void UI::PlayAnimation(const Entity& target, const std::string& animation) {
    if (ui::UIAnimator* a = AnimatorFor(target)) a->Play(animation);
}

void UI::StopAnimation(const Entity& target, const std::string& animation) {
    if (ui::UIAnimator* a = AnimatorFor(target)) a->Stop(animation);
}

void UI::SetFocus(const Entity& target, const std::string& widget) {
    if (g_active != nullptr) g_active->FocusWidget(target, widget);
}

} // namespace aether

namespace aether::ui {

void RegisterWidgetComponents() { (void)GetComponentId<WidgetComponent>(); }

UISystem* UISystem::Active() { return g_active; }
void UISystem::MakeActive() { g_active = this; }

UISystem::UISystem(World& world, Viewport& viewport, UIInputRouter& router, LayoutLoader loader, const Theme* theme)
    : world_(world), viewport_(viewport), router_(router), loader_(std::move(loader)), theme_(theme) {
    RegisterWidgetComponents();
    MakeActive();
}

UISystem::~UISystem() {
    for (auto& [index, in] : instances_) Destroy(*in);
    if (g_active == this) g_active = nullptr;
}

const char* UISystem::BlueprintEventName(WidgetEvent::Kind kind) {
    switch (kind) {
    case WidgetEvent::Kind::Clicked: return "Event.OnWidgetClicked";
    case WidgetEvent::Kind::ValueChanged: return "Event.OnWidgetValueChanged";
    case WidgetEvent::Kind::CheckChanged: return "Event.OnWidgetCheckChanged";
    case WidgetEvent::Kind::TextCommitted: return "Event.OnWidgetTextCommitted";
    case WidgetEvent::Kind::SelectionChanged: return "Event.OnWidgetSelectionChanged";
    case WidgetEvent::Kind::AnimationFinished: return "Event.OnWidgetAnimationFinished";
    }
    return "";
}

UISystem::Instance* UISystem::Find(Entity e) {
    const auto it = instances_.find(e.index);
    return it == instances_.end() || it->second->entity != e ? nullptr : it->second.get();
}
const UISystem::Instance* UISystem::Find(Entity e) const { return const_cast<UISystem*>(this)->Find(e); }

Widget* UISystem::RootOf(Entity e) const {
    const Instance* in = Find(e);
    return in != nullptr ? in->root : nullptr;
}

Widget* UISystem::FindWidget(Entity e, const std::string& widget) const {
    Widget* root = RootOf(e);
    return root != nullptr ? root->Find(widget) : nullptr;
}

UIAnimator* UISystem::AnimatorOf(Entity e) const {
    const Instance* in = Find(e);
    return in != nullptr ? in->animator.get() : nullptr;
}

DataBinder* UISystem::BinderOf(Entity e) const {
    const Instance* in = Find(e);
    return in != nullptr ? in->binder.get() : nullptr;
}

void UISystem::AddGlobalSource(const std::string& name, void* object, const reflect::TypeInfo& type) { globals_[name] = {object, &type}; }

Entity UISystem::CreateWidget(const std::string& layout, i32 z) {
    WidgetComponent w;
    w.layout = layout;
    w.z = z;
    const Entity e = world_.CreateEntity(w);
    // Shown at once, so the caller can change it straight away (without an
    // Update: this may run from a Blueprint while the game dispatches events).
    if (Instance* in = Ensure(e, w)) {
        in->seen = generation_;
        if (in->root != nullptr) {
            Refresh(*in);
            in->binder->Update();
        }
    }
    viewport_.Layout(router_.GetFont());
    return e;
}

void UISystem::RemoveWidget(Entity e) {
    if (Instance* in = Find(e)) {
        Destroy(*in);
        instances_.erase(e.index);
    }
    if (world_.IsAlive(e) && world_.GetComponent<WidgetComponent>(e) != nullptr) world_.RemoveComponent<WidgetComponent>(e);
    viewport_.Layout(router_.GetFont());
}

bool UISystem::FocusWidget(Entity e, const std::string& widget) {
    Widget* w = FindWidget(e, widget);
    if (w == nullptr || !w->Focusable()) return false;
    router_.SetFocus(w);
    return true;
}

void detail::HookWidgetEvents(Widget& w, Entity e, std::vector<WidgetEvent>& sink) {
    // Controls raise events for the entity's Blueprint (after whatever they did before).
    const std::string name = w.name;
    std::vector<WidgetEvent>* out = &sink;
    auto push = [out, e, name](WidgetEvent::Kind kind, f32 value, const std::string& text) { out->push_back({e, kind, name, value, text}); };
    if (auto* b = dynamic_cast<Button*>(&w)) {
        auto prev = b->on_clicked;
        b->on_clicked = [push, prev] {
            if (prev) prev();
            push(WidgetEvent::Kind::Clicked, 0.0f, {});
        };
    } else if (auto* s = dynamic_cast<Slider*>(&w)) {
        auto prev = s->on_changed;
        s->on_changed = [push, prev](f32 v) {
            if (prev) prev(v);
            push(WidgetEvent::Kind::ValueChanged, v, {});
        };
    } else if (auto* t = dynamic_cast<Toggle*>(&w)) {
        auto prev = t->on_changed;
        t->on_changed = [push, prev](bool v) {
            if (prev) prev(v);
            push(WidgetEvent::Kind::CheckChanged, v ? 1.0f : 0.0f, {});
        };
    } else if (auto* ti = dynamic_cast<TextInput*>(&w)) {
        auto prev = ti->on_committed;
        ti->on_committed = [push, prev](const std::string& v) {
            if (prev) prev(v);
            push(WidgetEvent::Kind::TextCommitted, 0.0f, v);
        };
    } else if (auto* d = dynamic_cast<Dropdown*>(&w)) {
        auto prev = d->on_changed;
        d->on_changed = [push, prev](i32 v) {
            if (prev) prev(v);
            push(WidgetEvent::Kind::SelectionChanged, static_cast<f32>(v), {});
        };
    } else if (auto* l = dynamic_cast<ListView*>(&w)) {
        auto prev = l->on_selected;
        l->on_selected = [push, prev](i64 v) {
            if (prev) prev(v);
            push(WidgetEvent::Kind::SelectionChanged, static_cast<f32>(v), {});
        };
    }
    for (usize i = 0; i < w.ChildCount(); ++i) HookWidgetEvents(*w.Child(i), e, sink);
}

std::vector<std::string> detail::BindingSourceNames(const std::vector<Binding>& bindings) {
    std::vector<std::string> out;
    for (const Binding& b : bindings) {
        for (const std::string* path : {&b.source, &b.divide_by}) {
            const std::string head = path->substr(0, path->find('.'));
            if (!head.empty() && std::find(out.begin(), out.end(), head) == out.end()) out.push_back(head);
        }
    }
    return out;
}

void detail::RefreshBindingSources(World& world, Entity entity, const std::vector<std::string>& sources, const GlobalSources& globals, DataBinder& binder) {
    // Component storage moves as entities change archetype: point the sources at it again each frame.
    for (const std::string& name : sources) {
        if (const auto g = globals.find(name); g != globals.end()) {
            binder.AddSource(name, g->second.first, *g->second.second);
            continue;
        }
        const ComponentId id = FindComponentIdByName(name);
        const reflect::TypeInfo* type = reflect::TypeRegistry::Find(name);
        void* data = id == kInvalidComponentId ? nullptr : world.GetComponentRaw(entity, id);
        if (data != nullptr && type != nullptr) binder.AddSource(name, data, *type);
        else binder.RemoveSource(name);
    }
}

bool UISystem::Create(Instance& in) {
    LayoutDocument doc;
    std::string error;
    if (!loader_ || !loader_(in.layout, doc, &error) || !doc.root) {
        if (!reported_[in.layout]) {
            reported_[in.layout] = true;
            problems_.push_back("layout '" + in.layout + "': " + (error.empty() ? std::string("couldn't be loaded") : error));
        }
        return false;
    }
    if (theme_ != nullptr) ApplyTheme(*theme_, *doc.root);
    in.root = viewport_.Add(std::move(doc.root), in.z);
    in.animator = std::make_unique<UIAnimator>(*in.root);
    for (UIAnimation& a : doc.animations) {
        for (const std::string& p : in.animator->Validate(a)) problems_.push_back("layout '" + in.layout + "': " + p);
        in.animator->Add(std::move(a));
    }
    in.binder = std::make_unique<DataBinder>();
    in.bindings = doc.bindings;
    in.sources = detail::BindingSourceNames(in.bindings);
    Refresh(in);
    for (const std::string& p : in.binder->Bind(*in.root, in.bindings)) problems_.push_back("layout '" + in.layout + "': " + p);
    detail::HookWidgetEvents(*in.root, in.entity, pending_);
    return true;
}

void UISystem::Refresh(Instance& in) { detail::RefreshBindingSources(world_, in.entity, in.sources, globals_, *in.binder); }

void UISystem::Destroy(Instance& in) {
    if (in.root != nullptr) {
        in.animator.reset();
        viewport_.Remove(in.root);
        in.root = nullptr;
    }
}

UISystem::Instance* UISystem::Ensure(Entity e, const WidgetComponent& wc) {
    auto& slot = instances_[e.index];
    if (slot && (slot->entity != e || slot->layout != wc.layout || slot->z != wc.z)) {
        Destroy(*slot); // a recycled entity, or a new layout or layer
        slot.reset();
    }
    if (!slot) {
        slot = std::make_unique<Instance>();
        slot->entity = e;
        slot->layout = wc.layout;
        slot->z = wc.z;
        Create(*slot);
    }
    return slot.get();
}

void UISystem::Update(f32 dt) {
    const u64 now = ++generation_;
    events_ = std::move(pending_); // what controls raised since the last Update
    pending_.clear();
    std::vector<Entity> entities;
    world_.ForEachArchetype([&](Archetype& archetype) {
        if (!archetype.Mask().test(GetComponentId<WidgetComponent>())) return;
        for (usize c = 0; c < archetype.ChunkCount(); ++c) {
            Entity* e = archetype.EntityArray(c);
            entities.insert(entities.end(), e, e + archetype.ChunkEntityCount(c));
        }
    });
    for (Entity e : entities) {
        const WidgetComponent& wc = *world_.GetComponent<WidgetComponent>(e);
        Instance& in = *Ensure(e, wc);
        in.seen = now;
        if (in.root == nullptr) continue;
        in.root->visibility = wc.visible ? Visibility::Visible : Visibility::Collapsed;
        Refresh(in);
        in.binder->Update();
        in.animator->Tick(dt);
        for (const std::string& name : in.animator->Finished()) events_.push_back({e, WidgetEvent::Kind::AnimationFinished, name, 0.0f, {}});
    }
    for (auto it = instances_.begin(); it != instances_.end();) {
        if (it->second->seen == now) {
            ++it;
        } else {
            Destroy(*it->second);
            it = instances_.erase(it);
        }
    }
    viewport_.Layout(router_.GetFont());
}

} // namespace aether::ui
