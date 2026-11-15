#pragma once

#include "aether/core/base.h"
#include "aether/math/math.h"
#include "aether/reflection/reflection.h"

#include <string>

namespace aether::chk {

// A place the player can be put back to: when an entity with `activator_tag` comes within `radius` of this entity it becomes the last checkpoint
// (see CheckpointSystem). A component, saved with the entity. The state (which checkpoint is the last, which have been reached) is not here but in
// the game's save bag, so it goes into and out of a save slot with the rest of the game.
struct Checkpoint {
    static constexpr f32 kMinRadius = 0.01f;
    static constexpr f32 kMaxRadius = 10000.0f;

    bool enabled = true;
    std::string name;                 // what scripts and the save call it; a checkpoint with no name does nothing
    f32 radius = 3.0f;                // how close the activator has to come, metres
    std::string activator_tag = "Player"; // which entities activate it (the ones carrying this tag)
    Vec3 respawn_offset{0.0f, 0.5f, 0.0f}; // where Respawn puts an entity, relative to this entity (a little above the ground by default)
};

void RegisterCheckpointComponents();

} // namespace aether::chk

AETHER_REFLECT(aether::chk::Checkpoint, 1,
    AETHER_FIELD(enabled, Field_EditAnywhere),
    AETHER_FIELD(name, Field_EditAnywhere, {.tooltip = "What scripts and the save call it; a checkpoint with no name does nothing"}),
    AETHER_FIELD(radius, Field_EditAnywhere, {.tooltip = "How close an activator has to come", .range_min = 0.01, .range_max = 10000.0, .units = "m"}),
    AETHER_FIELD(activator_tag, Field_EditAnywhere, {.tooltip = "Entities carrying this tag activate it"}),
    AETHER_FIELD(respawn_offset, Field_EditAnywhere, {.tooltip = "Where Respawn puts an entity, relative to this one", .units = "m"}))
