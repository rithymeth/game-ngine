#pragma once

#include "aether/ecs/world.h"
#include "aether/reflection/reflection.h"
#include "aether/scene/entity_guid.h"
#include "aether/scene/lifecycle.h"

#include <string>
#include <vector>

// World state in a save (Phase 28 step 2, docs/design/PHASE_SPECS.md §28.3):
// the things in a level that change as you play and must be there after a
// load (a door that was opened, a pickup that was taken, an enemy that was
// killed), keyed by the entity's EntityGuid so they find their entity again
// when the scene is loaded afresh.
//
// An entity opts in with a SaveableEntity component that lists what to keep:
// "Door.open" keeps one field of its Door component, "Door" the whole
// component. CaptureWorld turns every such entity into a SavedEntity inside a
// WorldSnapshot; RestoreWorld applies a snapshot to a freshly loaded scene.
// A snapshot is a reflected struct, so it can be a field of the game's own
// save struct or be saved on its own with SaveSystem.
//
// An entity that was destroyed isn't there to capture, so a WorldTracker
// remembers which saveable entities the scene started with (call Begin right
// after loading it, and again after restoring); capture lists the ones that
// are gone, and restore destroys them again.
//
// Not covered: entities made while playing (they are counted and skipped,
// not saved), and Blueprint or script variables.

namespace aether {

// Marks an entity whose state is saved. It needs an IdComponent (the
// editor gives every entity one).
struct SaveableEntity {
    std::string tag; // free text for the game ("door_a")
    // What to keep: "Component.field" for one field, "Component" for the
    // whole component. Names are the reflected ones ("Transform.position").
    std::vector<std::string> fields;
};

} // namespace aether

AETHER_REFLECT(aether::SaveableEntity, 1,
    AETHER_FIELD(tag, Field_EditAnywhere, {.tooltip = "A label for the game"}),
    AETHER_FIELD(fields, Field_EditAnywhere, {.tooltip = "What to save: 'Component.field' for one field, 'Component' for all of it"})
)

namespace aether::save {

// One component's saved values: its reflected name and the values as compact
// JSON (only the listed fields, or all of them for a whole component).
struct SavedComponent {
    std::string name;
    std::string data;
};

struct SavedEntity {
    EntityGuid guid;
    std::string tag;
    std::vector<SavedComponent> components;
};

struct WorldSnapshot {
    std::vector<SavedEntity> entities;
    std::vector<EntityGuid> destroyed; // saveable entities the scene started with that are gone
};

} // namespace aether::save

AETHER_REFLECT(aether::save::SavedComponent, 1, AETHER_FIELD(name, Field_EditAnywhere), AETHER_FIELD(data, Field_EditAnywhere))
AETHER_REFLECT(aether::save::SavedEntity, 1, AETHER_FIELD(guid, Field_EditAnywhere), AETHER_FIELD(tag, Field_EditAnywhere),
               AETHER_FIELD(components, Field_EditAnywhere))
AETHER_REFLECT(aether::save::WorldSnapshot, 1, AETHER_FIELD(entities, Field_EditAnywhere), AETHER_FIELD(destroyed, Field_EditAnywhere))

namespace aether::save {

// The saveable entities a scene started with.
class WorldTracker {
public:
    // Records every entity with a SaveableEntity and an IdComponent. Call it
    // right after the scene is loaded (and again after RestoreWorld, so
    // entities it destroyed still count as destroyed).
    void Begin(World& world, GuidIndex& guids);
    bool Tracking() const { return tracking_; }
    const std::vector<EntityGuid>& Baseline() const { return baseline_; }
    bool InBaseline(const EntityGuid& guid) const;

private:
    std::vector<EntityGuid> baseline_;
    bool tracking_ = false;
};

struct CaptureReport {
    u32 entities = 0;        // saved
    u32 skipped_runtime = 0; // saveable entities that weren't in the scene when it loaded
    std::vector<std::string> warnings; // an entity with no GUID, a field or component that doesn't exist, a duplicate GUID
};

// Captures every SaveableEntity's listed state (and, with a tracker, which
// saveable entities have been destroyed). Refreshes `guids` first.
WorldSnapshot CaptureWorld(World& world, GuidIndex& guids, const WorldTracker* tracker = nullptr, CaptureReport* report = nullptr);

struct RestoreReport {
    u32 applied = 0;   // component states written
    u32 destroyed = 0; // entities destroyed
    std::vector<EntityGuid> missing;           // saved entities the scene doesn't have
    std::vector<std::string> unknown_components; // "Door" (no such component, or one that can't be saved by field)
    std::vector<std::string> unknown_fields;     // "Door.hinge"
    std::vector<std::string> warnings;           // an entity that lacks the component, bad data, and what loading skipped
};

// Applies a snapshot to the loaded scene: finds each entity by GUID, writes
// the saved fields (the others keep their values; a migration hook runs for
// older component versions), and destroys the entities recorded as
// destroyed (through `lifecycle` when given, so OnDestroy fires). A
// component the entity lacks isn't added. Returns true when nothing went
// wrong; otherwise the report says what (a saved entity the scene no longer
// has, a component or field that no longer exists, a warning), and
// everything that could be restored was. Call it after the scene is loaded
// and before play starts.
bool RestoreWorld(World& world, GuidIndex& guids, const WorldSnapshot& snapshot, RestoreReport* report = nullptr, Lifecycle* lifecycle = nullptr);

} // namespace aether::save
