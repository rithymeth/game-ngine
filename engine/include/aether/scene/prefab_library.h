#pragma once

#include "aether/assets/asset_database.h"
#include "aether/scene/prefab.h"

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace aether {

// The editor's loaded prefab assets (Phase 9 step 4, docs/design/PHASE_SPECS.md
// §9.4), read from a project's AssetDatabase on first use and kept with
// their flattened form. When a prefab is saved or its file changes, the
// cached flat data of every prefab built on it (nesting it, or a variant of
// it, at any depth) is dropped, so the next lookup sees the change.
class PrefabLibrary {
public:
    explicit PrefabLibrary(assets::AssetDatabase& database) : database_(database) {}

    // The prefab as saved, or null if `guid` isn't a readable prefab asset
    // (LastError says why).
    const PrefabData* Get(const assets::AssetGuid& guid);
    // Its flattened data (FlattenPrefab), or null on error.
    const PrefabData* Flattened(const assets::AssetGuid& guid);
    std::string LastError(const assets::AssetGuid& guid) const;

    // For ResolveAllPrefabInstances (as saved) and CommandContext::find_prefab
    // (flattened). Valid while the library is.
    PrefabLookup Lookup();
    PrefabLookup FlatLookup();

    // Writes the prefab's file and replaces the loaded copy. Returns the
    // prefabs whose data this changes (see Dependents), or nothing on error.
    std::vector<assets::AssetGuid> Save(const assets::AssetGuid& guid, PrefabData data, std::string* error = nullptr);
    // Drops the loaded copy (the file changed on disk); the next Get reads
    // it again. Returns the affected prefabs, like Save.
    std::vector<assets::AssetGuid> Reload(const assets::AssetGuid& guid);

    // `guid` and every prefab asset that nests it or is a variant of it,
    // directly or not, sorted. Reads every prefab in the database.
    std::vector<assets::AssetGuid> Dependents(const assets::AssetGuid& guid);

    assets::AssetDatabase& Database() { return database_; }

private:
    struct Entry {
        bool loaded = false;
        std::optional<PrefabData> data;
        std::optional<PrefabData> flat;
        std::string error;
    };
    Entry& EntryFor(const assets::AssetGuid& guid);
    void Invalidate(const std::vector<assets::AssetGuid>& guids);

    assets::AssetDatabase& database_;
    std::unordered_map<assets::AssetGuid, Entry> entries_;
};

struct PropagationReport {
    usize instances = 0; // instances re-resolved
    std::vector<std::pair<Entity, ResolveReport>> results; // failures and orphaned overrides are in here
    usize orphaned = 0;
    usize failed = 0;
};

// After prefab `changed` was saved or reloaded: re-resolves every instance in
// the world of it or of a prefab built on it (§9.4). Entities are reused, so
// selection and references survive.
PropagationReport PropagatePrefabChange(World& world, GuidIndex& guids, PrefabLibrary& library,
                                        const assets::AssetGuid& changed);

// How a prefab change affects one saved scene.
struct SceneImpact {
    std::string scene;           // content-relative path
    usize instances = 0;         // instances in it of the prefab (or ones built on it)
    std::vector<PropertyOverride> orphaned; // overrides the change broke
    std::vector<std::string> errors;        // instances that couldn't be resolved at all
};

// Every scene using `prefab` (directly or through prefabs built on it, per
// the database's last Scan), loaded into a scratch world and resolved
// against the library: the list to warn with when a change breaks instances
// (§9.4). Scenes on disk are not modified.
std::vector<SceneImpact> CheckScenesUsingPrefab(PrefabLibrary& library, const assets::AssetGuid& prefab);

} // namespace aether
