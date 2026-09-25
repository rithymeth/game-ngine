#pragma once

#include "aether/core/base.h"
#include "aether/ecs/component.h"
#include "aether/ecs/entity.h"

#include <array>
#include <memory>
#include <vector>

namespace aether {

// Entities with an identical component signature live together in
// contiguous, chunked SoA storage: one Archetype per unique combination of
// component types, one Chunk per kChunkSize block of entities. Scanning a
// chunk's component array is a linear read with no branching or pointer
// chasing — this is what lets a System iterate millions of components with
// the CPU prefetcher doing most of the work, and what makes chunks a natural
// unit of parallel work for the job system.
class Archetype {
public:
    static constexpr usize kChunkSize = 16 * 1024;

    struct Location {
        u32 chunk_index;
        u32 row;
    };

    explicit Archetype(ComponentMask mask);
    ~Archetype();

    Archetype(const Archetype&) = delete;
    Archetype& operator=(const Archetype&) = delete;

    const ComponentMask& Mask() const { return mask_; }
    bool Has(ComponentId id) const { return mask_.test(id); }

    usize ChunkCount() const { return chunks_.size(); }
    u32 ChunkEntityCount(usize chunk_index) const { return chunks_[chunk_index]->count; }
    u32 EntitiesPerChunk() const { return capacity_; }

    // Base pointer of `id`'s component array within a chunk, or nullptr if
    // this archetype doesn't carry that component.
    void* ComponentArray(usize chunk_index, ComponentId id) const;
    Entity* EntityArray(usize chunk_index) const;

    // Reserves a slot for `entity` (allocating a new chunk if the last one
    // is full) and writes its id into the entity array. Every component slot
    // at the returned location is left uninitialized — the caller (World)
    // is responsible for constructing or move-adopting every one of them
    // before the slot is read.
    Location AllocateSlot(Entity entity);

    // Destructs every component at `loc`, then swaps the archetype's last
    // entity into that slot to keep storage dense. Returns the entity that
    // got moved there (kNullEntity if `loc` held the last entity already),
    // so the caller can fix up its record.
    Entity RemoveEntity(Location loc);

    // For every component `this` archetype shares with `src`, move-
    // constructs it from `src`'s slot into `this`'s slot at `dst_loc`.
    // Components unique to `this` (e.g. one just being added) are left
    // uninitialized for the caller to construct; components unique to `src`
    // (e.g. one being removed) are left untouched in `src` for its own
    // RemoveEntity() call to destruct. Used when an entity migrates to a
    // different archetype after AddComponent/RemoveComponent.
    void AdoptFrom(const Archetype& src, Location src_loc, Location dst_loc);

private:
    struct Chunk {
        std::unique_ptr<u8[]> buffer;
        u32 count = 0;
    };

    Chunk& EnsureSpace();

    ComponentMask mask_;
    std::vector<ComponentId> component_ids_;               // sorted ascending
    std::vector<usize> component_offsets_;                 // parallel to component_ids_: offset within a chunk
    std::array<i32, kMaxComponentTypes> index_by_id_{};     // ComponentId -> index into component_ids_, or -1
    usize entity_array_offset_ = 0;
    u32 capacity_ = 0; // entities per chunk
    std::vector<std::unique_ptr<Chunk>> chunks_;
};

} // namespace aether
