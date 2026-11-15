#pragma once

#include "aether/core/base.h"
#include "aether/math/math.h"
#include "aether/reflection/reflection.h"

namespace aether::ff {

enum class FieldShape : u8 { Sphere, Box };
enum class FieldMode : u8 {
    Directional, // pushes along `direction`: wind, a conveyor, a lift, anti-gravity
    Radial,      // toward the centre (strength > 0) or away from it (strength < 0): a gravity well, a repeller, a fan
    Vortex,      // around the field's up axis: a whirlpool, a tornado
    Buoyancy,    // lifts what is below the top of the volume: water. `strength` is how much of gravity it cancels at full submersion
    Drag         // slows what moves through it, by `strength` per second: mud, thick air, deep water
};

// A volume that pushes the dynamic bodies inside it, every physics step. A component, saved with the entity; the volume is at the entity's Transform
// (a box turns with it). Static and kinematic bodies are not pushed. Which bodies are inside is the physics overlap query's: a body counts while its
// collider touches the volume, and the push is worked out from where its centre of mass is.
struct ForceField {
    static constexpr f32 kMinSize = 0.01f;
    static constexpr f32 kMaxSize = 10000.0f;

    bool enabled = true;
    FieldShape shape = FieldShape::Sphere;
    f32 radius = 5.0f;                    // Sphere: how big
    Vec3 half_extents{5.0f, 5.0f, 5.0f};  // Box: half the size on each axis (turned with the entity)
    FieldMode mode = FieldMode::Directional;
    // What the field does, by mode. Directional, Radial and Vortex: the size of the push (m/s^2, or newtons when `mass_independent` is off). Buoyancy: the
    // fraction of gravity cancelled at full submersion (1 holds a body up, 2 floats it up). Drag: the coefficient, 1/s.
    f32 strength = 10.0f;
    Vec3 direction{0.0f, 1.0f, 0.0f};     // Directional: which way, in the world (any length)
    f32 falloff = 1.0f;                   // Radial and Vortex: 0 is the same everywhere, 1 fades linearly to nothing at the edge, higher fades faster
    bool mass_independent = true;         // Directional, Radial and Vortex: strength is an acceleration (a heavy and a light body move alike), not a force
    f32 fluid_drag = 0.0f;                // Buoyancy: also slows what is in it, by this per second at full submersion
};

void RegisterForceFieldComponents();

} // namespace aether::ff

AETHER_ENUM(aether::ff::FieldShape, 1, AETHER_ENUM_VALUE(Sphere), AETHER_ENUM_VALUE(Box))
AETHER_ENUM(aether::ff::FieldMode, 1, AETHER_ENUM_VALUE(Directional), AETHER_ENUM_VALUE(Radial), AETHER_ENUM_VALUE(Vortex), AETHER_ENUM_VALUE(Buoyancy), AETHER_ENUM_VALUE(Drag))
AETHER_REFLECT(aether::ff::ForceField, 1,
    AETHER_FIELD(enabled, Field_EditAnywhere),
    AETHER_FIELD(shape, Field_EditAnywhere),
    AETHER_FIELD(radius, Field_EditAnywhere, {.tooltip = "Sphere: how big", .range_min = 0.01, .range_max = 10000.0, .units = "m"}),
    AETHER_FIELD(half_extents, Field_EditAnywhere, {.tooltip = "Box: half the size on each axis (it turns with the entity)", .units = "m"}),
    AETHER_FIELD(mode, Field_EditAnywhere),
    AETHER_FIELD(strength, Field_EditAnywhere, {.tooltip = "Directional/Radial/Vortex: the push (m/s^2). Buoyancy: the fraction of gravity cancelled when fully submerged. Drag: the coefficient, 1/s"}),
    AETHER_FIELD(direction, Field_EditAnywhere, {.tooltip = "Directional: which way, in the world"}),
    AETHER_FIELD(falloff, Field_EditAnywhere, {.tooltip = "Radial and Vortex: 0 the same everywhere, 1 fades linearly to nothing at the edge", .range_min = 0.0, .range_max = 8.0}),
    AETHER_FIELD(mass_independent, Field_EditAnywhere, {.tooltip = "Directional/Radial/Vortex: strength is an acceleration (heavy and light bodies move alike) rather than a force"}),
    AETHER_FIELD(fluid_drag, Field_EditAnywhere, {.tooltip = "Buoyancy: also slows what is in it, per second at full submersion", .range_min = 0.0, .range_max = 100.0, .units = "1/s"}))
