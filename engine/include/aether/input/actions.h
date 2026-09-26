#pragma once

#include "aether/input/keys.h"
#include "aether/math/math.h"

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace aether::input {

// Action mapping (Phase 10, docs/ROADMAP.md): gameplay asks for "Jump" or
// "Move", not for keys. Mapping contexts bind keys to actions, shaped by
// modifiers and gated by triggers; contexts stack by priority.

enum class ActionValueType { Bool, Axis1D, Axis2D, Axis3D };

// What an action is. Referred to by name from bindings.
struct InputAction {
    std::string name;
    ActionValueType value_type = ActionValueType::Bool;
};

// Shapes a binding's value (a Vec3; a key's own value starts in x).
enum class ModifierType {
    DeadZone, // magnitudes below `lower` become 0; lower..upper is rescaled to 0..1
    Negate,   // flips the components selected by negate_x/y/z
    Swizzle,  // reorders components (e.g. a key's x into y, for W/S)
    Scale,    // multiplies by `scale`
};
enum class SwizzleOrder { YXZ, ZYX, XZY, YZX, ZXY };

struct InputModifier {
    ModifierType type = ModifierType::DeadZone;
    f32 lower = 0.2f;
    f32 upper = 1.0f;
    bool negate_x = true;
    bool negate_y = true;
    bool negate_z = true;
    SwizzleOrder order = SwizzleOrder::YXZ;
    Vec3 scale{1.0f, 1.0f, 1.0f};
};

// When a binding counts. A binding with no triggers counts whenever its
// value (after modifiers, e.g. a dead zone) is non-zero, which suits sticks
// and mouse movement. With triggers, it's actuated at `actuation` magnitude
// and all its triggers must fire at once.
enum class TriggerType {
    Down,      // every frame while actuated
    Pressed,   // the frame it becomes actuated
    Released,  // the frame it stops being actuated (ongoing while held)
    Hold,      // after being held `time` seconds (once, unless !one_shot)
    Tap,       // released within `time` seconds of being pressed
    DoubleTap, // pressed a second time within `time` seconds of the first press
    Chord,     // while actuated and `chord_action` is triggered (e.g. Shift for Sprint)
};

struct InputTrigger {
    TriggerType type = TriggerType::Down;
    f32 time = 0.5f;      // Hold / Tap / DoubleTap
    bool one_shot = true; // Hold: fire once, or every frame after `time`
    std::string chord_action;
};

struct InputBinding {
    std::string action;
    Key key = Key::None;
    f32 actuation = 0.5f; // magnitude at which it counts as pressed, for its triggers
    std::vector<InputModifier> modifiers;
    std::vector<InputTrigger> triggers;
};

struct InputMappingContext {
    std::string name;
    std::vector<InputBinding> bindings;
    // A key this context uses (while actuated) isn't seen by lower-priority
    // contexts, so the same key can mean different things in each.
    bool consume_input = true;
    // Lower-priority contexts get no input at all while this one is active
    // (e.g. a UI context over gameplay).
    bool block_lower_contexts = false;
};

// An action's state this frame.
enum class ActionPhase { None, Ongoing, Triggered };
enum class ActionEvent {
    Started,   // left None (the first frame of pressing, for most triggers)
    Ongoing,   // being evaluated (e.g. a Hold not yet long enough)
    Triggered, // fired this frame
    Completed, // stopped after having triggered
    Canceled,  // stopped without triggering (e.g. a Tap held too long)
};

struct Axis2 {
    f32 x = 0.0f;
    f32 y = 0.0f;
};

struct ActionState {
    ActionPhase phase = ActionPhase::None;
    Vec3 value{0.0f, 0.0f, 0.0f}; // sum of the values of its bindings that are actuated
    f32 triggered_time = 0.0f;    // seconds it's been continuously triggered
};

// Evaluates contexts against device state once per frame and raises action
// events. Typical frame: platform feeds InputState -> system.Update(state,
// dt) -> gameplay reads actions / handles events -> state.EndFrame().
class InputSystem {
public:
    void AddAction(InputAction action);
    const InputAction* FindAction(const std::string& name) const;

    // Higher priority wins; equal priorities keep insertion order (first on
    // top). Adding a context with the same name replaces it.
    void AddContext(InputMappingContext context, i32 priority = 0);
    bool RemoveContext(const std::string& name);
    bool HasContext(const std::string& name) const;

    void Update(const InputState& state, f32 dt);

    const ActionState& GetAction(const std::string& name) const;
    bool IsTriggered(const std::string& name) const { return GetAction(name).phase == ActionPhase::Triggered; }
    // Bool actions: triggered this frame. Axis actions: their value.
    bool GetBool(const std::string& name) const { return IsTriggered(name); }
    f32 GetAxis1D(const std::string& name) const { return GetAction(name).value.x; }
    Axis2 GetAxis2D(const std::string& name) const {
        const Vec3& v = GetAction(name).value;
        return Axis2{v.x, v.y};
    }
    Vec3 GetAxis3D(const std::string& name) const { return GetAction(name).value; }

    // Called during Update for each event, in the order actions were added.
    using Handler = std::function<void(const std::string& action, ActionEvent event, const ActionState& state)>;
    // Returns an id for Unsubscribe. An empty action name gets every action.
    u32 Subscribe(const std::string& action, ActionEvent event, Handler handler);
    void Unsubscribe(u32 id);

    // Forgets per-binding timing state (e.g. when gameplay is paused).
    void Reset();

private:
    struct BindingState {
        bool actuated = false;
        f32 held = 0.0f;           // seconds actuated
        bool hold_fired = false;
        f32 since_press = 1e9f;    // for DoubleTap: seconds since the last press
        bool tap_pending = false;  // DoubleTap: a first press is waiting for a second
    };
    struct ContextEntry {
        InputMappingContext context;
        i32 priority;
        u64 order;
        std::vector<BindingState> states;
    };
    struct Subscription {
        u32 id;
        std::string action;
        ActionEvent event;
        Handler handler;
    };

    ActionPhase EvaluateBinding(const InputBinding& binding, BindingState& state, bool actuated_now, f32 dt,
                                const std::function<bool(const std::string&)>& chord_triggered);

    std::vector<InputAction> actions_;
    std::vector<ContextEntry> contexts_; // sorted, highest priority first
    u64 next_order_ = 0;
    std::unordered_map<std::string, ActionState> states_;
    std::vector<Subscription> subscriptions_;
    u32 next_subscription_ = 1;
};

// Applies a binding's modifiers to a raw key value.
Vec3 ApplyModifiers(f32 raw, const std::vector<InputModifier>& modifiers);

} // namespace aether::input

AETHER_ENUM(aether::input::ActionValueType, 1, AETHER_ENUM_VALUE(Bool), AETHER_ENUM_VALUE(Axis1D),
            AETHER_ENUM_VALUE(Axis2D), AETHER_ENUM_VALUE(Axis3D))
AETHER_ENUM(aether::input::ModifierType, 1, AETHER_ENUM_VALUE(DeadZone), AETHER_ENUM_VALUE(Negate),
            AETHER_ENUM_VALUE(Swizzle), AETHER_ENUM_VALUE(Scale))
AETHER_ENUM(aether::input::SwizzleOrder, 1, AETHER_ENUM_VALUE(YXZ), AETHER_ENUM_VALUE(ZYX), AETHER_ENUM_VALUE(XZY),
            AETHER_ENUM_VALUE(YZX), AETHER_ENUM_VALUE(ZXY))
AETHER_ENUM(aether::input::TriggerType, 1, AETHER_ENUM_VALUE(Down), AETHER_ENUM_VALUE(Pressed),
            AETHER_ENUM_VALUE(Released), AETHER_ENUM_VALUE(Hold), AETHER_ENUM_VALUE(Tap), AETHER_ENUM_VALUE(DoubleTap),
            AETHER_ENUM_VALUE(Chord))

AETHER_REFLECT(aether::input::InputAction, 1, AETHER_FIELD(name, Field_EditAnywhere), AETHER_FIELD(value_type, Field_EditAnywhere))
AETHER_REFLECT(aether::input::InputModifier, 1,
    AETHER_FIELD(type, Field_EditAnywhere), AETHER_FIELD(lower, Field_EditAnywhere), AETHER_FIELD(upper, Field_EditAnywhere),
    AETHER_FIELD(negate_x, Field_EditAnywhere), AETHER_FIELD(negate_y, Field_EditAnywhere), AETHER_FIELD(negate_z, Field_EditAnywhere),
    AETHER_FIELD(order, Field_EditAnywhere), AETHER_FIELD(scale, Field_EditAnywhere))
AETHER_REFLECT(aether::input::InputTrigger, 1,
    AETHER_FIELD(type, Field_EditAnywhere), AETHER_FIELD(time, Field_EditAnywhere, {.units = "s"}),
    AETHER_FIELD(one_shot, Field_EditAnywhere), AETHER_FIELD(chord_action, Field_EditAnywhere))
AETHER_REFLECT(aether::input::InputBinding, 1,
    AETHER_FIELD(action, Field_EditAnywhere), AETHER_FIELD(key, Field_EditAnywhere), AETHER_FIELD(actuation, Field_EditAnywhere),
    AETHER_FIELD(modifiers, Field_EditAnywhere), AETHER_FIELD(triggers, Field_EditAnywhere))
AETHER_REFLECT(aether::input::InputMappingContext, 1,
    AETHER_FIELD(name, Field_EditAnywhere), AETHER_FIELD(bindings, Field_EditAnywhere),
    AETHER_FIELD(consume_input, Field_EditAnywhere), AETHER_FIELD(block_lower_contexts, Field_EditAnywhere))
