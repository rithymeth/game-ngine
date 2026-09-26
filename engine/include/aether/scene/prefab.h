#pragma once

#include "aether/assets/asset_ref.h"
#include "aether/ecs/world.h"
#include "aether/scene/components.h"
#include "aether/scene/entity_guid.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace aether {

// Prefabs (Phase 9, docs/design/PHASE_SPECS.md §9.1-9.2): a reusable group of
// entities saved as an asset (.aprefab), placed into scenes as instances that
// follow later edits to the prefab, except for the fields an instance
// overrides.

// Identifies an entity inside one prefab (unique only there). 0 = none.
using PrefabLocalId = u32;

// One field of one entity of the prefab, changed in this instance.
struct PropertyOverride {
    PrefabLocalId entity = 0;
    std::string component;  // declared name: "Transform"
    // A path into the component's JSON: "max_health", "items[2].tint",
    // "position[1]" (vectors are saved as [x, y, z]); "" = the whole component.
    std::string field_path;
    std::string value;      // the field's JSON, as text
};

// An instance of another prefab placed inside this one (nested prefab).
struct NestedPrefab {
    assets::AssetGuid source;
    std::vector<PropertyOverride> overrides;      // on the nested prefab's (flattened) entities
    std::vector<PrefabLocalId> removed_entities;  // the nested prefab's entities left out here
};

struct PrefabEntity {
    PrefabLocalId id = 0;
    PrefabLocalId parent = 0; // 0 for the prefab's root
    // Component declared name -> its reflected JSON (the same form as JSON
    // scenes). Identity and hierarchy (IdComponent, Parent) and the prefab
    // components themselves are never stored here.
    nlohmann::json components = nlohmann::json::object();
    // Set if this entity is an instance of another prefab: it stands for that
    // prefab's root (`components` are applied over the root's, e.g. its
    // Transform), and the rest of that prefab's entities come with it.
    std::optional<NestedPrefab> nested;
};

// A prefab asset's contents: the root first, then its descendants
// (parents before children).
//
// A prefab can also be a *variant* of another (`base` set): the base's
// entities with `base_overrides` and `base_removed` applied, plus `entities`,
// which are then only the entities the variant adds (each attached to a
// parent from the base or added earlier).
//
// Nested prefabs and variants are resolved by FlattenPrefab into "flat" data
// (no base, no nested entities), which is what instances are resolved from.
struct PrefabData {
    assets::AssetGuid base;
    std::vector<PropertyOverride> base_overrides;
    std::vector<PrefabLocalId> base_removed;
    std::vector<PrefabEntity> entities;

    bool IsVariant() const { return !base.IsNull(); }
    bool IsFlat() const;

    const PrefabEntity* Find(PrefabLocalId id) const;
    PrefabEntity* Find(PrefabLocalId id);
    PrefabLocalId Root() const { return entities.empty() ? 0 : entities.front().id; }
};

// .aprefab files are JSON: {"$type": "Prefab", "$version": 1, "entities": [
//   {"id": 1, "parent": 0, "components": {"Transform": {...}}}, ...]}.
// A nested entity adds "prefab": {"source": guid, "overrides": [...],
// "removed": [...]}; a variant adds a top-level "base" of the same shape.
bool SavePrefab(const PrefabData& prefab, const std::filesystem::path& path, std::string* error = nullptr);
bool LoadPrefab(const std::filesystem::path& path, PrefabData& out, std::string* error = nullptr);
nlohmann::json PrefabToJson(const PrefabData& prefab);
bool PrefabFromJson(const nlohmann::json& json, PrefabData& out, std::string* error = nullptr);

// Captures `root` and all its descendants (by Parent) as prefab data, with
// local IDs 1, 2, ... in depth-first order. Only reflected components are
// kept (like JSON scenes); non-reflected ones are left out. A prefab instance
// among them (the root included) becomes a nested prefab entry — its source,
// overrides, removed entities and Transform — rather than copies of its
// entities.
PrefabData MakePrefab(const World& world, const GuidIndex& guids, Entity root);

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

// A prefab asset's data by GUID, or null if it isn't available.
using PrefabLookup = std::function<const PrefabData*(const assets::AssetGuid&)>;

inline constexpr int kMaxPrefabNesting = 32;

// The local ID an entity of a nested prefab gets in the outer prefab's flat
// data: derived from the nesting entity's ID and its own, so it's stable (the
// same every time the prefab is flattened) and overrides can refer to it. The
// nested prefab's root takes the nesting entity's own ID.
PrefabLocalId NestedLocalId(PrefabLocalId outer, PrefabLocalId inner);

// Resolves nesting and variants (§9.2 step 1) into flat data. Precedence, from
// weakest: a nested prefab's defaults, the overrides of the prefab that
// nests it, variant overrides; instance overrides then apply on top when an
// instance is resolved. Fails on a missing prefab, too-deep nesting
// (kMaxPrefabNesting), clashing local IDs, and cycles (a prefab that contains
// itself or is a variant of itself, directly or not); the error names the
// chain. `self` is the prefab's own GUID, if known, so a prefab referring to
// itself is caught. Overrides at any level that no longer apply go to
// `orphaned`.
bool FlattenPrefab(const PrefabData& prefab, const PrefabLookup& find, PrefabData& out, std::string* error = nullptr,
                   std::vector<PropertyOverride>* orphaned = nullptr, const assets::AssetGuid& self = {});
bool FlattenPrefab(const assets::AssetGuid& source, const PrefabLookup& find, PrefabData& out,
                   std::string* error = nullptr, std::vector<PropertyOverride>* orphaned = nullptr);

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
// For a variant, overrides of entities that come from its base become
// variant overrides (base_overrides). Overrides of entities inside nested
// prefabs aren't applied yet (they stay on the instance).
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
// line with `prefab` (§9.2), which must be flat (FlattenPrefab): prefab data, minus removed entities, plus
// overrides, then entities created, updated or destroyed to match. Entities
// that already belong to the instance are reused. The root keeps its own
// Transform and Parent (where the instance is placed); its other components
// come from the prefab. Entities added under the instance by hand have no
// PrefabLink, so they're left alone.
ResolveReport ResolvePrefabInstance(World& world, GuidIndex& guids, Entity root, const PrefabData& prefab);

// Creates a new instance of `prefab` (asset `source`) and resolves it.
// The root is placed at `transform` if given, else where the prefab's root is.
Entity InstantiatePrefab(World& world, GuidIndex& guids, const assets::AssetGuid& source, const PrefabData& prefab,
                         const Transform* transform = nullptr, ResolveReport* report = nullptr);

// Re-resolves every instance in the world (after loading a scene, or when a
// prefab asset changed). `find` returns a prefab's data by asset GUID (as
// saved: it's flattened here). Instances whose prefab is unavailable or can't
// be flattened are reported and left as they are. Returns one report per
// instance root, in entity order.
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
