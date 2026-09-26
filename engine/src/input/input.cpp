#include "aether/input/actions.h"

#include <algorithm>
#include <bitset>
#include <cmath>
#include <unordered_set>

namespace aether::input {

const char* KeyName(Key key) {
    const char* name = reflect::Reflect<Key>().EnumName(static_cast<i64>(key));
    return name != nullptr ? name : "None";
}

Key KeyFromName(std::string_view name) {
    i64 value = 0;
    return reflect::Reflect<Key>().EnumValueOf(name, value) ? static_cast<Key>(value) : Key::None;
}

namespace {

f32 DeadZone(f32 c, f32 lower, f32 upper) {
    const f32 magnitude = std::fabs(c);
    if (magnitude < lower || upper <= lower) {
        return magnitude < lower ? 0.0f : c;
    }
    const f32 scaled = std::min(1.0f, (magnitude - lower) / (upper - lower));
    return c < 0.0f ? -scaled : scaled;
}

} // namespace

Vec3 ApplyModifiers(f32 raw, const std::vector<InputModifier>& modifiers) {
    Vec3 v{raw, 0.0f, 0.0f};
    for (const InputModifier& m : modifiers) {
        switch (m.type) {
        case ModifierType::DeadZone:
            v = Vec3{DeadZone(v.x, m.lower, m.upper), DeadZone(v.y, m.lower, m.upper), DeadZone(v.z, m.lower, m.upper)};
            break;
        case ModifierType::Negate:
            v = Vec3{m.negate_x ? -v.x : v.x, m.negate_y ? -v.y : v.y, m.negate_z ? -v.z : v.z};
            break;
        case ModifierType::Swizzle:
            switch (m.order) {
            case SwizzleOrder::YXZ: v = Vec3{v.y, v.x, v.z}; break;
            case SwizzleOrder::ZYX: v = Vec3{v.z, v.y, v.x}; break;
            case SwizzleOrder::XZY: v = Vec3{v.x, v.z, v.y}; break;
            case SwizzleOrder::YZX: v = Vec3{v.y, v.z, v.x}; break;
            case SwizzleOrder::ZXY: v = Vec3{v.z, v.x, v.y}; break;
            }
            break;
        case ModifierType::Scale:
            v = Vec3{v.x * m.scale.x, v.y * m.scale.y, v.z * m.scale.z};
            break;
        }
    }
    return v;
}

void InputSystem::AddAction(InputAction action) {
    for (InputAction& existing : actions_) {
        if (existing.name == action.name) {
            existing = std::move(action);
            return;
        }
    }
    actions_.push_back(std::move(action));
}

const InputAction* InputSystem::FindAction(const std::string& name) const {
    for (const InputAction& action : actions_) {
        if (action.name == name) {
            return &action;
        }
    }
    return nullptr;
}

void InputSystem::AddContext(InputMappingContext context, i32 priority) {
    RemoveContext(context.name);
    ContextEntry entry{std::move(context), priority, next_order_++, {}};
    entry.states.resize(entry.context.bindings.size());
    contexts_.push_back(std::move(entry));
    std::stable_sort(contexts_.begin(), contexts_.end(), [](const ContextEntry& a, const ContextEntry& b) {
        return a.priority != b.priority ? a.priority > b.priority : a.order < b.order;
    });
}

bool InputSystem::RemoveContext(const std::string& name) {
    auto it = std::find_if(contexts_.begin(), contexts_.end(), [&](const ContextEntry& e) { return e.context.name == name; });
    if (it == contexts_.end()) {
        return false;
    }
    contexts_.erase(it);
    return true;
}

bool InputSystem::HasContext(const std::string& name) const {
    return std::any_of(contexts_.begin(), contexts_.end(), [&](const ContextEntry& e) { return e.context.name == name; });
}

void InputSystem::Reset() {
    for (ContextEntry& entry : contexts_) {
        std::fill(entry.states.begin(), entry.states.end(), BindingState{});
    }
    states_.clear();
}

ActionPhase InputSystem::EvaluateBinding(const InputBinding& binding, BindingState& state, bool actuated_now, f32 dt,
                                         const std::function<bool(const std::string&)>& chord_triggered) {
    const bool was = state.actuated;
    const f32 held_before = state.held;
    state.since_press += dt;
    if (actuated_now) {
        state.held = was ? state.held + dt : 0.0f;
    }
    const bool pressed = actuated_now && !was;

    bool all_triggered = !binding.triggers.empty();
    bool any_active = false;
    for (const InputTrigger& trigger : binding.triggers) {
        ActionPhase result = ActionPhase::None;
        switch (trigger.type) {
        case TriggerType::Down:
            result = actuated_now ? ActionPhase::Triggered : ActionPhase::None;
            break;
        case TriggerType::Pressed:
            result = pressed ? ActionPhase::Triggered : ActionPhase::None;
            break;
        case TriggerType::Released:
            result = actuated_now ? ActionPhase::Ongoing : (was ? ActionPhase::Triggered : ActionPhase::None);
            break;
        case TriggerType::Hold:
            if (!actuated_now) {
                result = ActionPhase::None;
            } else if (state.held < trigger.time) {
                result = ActionPhase::Ongoing;
            } else if (trigger.one_shot && state.hold_fired) {
                result = ActionPhase::None; // fired once: completes while still held
            } else {
                result = ActionPhase::Triggered;
                state.hold_fired = true;
            }
            break;
        case TriggerType::Tap:
            if (actuated_now) {
                result = state.held < trigger.time ? ActionPhase::Ongoing : ActionPhase::None;
            } else {
                result = was && held_before < trigger.time ? ActionPhase::Triggered : ActionPhase::None;
            }
            break;
        case TriggerType::DoubleTap:
            if (state.tap_pending && state.since_press > trigger.time) {
                state.tap_pending = false;
            }
            if (pressed) {
                if (state.tap_pending) {
                    result = ActionPhase::Triggered;
                    state.tap_pending = false;
                } else {
                    state.tap_pending = true;
                }
                state.since_press = 0.0f;
            }
            break;
        case TriggerType::Chord:
            result = actuated_now && chord_triggered(trigger.chord_action) ? ActionPhase::Triggered : ActionPhase::None;
            break;
        }
        all_triggered = all_triggered && result == ActionPhase::Triggered;
        any_active = any_active || result != ActionPhase::None;
    }
    if (!actuated_now) {
        state.hold_fired = false;
    }
    state.actuated = actuated_now;
    if (binding.triggers.empty()) {
        return actuated_now ? ActionPhase::Triggered : ActionPhase::None;
    }
    return all_triggered ? ActionPhase::Triggered : (any_active ? ActionPhase::Ongoing : ActionPhase::None);
}

void InputSystem::Update(const InputState& input, f32 dt) {
    const std::unordered_map<std::string, ActionState> previous = states_;
    std::unordered_map<std::string, ActionState> current;
    std::unordered_set<std::string> evaluated;
    for (const InputAction& action : actions_) {
        current[action.name] = ActionState{};
    }
    // A chord sees the action as evaluated so far this frame, or last frame's
    // state if it hasn't been evaluated yet (it's in a lower context).
    auto chord_triggered = [&](const std::string& action) {
        if (evaluated.count(action) != 0) {
            return current[action].phase == ActionPhase::Triggered;
        }
        auto it = previous.find(action);
        return it != previous.end() && it->second.phase == ActionPhase::Triggered;
    };
    auto has_chord = [](const InputBinding& b) {
        return std::any_of(b.triggers.begin(), b.triggers.end(),
                           [](const InputTrigger& t) { return t.type == TriggerType::Chord; });
    };

    std::bitset<kKeyCount> consumed;
    bool blocked = false;
    for (ContextEntry& entry : contexts_) {
        if (entry.states.size() != entry.context.bindings.size()) {
            entry.states.resize(entry.context.bindings.size());
        }
        std::bitset<kKeyCount> used_here;
        for (int pass = 0; pass < 2; ++pass) { // bindings without chords first
            for (usize i = 0; i < entry.context.bindings.size(); ++i) {
                const InputBinding& binding = entry.context.bindings[i];
                if (has_chord(binding) != (pass == 1)) {
                    continue;
                }
                BindingState& state = entry.states[i];
                const usize key = static_cast<usize>(binding.key);
                if (blocked || binding.key == Key::None || key >= kKeyCount || consumed.test(key)) {
                    state = BindingState{}; // no input reaches it: quietly reset
                    continue;
                }
                const Vec3 value = ApplyModifiers(input.Value(binding.key), binding.modifiers);
                const f32 magnitude = value.Length();
                const bool actuated =
                    binding.triggers.empty() ? magnitude > 0.0f : magnitude >= std::max(binding.actuation, 1e-6f);
                const ActionPhase phase = EvaluateBinding(binding, state, actuated, dt, chord_triggered);
                ActionState& action = current[binding.action];
                evaluated.insert(binding.action);
                if (phase > action.phase) {
                    action.phase = phase;
                }
                if (actuated) {
                    action.value = action.value + value;
                    used_here.set(key);
                }
            }
        }
        if (entry.context.consume_input) {
            consumed |= used_here;
        }
        blocked = blocked || entry.context.block_lower_contexts;
    }

    // Phase transitions -> events, in action order (declared ones first).
    std::vector<std::string> order;
    for (const InputAction& action : actions_) {
        order.push_back(action.name);
    }
    std::vector<std::string> extra;
    for (const auto& [name, state] : current) {
        if (FindAction(name) == nullptr) {
            extra.push_back(name);
        }
    }
    for (const auto& [name, state] : previous) {
        if (FindAction(name) == nullptr && current.count(name) == 0) {
            extra.push_back(name);
            current[name] = ActionState{};
        }
    }
    std::sort(extra.begin(), extra.end());
    order.insert(order.end(), extra.begin(), extra.end());

    for (const std::string& name : order) {
        ActionState& now = current[name];
        auto it = previous.find(name);
        const ActionState before = it != previous.end() ? it->second : ActionState{};
        if (now.phase == ActionPhase::Triggered) {
            now.triggered_time = before.phase == ActionPhase::Triggered ? before.triggered_time + dt : 0.0f;
        }
        std::vector<ActionEvent> events;
        if (before.phase == ActionPhase::None && now.phase != ActionPhase::None) {
            events.push_back(ActionEvent::Started);
        }
        if (now.phase == ActionPhase::Ongoing) {
            events.push_back(ActionEvent::Ongoing);
        } else if (now.phase == ActionPhase::Triggered) {
            events.push_back(ActionEvent::Triggered);
        } else if (before.phase == ActionPhase::Triggered) {
            events.push_back(ActionEvent::Completed);
        } else if (before.phase == ActionPhase::Ongoing) {
            events.push_back(ActionEvent::Canceled);
        }
        states_[name] = now;
        for (ActionEvent event : events) {
            for (const Subscription& sub : std::vector<Subscription>(subscriptions_)) {
                if ((sub.action.empty() || sub.action == name) && sub.event == event) {
                    sub.handler(name, event, now);
                }
            }
        }
    }
    // Actions no longer produced by anything are dropped once they're None.
    for (auto it = states_.begin(); it != states_.end();) {
        if (it->second.phase == ActionPhase::None && FindAction(it->first) == nullptr) {
            it = states_.erase(it);
        } else {
            ++it;
        }
    }
}

const ActionState& InputSystem::GetAction(const std::string& name) const {
    static const ActionState kNone;
    auto it = states_.find(name);
    return it != states_.end() ? it->second : kNone;
}

u32 InputSystem::Subscribe(const std::string& action, ActionEvent event, Handler handler) {
    const u32 id = next_subscription_++;
    subscriptions_.push_back({id, action, event, std::move(handler)});
    return id;
}

void InputSystem::Unsubscribe(u32 id) {
    subscriptions_.erase(std::remove_if(subscriptions_.begin(), subscriptions_.end(),
                                        [&](const Subscription& s) { return s.id == id; }),
                         subscriptions_.end());
}

} // namespace aether::input
