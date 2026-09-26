#pragma once

#include "aether/assets/asset_ref.h"
#include "aether/ecs/world.h"
#include "aether/scene/components.h"
#include "aether/scene/entity_guid.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace aether {

// Prefabs (Phase 9, docs/design/PHASE_SPECS.md §9.1-9.2): a reusable group of
// entities saved as an asset (.aprefab), placed into scenes as instances that
// follow later edits to the prefab, except for the fields an instance
// overrides.

// Identifies an entity inside one prefab (unique only there). 0 = none.
using PrefabLocalId = u32;

struct PrefabEntity {
    PrefabLocalId id = 0;
    PrefabLocalId parent = 0; // 0 for the prefab's root
    // Component declared name -> its reflected JSON (the same form as JSON
    // scenes). Identity and hierarchy (IdComponent, Parent) and the prefab
    // components themselves are never stored here.
    nlohmann::json components = nlohmann::json::object();
};

// A prefab asset's contents: the root first, then its descendants
// (parents before children).
struct PrefabData {
    std::vector<PrefabEntity> entities;

    const PrefabEntity* Find(PrefabLocalId id) const;
    PrefabEntity* Find(PrefabLocalId id);
    PrefabLocalId Root() const { return entities.empty() ? 0 : entities.front().id; }
};

// .aprefab files are JSON: {"$type": "Prefab", "$version": 1, "entities": [
//   {"id": 1, "parent": 0, "components": {"Transform": {...}}}, ...]}.
bool SavePrefab(const PrefabData& prefab, const std::filesystem::path& path, std::string* error = nullptr);
bool LoadPrefab(const std::filesystem::path& path, PrefabData& out, std::string* error = nullptr);
nlohmann::json PrefabToJson(const PrefabData& prefab);
bool PrefabFromJson(const nlohmann::json& json, PrefabData& out, std::string* error = nullptr);

// Captures `root` and all its descendants (by Parent) as prefab data, with
// local IDs 1, 2, ... in depth-first order. Only reflected components are
// kept (like JSON scenes); non-reflected ones are left out.
PrefabData MakePrefab(const World& world, const GuidIndex& guids, Entity root);

// One field of one entity of the prefab, changed in this instance.
struct PropertyOverride {
    PrefabLocalId entity = 0;
    std::string component;  // declared name: "Transform"
    // A path into the component's JSON: "max_health", "items[2].tint",
    // "position[1]" (vectors are saved as [x, y, z]); "" = the whole component.
    std::string field_path;
    std::string value;      // the field's JSON, as text
};

// On an instance's root entity.
struct PrefabInstance {
    assets::AssetRef<assets::PrefabAsset> source;
    std::vector<PropertyOverride> overrides;
    std::vector<PrefabLocalId> removed_entities; // deleted in this instance (with their children)
};

// On every entity created from a prefab (the root included): which instance
// it belongs to and which prefab entity it is. This is how re-resolving finds
// and reuses existing entities, so references to them stay valid.
struct PrefabLink {
    EntityGuid instance; // the instance root's GUID
    PrefabLocalId local_id = 0;
};

// Sets (or replaces) an override, or removes one.
void SetOverride(PrefabInstance& instance, PrefabLocalId entity, const std::string& component,
                 const std::string& field_path, const nlohmann::json& value);
bool RemoveOverride(PrefabInstance& instance, PrefabLocalId entity, const std::string& component,
                    const std::string& field_path);

// ---------------------------------------------------------------------------
// Recording, Apply and Revert (§9.3)
// ---------------------------------------------------------------------------

// The root of the instance `entity` belongs to (itself for a root), or
// kNullEntity if it isn't part of one.
Entity FindInstanceRoot(const World& world, const GuidIndex& guids, Entity entity);

// After a component of an instance's entity was edited: records how it now
// differs from the prefab as per-field overrides, replacing that component's
// earlier overrides. A field edited back to the prefab's value loses its
// override (so the Inspector stops marking it). Overrides that are orphaned
// (their field isn't in the prefab) are kept. The root's Transform is its
// placement, not an override, so it records nothing. Returns false if
// `entity` isn't part of an instance, or the prefab has no such component
// on that entity (added components aren't supported yet).
bool RecordPrefabOverrides(World& world, const GuidIndex& guids, Entity entity, ComponentId component,
                           const PrefabData& prefab);

// Whether the Inspector should mark a field as overridden: an override on
// exactly `field_path`, inside it ("position" when "position[1]" is
// overridden) or around it (the whole component).
bool IsFieldOverridden(const PrefabInstance& instance, PrefabLocalId entity, const std::string& component,
                       const std::string& field_path);

// The overrides a selection covers: entity 0 = every entity, component "" =
// every component, field_path "" = the whole component; otherwise the path
// and anything inside it.
bool OverrideMatches(const PropertyOverride& override_, PrefabLocalId entity, const std::string& component,
                     const std::string& field_path);

// "Apply to Prefab": writes the selected overrides' values into `prefab`
// and removes them from `instance`. Orphaned ones can't be applied and stay.
// The caller saves the prefab and re-resolves its instances (others that
// override the same field keep their own values). Returns how many applied.
usize ApplyOverridesToPrefab(PrefabData& prefab, PrefabInstance& instance, PrefabLocalId entity = 0,
                             const std::string& component = "", const std::string& field_path = "");

// "Revert": removes the selected overrides; resolve the instance afterwards.
// Returns how many were removed.
usize RevertOverrides(PrefabInstance& instance, PrefabLocalId entity = 0, const std::string& component = "",
                      const std::string& field_path = "");

struct ResolveReport {
    bool ok = false;
    std::string error;
    usize created = 0;
    usize updated = 0;
    usize destroyed = 0;
    // Overrides whose entity, component or field no longer exists in the
    // prefab. They're kept (not deleted: that would lose the user's data
    // silently) and reported, for the Messages panel.
    std::vector<PropertyOverride> orphaned;
};

// Brings the instance whose root is `root` (which has a PrefabInstance) in
// line with `prefab` (§9.2): prefab data, minus removed entities, plus
// overrides, then entities created, updated or destroyed to match. Entities
// that already belong to the instance are reused. The root keeps its own
// Transform and Parent (where the instance is placed); its other components
// come from the prefab. Entities added under the instance by hand have no
// PrefabLink, so they're left alone. Nested prefabs come in a later step.
ResolveReport ResolvePrefabInstance(World& world, GuidIndex& guids, Entity root, const PrefabData& prefab);

// Creates a new instance of `prefab` (asset `source`) and resolves it.
// The root is placed at `transform` if given, else where the prefab's root is.
Entity InstantiatePrefab(World& world, GuidIndex& guids, const assets::AssetGuid& source, const PrefabData& prefab,
                         const Transform* transform = nullptr, ResolveReport* report = nullptr);

// Re-resolves every instance in the world (after loading a scene, or when a
// prefab asset changed). `find` returns a prefab's data by asset GUID, or
// null if it isn't available (those instances are reported and left as
// they are). Returns one report per instance root, in entity order.
using PrefabLookup = std::function<const PrefabData*(const assets::AssetGuid&)>;
std::vector<std::pair<Entity, ResolveReport>> ResolveAllPrefabInstances(World& world, GuidIndex& guids,
                                                                         const PrefabLookup& find);

} // namespace aether

AETHER_REFLECT(aether::PropertyOverride, 1,
    AETHER_FIELD(entity),
    AETHER_FIELD(component),
    AETHER_FIELD(field_path),
    AETHER_FIELD(value)
)

AETHER_REFLECT(aether::PrefabInstance, 1,
    AETHER_FIELD(source, Field_EditAnywhere, {.tooltip = "The prefab this is an instance of"}),
    AETHER_FIELD(overrides, Field_ReadOnly),
    AETHER_FIELD(removed_entities, Field_ReadOnly)
)

AETHER_REFLECT(aether::PrefabLink, 1,
    AETHER_FIELD(instance, Field_ReadOnly),
    AETHER_FIELD(local_id, Field_ReadOnly)
)
