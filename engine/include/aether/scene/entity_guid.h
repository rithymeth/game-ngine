#pragma once

#include "aether/core/base.h"
#include "aether/ecs/world.h"
#include "aether/reflection/converters.h"
#include "aether/reflection/reflection.h"

#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace aether {

// A permanent identity for an entity (Phase 7, docs/design/PHASE_SPECS.md
// §7.1). An `Entity` is an index + generation: right for runtime lookups, but
// it changes whenever an entity is destroyed and recreated (undo, reloading a
// scene, instantiating a prefab), so it can't be saved or used to refer to an
// entity across those. An EntityGuid is a random (version 4) UUID created
// once and saved with the entity; references between entities in saved data
// use it, and GuidIndex turns it back into the current Entity.
struct EntityGuid {
    u64 hi = 0;
    u64 lo = 0;

    bool IsNull() const { return hi == 0 && lo == 0; }
    bool operator==(const EntityGuid& o) const { return hi == o.hi && lo == o.lo; }
    bool operator!=(const EntityGuid& o) const { return !(*this == o); }
};

// A new random version 4 UUID. Thread-safe.
EntityGuid NewEntityGuid();

// Canonical text form: "5c0a9e2e-7b1d-4c3a-9f10-2a6b8e4d1f77" (lowercase).
std::string ToString(const EntityGuid& guid);
// Accepts the canonical form in either case. Returns false (leaving `out`
// unchanged) for anything else.
bool ParseEntityGuid(std::string_view text, EntityGuid& out);

// Gives an entity its EntityGuid. Added to every editor-created entity.
struct IdComponent {
    EntityGuid guid;
};

// Maps EntityGuids to the entities that currently carry them. Not kept up to
// date automatically: code that creates or destroys identified entities calls
// Add/Remove, or calls Rebuild after bulk changes such as loading a scene.
class GuidIndex {
public:
    // Re-indexes every entity in `world` that has an IdComponent. Returns the
    // entities whose GUID was already taken by an earlier entity (e.g. the
    // same scene loaded twice); those aren't indexed, and the caller decides
    // whether to give them new GUIDs (see RegenerateGuid).
    std::vector<Entity> Rebuild(World& world);

    void Add(const EntityGuid& guid, Entity entity) { by_guid_[guid] = entity; }
    void Remove(const EntityGuid& guid) { by_guid_.erase(guid); }
    void Clear() { by_guid_.clear(); }
    usize Size() const { return by_guid_.size(); }

    // The live entity carrying `guid`, or kNullEntity if there is none. Stale
    // entries (the entity was destroyed, or its IdComponent changed since it
    // was indexed) are reported as kNullEntity rather than trusted.
    Entity Find(const World& world, const EntityGuid& guid) const;

private:
    struct Hash {
        usize operator()(const EntityGuid& g) const noexcept {
            return static_cast<usize>(g.hi ^ (g.lo * 0x9e3779b97f4a7c15ull));
        }
    };
    std::unordered_map<EntityGuid, Entity, Hash> by_guid_;
};

// Returns the entity's GUID, first giving it an IdComponent with a new GUID
// (and adding it to `index`, if given) when it has none.
EntityGuid EnsureGuid(World& world, Entity entity, GuidIndex* index = nullptr);

// Replaces the entity's GUID with a new one (updating `index`, if given).
// Used for duplicates: copy/paste, duplicate, or a scene loaded twice.
EntityGuid RegenerateGuid(World& world, Entity entity, GuidIndex* index = nullptr);

namespace detail {
nlohmann::json EntityGuidToJson(const void* object);
bool EntityGuidFromJson(const nlohmann::json& data, void* object);
} // namespace detail

} // namespace aether

// EntityGuid is reflected by hand (not with AETHER_REFLECT) so that building
// its TypeInfo also installs its JSON converter: it's saved as its canonical
// string, not as {"hi": ..., "lo": ...}.
template <>
struct aether::reflect::Reflector<aether::EntityGuid> {
    static const ::aether::reflect::TypeInfo& Get() {
        static const ::aether::reflect::TypeInfo info = [] {
            using namespace ::aether::reflect;
            using Self = aether::EntityGuid;
            TypeInfo type_info = detail::MakeTypeInfo<Self>("EntityGuid", TypeKind::Struct, 1);
            detail::AddMembers(type_info, {AETHER_FIELD(hi), AETHER_FIELD(lo)});
            return type_info;
        }();
        static const bool registered = [] {
            ::aether::reflect::TypeRegistry::Register(info);
            ::aether::reflect::RegisterJsonConverter(info, &aether::detail::EntityGuidToJson,
                                                     &aether::detail::EntityGuidFromJson);
            return true;
        }();
        (void)registered;
        return info;
    }
    static inline const bool kStaticRegistration = (Get(), true);
};

AETHER_REFLECT(aether::IdComponent, 1, AETHER_FIELD(guid, Field_ReadOnly, {.tooltip = "Permanent entity ID"}))

template <>
struct std::hash<aether::EntityGuid> {
    size_t operator()(const aether::EntityGuid& g) const noexcept {
        return static_cast<size_t>(g.hi ^ (g.lo * 0x9e3779b97f4a7c15ull));
    }
};
