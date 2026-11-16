#pragma once

#include "aether/core/base.h"
#include "aether/math/math.h"
#include "aether/reflection/reflection.h"

namespace aether::destruction {

// Something that breaks. Put it on an entity with a BoxCollider (and usually a RigidBody and a ModelRenderer): when it breaks, the entity is replaced by a
// grid of `pieces_x` x `pieces_y` x `pieces_z` smaller boxes that fall, tumble and scatter under physics, each with the source's model scaled down to fit.
// A component, saved with the entity. It breaks when a hit is hard enough (`break_speed`) or when damage has used up `health` (Damage, from scripts).
struct Destructible {
    static constexpr u8 kMaxPieces = 6; // per axis: at most 216 pieces from one break

    bool enabled = true;
    f32 health = 0.0f;       // damage it takes before breaking; 0 means damage does not break it (only a hard hit does)
    f32 break_speed = 6.0f;  // breaks when something hits it at least this fast, m/s along the contact; 0 means a hit never breaks it
    u8 pieces_x = 3, pieces_y = 3, pieces_z = 3; // how many pieces along each of its axes (1 to 6)
    f32 mass = 10.0f;        // the whole thing's mass; each piece gets its share
    f32 burst_speed = 3.0f;  // how fast the pieces are thrown outward from the break point, m/s
    f32 inherit_velocity = 1.0f; // how much of the body's own velocity the pieces keep (0 to 1)
    f32 piece_lifetime = 8.0f;   // seconds before a piece is removed; 0 keeps them
};

// A piece left by a break; the system removes it when `remaining` runs out. Pieces beyond the system's cap go first, oldest first.
struct Debris {
    f32 remaining = 8.0f; // seconds left; a negative number lasts forever
    u64 serial = 0;       // the order pieces were made in
};

void RegisterDestructionComponents();

} // namespace aether::destruction

AETHER_REFLECT(aether::destruction::Destructible, 1,
    AETHER_FIELD(enabled, Field_EditAnywhere),
    AETHER_FIELD(health, Field_EditAnywhere, {.tooltip = "Damage it takes before breaking; 0 means only a hard hit breaks it", .range_min = 0.0, .range_max = 100000.0}),
    AETHER_FIELD(break_speed, Field_EditAnywhere, {.tooltip = "Breaks when hit at least this fast; 0 means a hit never breaks it", .range_min = 0.0, .range_max = 1000.0, .units = "m/s"}),
    AETHER_FIELD(pieces_x, Field_EditAnywhere, {.tooltip = "Pieces along its x axis (1 to 6)", .range_min = 1.0, .range_max = 6.0}),
    AETHER_FIELD(pieces_y, Field_EditAnywhere, {.tooltip = "Pieces along its y axis (1 to 6)", .range_min = 1.0, .range_max = 6.0}),
    AETHER_FIELD(pieces_z, Field_EditAnywhere, {.tooltip = "Pieces along its z axis (1 to 6)", .range_min = 1.0, .range_max = 6.0}),
    AETHER_FIELD(mass, Field_EditAnywhere, {.tooltip = "The whole thing's mass; each piece gets its share", .range_min = 0.01, .range_max = 100000.0, .units = "kg"}),
    AETHER_FIELD(burst_speed, Field_EditAnywhere, {.tooltip = "How fast the pieces are thrown outward from the break", .range_min = 0.0, .range_max = 100.0, .units = "m/s"}),
    AETHER_FIELD(inherit_velocity, Field_EditAnywhere, {.tooltip = "How much of its own velocity the pieces keep", .range_min = 0.0, .range_max = 1.0}),
    AETHER_FIELD(piece_lifetime, Field_EditAnywhere, {.tooltip = "Seconds before a piece is removed; 0 keeps them", .range_min = 0.0, .range_max = 3600.0, .units = "s"}))

AETHER_REFLECT(aether::destruction::Debris, 1,
    AETHER_FIELD(remaining, Field_EditAnywhere, {.units = "s"}),
    AETHER_FIELD(serial, Field_ReadOnly))
