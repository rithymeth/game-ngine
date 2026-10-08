#pragma once

#include "aether/core/base.h"
#include "aether/ecs/archetype.h"
#include "aether/ecs/component.h"
#include "aether/ecs/entity.h"

#include <memory>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace aether {

// Owns every entity and archetype in a simulation. Systems are just plain
// functions that call World::ForEach/ForEachChunk over the component types
// they read and write; the World itself has no notion of "systems" or
// scheduling — that's the Job System's job, one job per chunk.
// A World is not internally synchronized. The caller must coordinate mutation
// with iteration and keep jobs on disjoint component storage when parallel.
// Component pointers from GetComponent/ForEach can become invalid when a
// structural edit moves or removes their row; reacquire after such edits.
class World {
public:
    World() = default;
    ~World() = default;

    World(const World&) = delete;
    World& operator=(const World&) = delete;

    // Safe: Archetypes are heap-allocated (owned via unique_ptr in
    // archetypes_) and don't move when the map moves, so EntityRecord's raw
    // Archetype* pointers stay valid across a World move.
    World(World&&) = default;
    World& operator=(World&&) = default;

    template <typename... Components>
    Entity CreateEntity(Components&&... components) {
        Entity entity = AllocateEntityId();
        ComponentMask mask = ComponentMaskOf<std::decay_t<Components>...>();
        Archetype& archetype = GetOrCreateArchetype(mask);

        Archetype::Location loc = archetype.AllocateSlot(entity);
        (ConstructInPlace(archetype, loc, std::forward<Components>(components)), ...);

        SetLocation(entity, &archetype, loc);
        return entity;
    }

    void DestroyEntity(Entity e) {
        EntityRecord& rec = GetRecord(e);
        Archetype* archetype = rec.archetype;
        Archetype::Location loc{rec.chunk_index, rec.row};

        Entity moved = archetype->RemoveEntity(loc);
        if (!moved.IsNull()) {
            SetLocation(moved, archetype, loc);
        }
        FreeEntityId(e);
    }

    template <typename T>
    T* GetComponent(Entity e) const {
        const EntityRecord& rec = GetRecord(e);
        void* base = rec.archetype->ComponentArray(rec.chunk_index, GetComponentId<T>());
        return base ? reinterpret_cast<T*>(static_cast<u8*>(base) + rec.row * sizeof(T)) : nullptr;
    }

    template <typename T>
    bool HasComponent(Entity e) const {
        return GetRecord(e).archetype->Has(GetComponentId<T>());
    }

    template <typename T>
    void AddComponent(Entity e, T value = T{}) {
        EntityRecord& rec = GetRecord(e);
        Archetype& old_archetype = *rec.archetype;
        Archetype::Location old_loc{rec.chunk_index, rec.row};

        if (old_archetype.Has(GetComponentId<T>())) {
            *GetComponent<T>(e) = std::move(value);
            return;
        }

        Archetype& new_archetype = GetOrCreateArchetype(old_archetype.Mask() | ComponentMaskOf<T>());
        Archetype::Location new_loc = new_archetype.AllocateSlot(e);
        new_archetype.AdoptFrom(old_archetype, old_loc, new_loc);
        ConstructInPlace(new_archetype, new_loc, std::move(value));

        MigrateEntity(e, old_archetype, old_loc, new_archetype, new_loc);
    }

    template <typename T>
    void RemoveComponent(Entity e) {
        EntityRecord& rec = GetRecord(e);
        Archetype& old_archetype = *rec.archetype;
        Archetype::Location old_loc{rec.chunk_index, rec.row};

        if (!old_archetype.Has(GetComponentId<T>())) {
            return;
        }

        Archetype& new_archetype = GetOrCreateArchetype(old_archetype.Mask() & ~ComponentMaskOf<T>());
        Archetype::Location new_loc = new_archetype.AllocateSlot(e);
        new_archetype.AdoptFrom(old_archetype, old_loc, new_loc);

        MigrateEntity(e, old_archetype, old_loc, new_archetype, new_loc);
    }

    // Calls func(count, Components*...) once per non-empty chunk across every
    // archetype that has at least all of Components. This is the unit of
    // work a System hands to the job system: one job per chunk.
    template <typename... Components, typename Func>
    void ForEachChunk(Func&& func) const {
        ComponentMask query_mask = ComponentMaskOf<Components...>();
        for (auto& [key, archetype] : archetypes_) {
            (void)key;
            if ((archetype->Mask() & query_mask) != query_mask) {
                continue;
            }
            for (usize c = 0; c < archetype->ChunkCount(); ++c) {
                u32 count = archetype->ChunkEntityCount(c);
                if (count == 0) {
                    continue;
                }
                func(count, static_cast<Components*>(archetype->ComponentArray(c, GetComponentId<Components>()))...);
            }
        }
    }

    // Scalar convenience wrapper over ForEachChunk for simple systems that
    // don't need to jobify per-chunk themselves.
    template <typename... Components, typename Func>
    void ForEach(Func&& func) const {
        ForEachChunk<Components...>([&](u32 count, Components*... arrays) {
            for (u32 i = 0; i < count; ++i) {
                func(arrays[i]...);
            }
        });
    }

    usize EntityCount() const { return records_.size() - free_indices_.size(); }

    // True if `e` refers to a live entity (not destroyed, not a stale handle
    // to a recycled slot, not kNullEntity).
    bool IsAlive(Entity e) const {
        return !e.IsNull() && e.index < records_.size() && records_[e.index].generation == e.generation &&
               records_[e.index].archetype != nullptr;
    }
    usize ArchetypeCount() const { return archetypes_.size(); }

    // Type-erased access, for code that must work over "whatever components
    // this entity happens to have" without knowing the types at compile
    // time — currently just the scene serializer. Prefer the templated
    // ForEach/ForEachChunk/GetComponent above for anything that knows its
    // component types.
    template <typename Func>
    void ForEachArchetype(Func&& func) const {
        for (auto& [key, archetype] : archetypes_) {
            (void)key;
            func(*archetype);
        }
    }

    // Allocates an entity with every component in `mask` default-constructed
    // (via each ComponentInfo::construct), for callers building an entity
    // from data rather than from a compile-time component list.
    Entity CreateEntityRaw(ComponentMask mask) {
        Entity entity = AllocateEntityId();
        Archetype& archetype = GetOrCreateArchetype(mask);
        Archetype::Location loc = archetype.AllocateSlot(entity);
        for (ComponentId id = 0; id < kMaxComponentTypes; ++id) {
            if (mask.test(id)) {
                const ComponentInfo& info = GetComponentInfo(id);
                void* base = archetype.ComponentArray(loc.chunk_index, id);
                info.construct(static_cast<u8*>(base) + loc.row * info.size);
            }
        }
        SetLocation(entity, &archetype, loc);
        return entity;
    }

    bool HasComponentRaw(Entity e, ComponentId id) const { return GetRecord(e).archetype->Has(id); }

    // Adds a default-constructed component by id (a no-op if the entity
    // already has one). The type-erased counterpart of AddComponent<T>, for
    // callers that only know the component at runtime — e.g. the editor's
    // "Add Component" menu.
    void AddComponentRaw(Entity e, ComponentId id) {
        EntityRecord& rec = GetRecord(e);
        Archetype& old_archetype = *rec.archetype;
        if (old_archetype.Has(id)) {
            return;
        }
        Archetype::Location old_loc{rec.chunk_index, rec.row};
        ComponentMask mask = old_archetype.Mask();
        mask.set(id);
        Archetype& new_archetype = GetOrCreateArchetype(mask);
        Archetype::Location new_loc = new_archetype.AllocateSlot(e);
        new_archetype.AdoptFrom(old_archetype, old_loc, new_loc);
        const ComponentInfo& info = GetComponentInfo(id);
        void* base = new_archetype.ComponentArray(new_loc.chunk_index, id);
        info.construct(static_cast<u8*>(base) + new_loc.row * info.size);
        MigrateEntity(e, old_archetype, old_loc, new_archetype, new_loc);
    }

    // Removes (destroys) a component by id; a no-op if the entity lacks it.
    void RemoveComponentRaw(Entity e, ComponentId id) {
        EntityRecord& rec = GetRecord(e);
        Archetype& old_archetype = *rec.archetype;
        if (!old_archetype.Has(id)) {
            return;
        }
        Archetype::Location old_loc{rec.chunk_index, rec.row};
        ComponentMask mask = old_archetype.Mask();
        mask.reset(id);
        Archetype& new_archetype = GetOrCreateArchetype(mask);
        Archetype::Location new_loc = new_archetype.AllocateSlot(e);
        new_archetype.AdoptFrom(old_archetype, old_loc, new_loc);
        MigrateEntity(e, old_archetype, old_loc, new_archetype, new_loc);
    }

    const void* GetComponentRaw(Entity e, ComponentId id) const {
        return const_cast<World*>(this)->GetComponentRaw(e, id);
    }

    void* GetComponentRaw(Entity e, ComponentId id) {
        const EntityRecord& rec = GetRecord(e);
        void* base = rec.archetype->ComponentArray(rec.chunk_index, id);
        if (!base) {
            return nullptr;
        }
        return static_cast<u8*>(base) + rec.row * GetComponentInfo(id).size;
    }

private:
    struct EntityRecord {
        u32 generation = 0;
        Archetype* archetype = nullptr;
        u32 chunk_index = 0;
        u32 row = 0;
    };

    template <typename T>
    static void ConstructInPlace(Archetype& archetype, Archetype::Location loc, T&& value) {
        using DecayedT = std::decay_t<T>;
        void* base = archetype.ComponentArray(loc.chunk_index, GetComponentId<DecayedT>());
        void* ptr = static_cast<u8*>(base) + loc.row * sizeof(DecayedT);
        new (ptr) DecayedT(std::forward<T>(value));
    }

    void MigrateEntity(Entity e, Archetype& old_archetype, Archetype::Location old_loc, Archetype& new_archetype,
                        Archetype::Location new_loc) {
        Entity moved = old_archetype.RemoveEntity(old_loc);
        if (!moved.IsNull()) {
            SetLocation(moved, &old_archetype, old_loc);
        }
        SetLocation(e, &new_archetype, new_loc);
    }

    Entity AllocateEntityId() {
        if (!free_indices_.empty()) {
            u32 index = free_indices_.back();
            free_indices_.pop_back();
            return Entity{index, records_[index].generation};
        }
        u32 index = static_cast<u32>(records_.size());
        records_.push_back(EntityRecord{});
        return Entity{index, records_[index].generation};
    }

    void FreeEntityId(Entity e) {
        EntityRecord& rec = records_[e.index];
        rec.generation++;
        rec.archetype = nullptr;
        free_indices_.push_back(e.index);
    }

    EntityRecord& GetRecord(Entity e) {
        AETHER_ASSERT(e.index < records_.size());
        AETHER_ASSERT(records_[e.index].generation == e.generation);
        return records_[e.index];
    }

    const EntityRecord& GetRecord(Entity e) const {
        AETHER_ASSERT(e.index < records_.size());
        AETHER_ASSERT(records_[e.index].generation == e.generation);
        return records_[e.index];
    }

    void SetLocation(Entity e, Archetype* archetype, Archetype::Location loc) {
        EntityRecord& rec = records_[e.index];
        rec.archetype = archetype;
        rec.chunk_index = loc.chunk_index;
        rec.row = loc.row;
    }

    Archetype& GetOrCreateArchetype(ComponentMask mask) {
        auto it = archetypes_.find(mask);
        if (it != archetypes_.end()) {
            return *it->second;
        }
        auto archetype = std::make_unique<Archetype>(mask);
        Archetype& ref = *archetype;
        archetypes_.emplace(mask, std::move(archetype));
        return ref;
    }

    std::vector<EntityRecord> records_;
    std::vector<u32> free_indices_;
    std::unordered_map<ComponentMask, std::unique_ptr<Archetype>> archetypes_; // std::hash<bitset> keyed
};

} // namespace aether
