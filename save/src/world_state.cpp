#include "aether/save/world_state.h"

#include "aether/core/log.h"
#include "aether/ecs/component.h"
#include "aether/reflection/serialize.h"

#include <algorithm>
#include <map>
#include <set>

namespace aether::save {

using reflect::Json;

namespace {

// "Door.open" -> {"Door", "open"}; "Door" -> {"Door", ""}.
std::pair<std::string, std::string> SplitSpec(const std::string& spec) {
    const usize dot = spec.find('.');
    if (dot == std::string::npos) return {spec, {}};
    return {spec.substr(0, dot), spec.substr(dot + 1)};
}

std::vector<Entity> SaveableEntities(World& world) {
    std::vector<Entity> out;
    const ComponentId saveable = GetComponentId<SaveableEntity>();
    world.ForEachArchetype([&](Archetype& archetype) {
        if (!archetype.Mask().test(saveable)) return;
        for (usize c = 0; c < archetype.ChunkCount(); ++c) {
            Entity* e = archetype.EntityArray(c);
            out.insert(out.end(), e, e + archetype.ChunkEntityCount(c));
        }
    });
    return out;
}

void Warn(std::vector<std::string>& list, std::string message) {
    AETHER_LOG_WARN("Save", "%s", message.c_str());
    list.push_back(std::move(message));
}

} // namespace

void WorldTracker::Begin(World& world, GuidIndex& guids) {
    guids.Rebuild(world);
    baseline_.clear();
    for (Entity e : SaveableEntities(world)) {
        const IdComponent* id = world.GetComponent<IdComponent>(e);
        if (id && !id->guid.IsNull()) baseline_.push_back(id->guid);
    }
    tracking_ = true;
}

bool WorldTracker::InBaseline(const EntityGuid& guid) const {
    return std::find(baseline_.begin(), baseline_.end(), guid) != baseline_.end();
}

WorldSnapshot CaptureWorld(World& world, GuidIndex& guids, const WorldTracker* tracker, CaptureReport* report) {
    CaptureReport local;
    CaptureReport& rep = report ? *report : local;
    rep = CaptureReport{};
    guids.Rebuild(world);
    WorldSnapshot snapshot;
    std::set<std::pair<u64, u64>> seen;
    std::vector<Entity> entities = SaveableEntities(world);
    // Entity order isn't stable across runs; the guid order is, so snapshots of the same state match.
    std::sort(entities.begin(), entities.end(), [&](Entity a, Entity b) {
        const IdComponent* ia = world.GetComponent<IdComponent>(a);
        const IdComponent* ib = world.GetComponent<IdComponent>(b);
        const EntityGuid ga = ia ? ia->guid : EntityGuid{}, gb = ib ? ib->guid : EntityGuid{};
        if (ga.hi != gb.hi) return ga.hi < gb.hi;
        if (ga.lo != gb.lo) return ga.lo < gb.lo;
        // Two entities sharing a GUID: the one created first is "the first" (the other is reported, not saved),
        // whatever order the ECS happens to iterate them in, which depends on component registration order.
        return a.index != b.index ? a.index < b.index : a.generation < b.generation;
    });
    for (Entity e : entities) {
        const IdComponent* id = world.GetComponent<IdComponent>(e);
        const SaveableEntity* saveable = world.GetComponent<SaveableEntity>(e);
        if (!id || id->guid.IsNull()) {
            Warn(rep.warnings, "a SaveableEntity has no GUID, so it can't be saved");
            continue;
        }
        if (!seen.insert({id->guid.hi, id->guid.lo}).second) {
            Warn(rep.warnings, "two saveable entities share the GUID " + ToString(id->guid) + "; only the first is saved");
            continue;
        }
        if (tracker && tracker->Tracking() && !tracker->InBaseline(id->guid)) {
            ++rep.skipped_runtime;
            continue;
        }
        SavedEntity saved;
        saved.guid = id->guid;
        saved.tag = saveable->tag;
        // Group the specs by component, in the order first named.
        std::vector<std::string> order;
        std::map<std::string, std::vector<std::string>> wanted; // component -> fields (empty entry: all)
        for (const std::string& spec : saveable->fields) {
            const auto [component, field] = SplitSpec(spec);
            if (component.empty()) continue;
            if (!wanted.count(component)) order.push_back(component);
            wanted[component].push_back(field);
        }
        for (const std::string& component : order) {
            const std::string where = ToString(id->guid) + " (" + saved.tag + "): ";
            const ComponentId cid = FindComponentIdByName(component);
            if (cid == kInvalidComponentId) {
                Warn(rep.warnings, where + "no component named '" + component + "'");
                continue;
            }
            const ComponentInfo& info = GetComponentInfo(cid);
            if (!info.reflected) {
                Warn(rep.warnings, where + "'" + component + "' isn't reflected, so it can't be saved");
                continue;
            }
            if (!world.HasComponentRaw(e, cid)) {
                Warn(rep.warnings, where + "the entity has no '" + component + "'");
                continue;
            }
            const Json all = reflect::ToJson(*info.reflected, world.GetComponentRaw(e, cid));
            const std::vector<std::string>& fields = wanted[component];
            const bool whole = std::find(fields.begin(), fields.end(), std::string()) != fields.end();
            Json data = Json::object();
            if (whole) {
                data = all;
            } else {
                for (const std::string& field : fields) {
                    if (all.contains(field)) data[field] = all[field];
                    else Warn(rep.warnings, where + "'" + component + "' has no field '" + field + "'");
                }
            }
            saved.components.push_back({component, data.dump()});
        }
        snapshot.entities.push_back(std::move(saved));
        ++rep.entities;
    }
    if (tracker && tracker->Tracking()) {
        for (const EntityGuid& guid : tracker->Baseline()) {
            if (guids.Find(world, guid).IsNull()) snapshot.destroyed.push_back(guid);
        }
        std::sort(snapshot.destroyed.begin(), snapshot.destroyed.end(), [](const EntityGuid& a, const EntityGuid& b) {
            return a.hi != b.hi ? a.hi < b.hi : a.lo < b.lo;
        });
    }
    return snapshot;
}

bool RestoreWorld(World& world, GuidIndex& guids, const WorldSnapshot& snapshot, RestoreReport* report, Lifecycle* lifecycle) {
    RestoreReport local;
    RestoreReport& rep = report ? *report : local;
    rep = RestoreReport{};
    guids.Rebuild(world);
    const auto note_once = [](std::vector<std::string>& list, const std::string& what) {
        if (std::find(list.begin(), list.end(), what) == list.end()) {
            AETHER_LOG_WARN("Save", "%s", what.c_str());
            list.push_back(what);
        }
    };
    for (const SavedEntity& saved : snapshot.entities) {
        const Entity e = guids.Find(world, saved.guid);
        if (e.IsNull()) {
            rep.missing.push_back(saved.guid);
            continue;
        }
        for (const SavedComponent& sc : saved.components) {
            const ComponentId cid = FindComponentIdByName(sc.name);
            if (cid == kInvalidComponentId || !GetComponentInfo(cid).reflected) {
                note_once(rep.unknown_components, sc.name);
                continue;
            }
            if (!world.HasComponentRaw(e, cid)) {
                Warn(rep.warnings, ToString(saved.guid) + " (" + saved.tag + "): the entity has no '" + sc.name + "' to restore into");
                continue;
            }
            const Json values = Json::parse(sc.data, nullptr, /*allow_exceptions=*/false);
            if (values.is_discarded() || !values.is_object()) {
                Warn(rep.warnings, ToString(saved.guid) + ": the saved '" + sc.name + "' isn't readable");
                continue;
            }
            const reflect::TypeInfo& type = *GetComponentInfo(cid).reflected;
            void* raw = world.GetComponentRaw(e, cid);
            Json merged = reflect::ToJson(type, raw); // the live values, with the saved ones laid over them
            for (auto it = values.begin(); it != values.end(); ++it) {
                if (it.key() != "$v" && !merged.contains(it.key())) {
                    note_once(rep.unknown_fields, sc.name + "." + it.key());
                    continue;
                }
                merged[it.key()] = it.value();
            }
            reflect::LoadReport lr;
            if (reflect::FromJson(type, raw, merged, &lr)) ++rep.applied;
            for (std::string& w : lr.warnings) rep.warnings.push_back(sc.name + ": " + w);
        }
    }
    for (const EntityGuid& guid : snapshot.destroyed) {
        const Entity e = guids.Find(world, guid);
        if (e.IsNull()) continue; // already gone
        if (lifecycle) lifecycle->Destroy(e);
        else world.DestroyEntity(e);
        guids.Remove(guid);
        ++rep.destroyed;
    }
    return rep.missing.empty() && rep.unknown_components.empty() && rep.unknown_fields.empty() && rep.warnings.empty();
}

} // namespace aether::save
