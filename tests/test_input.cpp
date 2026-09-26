#include "aether/input/actions.h"
#include "aether/reflection/serialize.h"
#include "test_framework.h"

#include <cmath>
#include <map>

using namespace aether;
using namespace aether::input;

namespace {

constexpr f32 kDt = 0.1f;

bool Near(f32 a, f32 b) { return std::fabs(a - b) < 1e-4f; }

InputModifier Modifier(ModifierType type) {
    InputModifier m;
    m.type = type;
    return m;
}

InputBinding Bind(const std::string& action, Key key, std::vector<InputTrigger> triggers = {},
                  std::vector<InputModifier> modifiers = {}) {
    InputBinding b;
    b.action = action;
    b.key = key;
    b.triggers = std::move(triggers);
    b.modifiers = std::move(modifiers);
    return b;
}

InputTrigger Trigger(TriggerType type, f32 time = 0.5f) {
    InputTrigger t;
    t.type = type;
    t.time = time;
    return t;
}

// Drives one action through frames of synthetic input and records the events.
struct Harness {
    InputSystem system;
    InputState state;
    std::vector<std::string> events;

    Harness() {
        system.Subscribe("", ActionEvent::Started, Log("Started"));
        system.Subscribe("", ActionEvent::Triggered, Log("Triggered"));
        system.Subscribe("", ActionEvent::Completed, Log("Completed"));
        system.Subscribe("", ActionEvent::Canceled, Log("Canceled"));
    }
    InputSystem::Handler Log(const char* what) {
        return [this, what](const std::string& action, ActionEvent, const ActionState&) {
            events.push_back(action + ":" + what);
        };
    }
    // Holds `key` down (or not) for `frames` frames; returns the events seen.
    std::vector<std::string> Frames(Key key, bool down, int frames) {
        events.clear();
        for (int i = 0; i < frames; ++i) {
            state.SetButton(key, down);
            system.Update(state, kDt);
            state.EndFrame();
        }
        return events;
    }
};

using Events = std::vector<std::string>;

} // namespace

AETHER_TEST(Input_KeysAndDeviceState) {
    AETHER_CHECK(std::string(KeyName(Key::W)) == "W" && std::string(KeyName(Key::GamepadLeftStickX)) == "GamepadLeftStickX");
    AETHER_CHECK(KeyFromName("MouseWheel") == Key::MouseWheel && KeyFromName("Nope") == Key::None);
    AETHER_CHECK(IsAxis(Key::MouseX) && IsAxis(Key::GamepadRightTrigger) && !IsAxis(Key::Space) && IsDelta(Key::MouseY));

    InputState state;
    state.SetButton(Key::Space, true);
    state.SetAxis(Key::GamepadLeftStickX, -0.7f);
    state.AddMouseDelta(3.0f, -2.0f);
    state.AddMouseDelta(1.0f, 0.0f);
    state.AddWheel(1.0f);
    AETHER_CHECK(state.IsDown(Key::Space) && Near(state.Value(Key::MouseX), 4.0f) && Near(state.Value(Key::MouseY), -2.0f));
    state.EndFrame(); // deltas reset; held buttons and stick positions don't
    AETHER_CHECK(state.Value(Key::MouseX) == 0.0f && state.Value(Key::MouseWheel) == 0.0f);
    AETHER_CHECK(state.IsDown(Key::Space) && Near(state.Value(Key::GamepadLeftStickX), -0.7f));
    state.Clear();
    AETHER_CHECK(!state.IsDown(Key::Space));
}

AETHER_TEST(Input_ModifiersBuildAxes) {
    // WASD into a 2D Move action, the Enhanced Input way.
    InputModifier swizzle = Modifier(ModifierType::Swizzle);
    InputModifier negate = Modifier(ModifierType::Negate);
    InputMappingContext context;
    context.name = "OnFoot";
    context.bindings = {Bind("Move", Key::W, {}, {swizzle}), Bind("Move", Key::S, {}, {swizzle, negate}),
                        Bind("Move", Key::D), Bind("Move", Key::A, {}, {negate})};
    InputModifier dead = Modifier(ModifierType::DeadZone);
    dead.lower = 0.2f;
    InputModifier scale = Modifier(ModifierType::Scale);
    scale.scale = Vec3{2.0f, 1.0f, 1.0f};
    context.bindings.push_back(Bind("Steer", Key::GamepadLeftStickX, {}, {dead, scale}));
    InputSystem system;
    system.AddAction({"Move", ActionValueType::Axis2D});
    system.AddAction({"Steer", ActionValueType::Axis1D});
    system.AddContext(context);

    InputState state;
    state.SetButton(Key::W, true);
    state.SetButton(Key::D, true);
    system.Update(state, kDt);
    AETHER_CHECK(Near(system.GetAxis2D("Move").x, 1.0f) && Near(system.GetAxis2D("Move").y, 1.0f));
    state.SetButton(Key::D, false);
    state.SetButton(Key::A, true);
    state.SetButton(Key::S, true); // W and S cancel out
    system.Update(state, kDt);
    AETHER_CHECK(Near(system.GetAxis2D("Move").x, -1.0f) && Near(system.GetAxis2D("Move").y, 0.0f));

    state.SetAxis(Key::GamepadLeftStickX, 0.1f); // inside the dead zone
    system.Update(state, kDt);
    AETHER_CHECK(system.GetAxis1D("Steer") == 0.0f && !system.IsTriggered("Steer"));
    state.SetAxis(Key::GamepadLeftStickX, -0.6f); // (0.6 - 0.2) / 0.8 = 0.5, scaled x2
    system.Update(state, kDt);
    AETHER_CHECK(Near(system.GetAxis1D("Steer"), -1.0f) && system.IsTriggered("Steer"));

    // Swizzle orders, on a raw x value.
    InputModifier zyx = swizzle;
    zyx.order = SwizzleOrder::ZYX;
    InputModifier negate_y = negate;
    negate_y.negate_x = false;
    negate_y.negate_z = false;
    const Vec3 v = ApplyModifiers(0.5f, {zyx});
    AETHER_CHECK(v.x == 0.0f && v.z == 0.5f);
    AETHER_CHECK(ApplyModifiers(1.0f, {swizzle, negate_y}).y == -1.0f);
}

AETHER_TEST(Input_PressReleaseAndHold) {
    Harness h;
    InputMappingContext context;
    context.name = "Test";
    context.bindings = {Bind("Fire", Key::MouseLeft, {Trigger(TriggerType::Pressed)}),
                        Bind("Throw", Key::G, {Trigger(TriggerType::Released)}),
                        Bind("Charge", Key::E, {Trigger(TriggerType::Hold, 0.5f)})};
    h.system.AddContext(context);

    // Pressed: triggers on the first frame only.
    AETHER_CHECK((h.Frames(Key::MouseLeft, true, 3) == Events{"Fire:Started", "Fire:Triggered", "Fire:Completed"}));
    AETHER_CHECK(h.Frames(Key::MouseLeft, false, 2).empty());

    // Released: ongoing while held, triggers when let go.
    AETHER_CHECK((h.Frames(Key::G, true, 3) == Events{"Throw:Started"}));
    AETHER_CHECK((h.Frames(Key::G, false, 2) == Events{"Throw:Triggered", "Throw:Completed"}));

    // Hold 0.5 s at 0.1 s frames: held time is 0 on the pressing frame, so it
    // fires on the 6th frame, once, and completes while still held.
    Events hold = h.Frames(Key::E, true, 5);
    AETHER_CHECK((hold == Events{"Charge:Started"}) && h.system.GetAction("Charge").phase == ActionPhase::Ongoing);
    AETHER_CHECK((h.Frames(Key::E, true, 1) == Events{"Charge:Triggered"}));
    AETHER_CHECK((h.Frames(Key::E, true, 3) == Events{"Charge:Completed"}));
    AETHER_CHECK(h.Frames(Key::E, false, 1).empty());
    // Let go early: canceled.
    AETHER_CHECK((h.Frames(Key::E, true, 3) == Events{"Charge:Started"}));
    AETHER_CHECK((h.Frames(Key::E, false, 1) == Events{"Charge:Canceled"}));

    // A repeating hold fires every frame after the threshold.
    context.bindings[2].triggers[0].one_shot = false;
    h.system.AddContext(context);
    h.Frames(Key::E, true, 6);
    // Triggered fires every frame an action is triggered (as with Down).
    AETHER_CHECK((h.Frames(Key::E, true, 3) == Events{"Charge:Triggered", "Charge:Triggered", "Charge:Triggered"}));
    AETHER_CHECK(h.system.IsTriggered("Charge") && Near(h.system.GetAction("Charge").triggered_time, 0.3f));
}

AETHER_TEST(Input_TapDoubleTapAndChord) {
    Harness h;
    InputMappingContext context;
    context.name = "Test";
    context.bindings = {Bind("Dodge", Key::Space, {Trigger(TriggerType::Tap, 0.3f)}),
                        Bind("Dash", Key::Q, {Trigger(TriggerType::DoubleTap, 0.3f)}),
                        Bind("Shift", Key::LeftShift)};
    InputTrigger chord = Trigger(TriggerType::Chord);
    chord.chord_action = "Shift";
    context.bindings.push_back(Bind("Sprint", Key::W, {chord}));
    h.system.AddContext(context);

    // Tap: a quick press-release triggers on release...
    AETHER_CHECK((h.Frames(Key::Space, true, 2) == Events{"Dodge:Started"}));
    AETHER_CHECK((h.Frames(Key::Space, false, 1) == Events{"Dodge:Triggered"}));
    h.Frames(Key::Space, false, 1);
    // ...held too long, it's canceled and doesn't fire on release.
    AETHER_CHECK((h.Frames(Key::Space, true, 5) == Events{"Dodge:Started", "Dodge:Canceled"}));
    AETHER_CHECK(h.Frames(Key::Space, false, 2).empty());

    // Double tap: a second press within 0.3 s.
    h.Frames(Key::Q, true, 1);
    h.Frames(Key::Q, false, 1);
    AETHER_CHECK((h.Frames(Key::Q, true, 1) == Events{"Dash:Started", "Dash:Triggered"}));
    AETHER_CHECK((h.Frames(Key::Q, false, 1) == Events{"Dash:Completed"}));
    // Too slow: no dash (the late press starts a new double tap instead).
    h.Frames(Key::Q, true, 1);
    h.Frames(Key::Q, false, 4);
    AETHER_CHECK(h.Frames(Key::Q, true, 1).empty() && !h.system.IsTriggered("Dash"));
    h.Frames(Key::Q, false, 1);
    AETHER_CHECK((h.Frames(Key::Q, true, 1) == Events{"Dash:Started", "Dash:Triggered"})); // ...which this completes

    // Chord: W sprints only while Shift is held.
    h.Frames(Key::W, true, 2);
    AETHER_CHECK(!h.system.IsTriggered("Sprint"));
    h.state.SetButton(Key::LeftShift, true);
    h.Frames(Key::W, true, 1);
    AETHER_CHECK(h.system.IsTriggered("Sprint") && h.system.IsTriggered("Shift"));
    h.state.SetButton(Key::LeftShift, false);
    h.Frames(Key::W, true, 1);
    AETHER_CHECK(!h.system.IsTriggered("Sprint"));
}

AETHER_TEST(Input_ContextStackConsumesAndBlocks) {
    Harness h;
    InputMappingContext on_foot;
    on_foot.name = "OnFoot";
    on_foot.bindings = {Bind("Jump", Key::Space, {Trigger(TriggerType::Pressed)}), Bind("Look", Key::MouseX)};
    InputMappingContext vehicle;
    vehicle.name = "Vehicle";
    vehicle.bindings = {Bind("Brake", Key::Space)};
    h.system.AddContext(on_foot, 0);
    h.system.AddContext(vehicle, 10);

    // Space means Brake in the vehicle: OnFoot doesn't see it.
    h.Frames(Key::Space, true, 1);
    AETHER_CHECK(h.system.IsTriggered("Brake") && !h.system.IsTriggered("Jump"));
    h.Frames(Key::Space, false, 1);
    h.system.RemoveContext("Vehicle");
    AETHER_CHECK(!h.system.HasContext("Vehicle") && !h.system.RemoveContext("Vehicle"));
    h.Frames(Key::Space, true, 1);
    AETHER_CHECK(h.system.IsTriggered("Jump"));
    h.Frames(Key::Space, false, 1);

    // A non-consuming context shares its keys.
    vehicle.consume_input = false;
    h.system.AddContext(vehicle, 10);
    h.Frames(Key::Space, true, 1);
    AETHER_CHECK(h.system.IsTriggered("Brake") && h.system.IsTriggered("Jump"));
    h.Frames(Key::Space, false, 1);
    h.system.RemoveContext("Vehicle");

    // A UI context blocks gameplay entirely; nothing fires spuriously when
    // it closes while a key is still held.
    InputMappingContext ui;
    ui.name = "Menu";
    ui.block_lower_contexts = true;
    ui.bindings = {Bind("Confirm", Key::Enter, {Trigger(TriggerType::Pressed)})};
    h.system.AddContext(ui, 100);
    h.state.AddMouseDelta(5.0f, 0.0f);
    Events blocked = h.Frames(Key::Space, true, 2);
    AETHER_CHECK(blocked.empty() && !h.system.IsTriggered("Look"));
    h.system.RemoveContext("Menu");
    // Space is still held: the Pressed trigger sees it as a new press now,
    // which is what a player expects after closing a menu with it held.
    AETHER_CHECK((h.Frames(Key::Space, true, 1) == Events{"Jump:Started", "Jump:Triggered"}));

    // Mouse movement is an axis: triggered whenever it moves.
    h.Frames(Key::Space, false, 1);
    h.state.AddMouseDelta(-3.0f, 0.0f);
    h.system.Update(h.state, kDt);
    AETHER_CHECK(h.system.IsTriggered("Look") && Near(h.system.GetAxis1D("Look"), -3.0f));
    h.state.EndFrame();
    h.system.Update(h.state, kDt);
    AETHER_CHECK(!h.system.IsTriggered("Look"));

    // Subscriptions: per action, and removable.
    int jumps = 0;
    const u32 id = h.system.Subscribe("Jump", ActionEvent::Triggered, [&](const std::string&, ActionEvent, const ActionState&) { ++jumps; });
    h.Frames(Key::Space, true, 1);
    h.Frames(Key::Space, false, 1);
    h.system.Unsubscribe(id);
    h.Frames(Key::Space, true, 1);
    AETHER_CHECK(jumps == 1);
    h.system.Reset();
    AETHER_CHECK(!h.system.IsTriggered("Jump"));
}

AETHER_TEST(Input_MappingContextsSaveAsJson) {
    InputMappingContext context;
    context.name = "OnFoot";
    InputModifier dead = Modifier(ModifierType::DeadZone);
    context.bindings = {Bind("Jump", Key::Space, {Trigger(TriggerType::Pressed)}),
                        Bind("Move", Key::GamepadLeftStickY, {}, {dead})};
    const std::string text = reflect::SaveJsonText(context);
    AETHER_CHECK(text.find("\"Space\"") != std::string::npos && text.find("\"Pressed\"") != std::string::npos);
    AETHER_CHECK(text.find("\"GamepadLeftStickY\"") != std::string::npos);
    InputMappingContext loaded;
    AETHER_CHECK(reflect::LoadJsonText(loaded, text));
    AETHER_CHECK(loaded.bindings.size() == 2 && loaded.bindings[0].key == Key::Space);
    AETHER_CHECK(loaded.bindings[0].triggers[0].type == TriggerType::Pressed && loaded.bindings[1].modifiers.size() == 1);
    AETHER_CHECK(reflect::SaveJsonText(loaded) == text);
}
