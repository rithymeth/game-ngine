#pragma once

#include "aether/core/base.h"
#include "aether/reflection/reflection.h"

#include <string>
#include <vector>

namespace aether::gas {

// One stat of an entity (Phase 30 step 2, §30.2): Health, Mana, Stamina,
// AttackPower... `base` is the value the game sets and saves; `current` is what
// the rest of the game reads, `base` after any modifiers (effects, step 3 of the
// phase) and the clamp to [min, max]. With nothing modifying it, current == base.
struct Attribute {
    std::string name;
    f32 base = 0.0f;
    f32 current = 0.0f;
    f32 min = -3.0e38f;
    f32 max = 3.0e38f;
    // Modifiers from active effects (§30.3), set by the EffectSystem and not
    // saved (it rebuilds them from the entity's active effects).
    f32 add = 0.0f;
    f32 mul = 1.0f;
    bool has_override = false;
    f32 override_value = 0.0f;
};

// An entity's attributes, sorted by name so it saves and compares
// deterministically. A component. Reading and defining attributes is here; the
// AttributeSystem wraps it to tell the game when something changed.
struct AttributeSet {
    std::vector<Attribute> attributes;

    Attribute* Find(const std::string& name);
    const Attribute* Find(const std::string& name) const;
    bool Has(const std::string& name) const { return Find(name) != nullptr; }
    // The attribute's current value, or `fallback` if it has none.
    f32 Get(const std::string& name, f32 fallback = 0.0f) const;

    // Creates the attribute, or (if it exists) sets its bounds and base. Both are
    // clamped into the bounds; `min` above `max` is read as the range [max, min].
    // False for an invalid name or a NaN.
    bool Define(const std::string& name, f32 base, f32 min = -3.0e38f, f32 max = 3.0e38f);
    // Sets / adds to the base (clamped to the bounds). False if there is no such
    // attribute or the value is NaN (nothing changes then).
    bool SetBase(const std::string& name, f32 base);
    bool AddBase(const std::string& name, f32 delta);

    // `value` clamped into the attribute's bounds.
    static f32 Clamp(const Attribute& a, f32 value);
    // current = clamp(override, or (base + add) * mul). With no modifiers that is the clamped base.
    static void RecomputeCurrent(Attribute& a) { a.current = Clamp(a, a.has_override ? a.override_value : (a.base + a.add) * a.mul); }
    // Repairs a set loaded from hand-edited data: bounds in order, base and current re-derived.
    void Normalize();
};

} // namespace aether::gas

AETHER_REFLECT(aether::gas::Attribute, 1,
    AETHER_FIELD(name, Field_EditAnywhere),
    AETHER_FIELD(base, Field_EditAnywhere, {.tooltip = "The value the game sets"}),
    AETHER_FIELD(current, Field_EditAnywhere, {.tooltip = "The base after modifiers, within min and max"}),
    AETHER_FIELD(min, Field_EditAnywhere),
    AETHER_FIELD(max, Field_EditAnywhere))
AETHER_REFLECT(aether::gas::AttributeSet, 1, AETHER_FIELD(attributes, Field_EditAnywhere, {.tooltip = "Health, Mana, Stamina, ..."}))
