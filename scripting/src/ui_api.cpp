#include "aether/script/ui_api.h"

#include "aether/ui/widget_system.h"

namespace aether::script {

namespace {

bool NeedString(NativeCall& c, usize i, const char* what) {
    if (c.IsString(i)) return true;
    c.Fail(std::string("UI: ") + what + " must be a string");
    return false;
}

// The entity whose layout holds `widget`, or kNullEntity (also when no UI system is running).
Entity Owner(const std::string& widget) {
    const ui::UISystem* system = ui::UISystem::Active();
    return system != nullptr ? system->EntityShowing(widget) : kNullEntity;
}

} // namespace

void InstallUiApi(LuauHost& host) {
    const auto def = [&](const char* name, NativeFunction fn) { host.RegisterNative("UI", name, std::move(fn)); };

    def("SetText", [](NativeCall& c) {
        if (!NeedString(c, 0, "the widget name") || !NeedString(c, 1, "the text")) return;
        const Entity owner = Owner(c.String(0));
        if (!owner.IsNull()) UI::SetText(owner, c.String(0), c.String(1));
        c.Return(!owner.IsNull());
    });
    def("GetText", [](NativeCall& c) {
        if (!NeedString(c, 0, "the widget name")) return;
        const Entity owner = Owner(c.String(0));
        if (owner.IsNull()) return c.Return(ScriptValue{});
        c.Return(UI::GetText(owner, c.String(0)));
    });
    def("SetValue", [](NativeCall& c) {
        if (!NeedString(c, 0, "the widget name")) return;
        if (!c.IsNumber(1)) return c.Fail("UI: the value must be a number");
        const Entity owner = Owner(c.String(0));
        if (!owner.IsNull()) UI::SetValue(owner, c.String(0), static_cast<f32>(c.Number(1)));
        c.Return(!owner.IsNull());
    });
    def("GetValue", [](NativeCall& c) {
        if (!NeedString(c, 0, "the widget name")) return;
        const Entity owner = Owner(c.String(0));
        if (owner.IsNull()) return c.Return(ScriptValue{});
        c.Return(static_cast<f64>(UI::GetValue(owner, c.String(0))));
    });
    def("SetVisible", [](NativeCall& c) {
        if (!NeedString(c, 0, "the widget name")) return;
        if (!c.IsBool(1)) return c.Fail("UI: the visibility must be a boolean");
        const Entity owner = Owner(c.String(0));
        if (!owner.IsNull()) UI::SetVisible(owner, c.String(0), c.Bool(1));
        c.Return(!owner.IsNull());
    });
    def("SetEnabled", [](NativeCall& c) {
        if (!NeedString(c, 0, "the widget name")) return;
        if (!c.IsBool(1)) return c.Fail("UI: enabled must be a boolean");
        const Entity owner = Owner(c.String(0));
        if (!owner.IsNull()) UI::SetEnabled(owner, c.String(0), c.Bool(1));
        c.Return(!owner.IsNull());
    });
    def("WasClicked", [](NativeCall& c) {
        if (!NeedString(c, 0, "the widget name")) return;
        const ui::UISystem* system = ui::UISystem::Active();
        bool clicked = false;
        if (system != nullptr) {
            for (const ui::WidgetEvent& e : system->Events()) clicked = clicked || (e.kind == ui::WidgetEvent::Kind::Clicked && e.widget == c.String(0));
        }
        c.Return(clicked);
    });
    def("ValueChanged", [](NativeCall& c) {
        if (!NeedString(c, 0, "the widget name")) return;
        const ui::UISystem* system = ui::UISystem::Active();
        if (system != nullptr) {
            for (const ui::WidgetEvent& e : system->Events()) {
                const bool changed = e.kind == ui::WidgetEvent::Kind::ValueChanged || e.kind == ui::WidgetEvent::Kind::CheckChanged || e.kind == ui::WidgetEvent::Kind::SelectionChanged;
                if (changed && e.widget == c.String(0)) return c.Return(static_cast<f64>(e.value));
            }
        }
        c.Return(ScriptValue{});
    });
    def("Exists", [](NativeCall& c) {
        if (NeedString(c, 0, "the widget name")) c.Return(!Owner(c.String(0)).IsNull());
    });
}

} // namespace aether::script
