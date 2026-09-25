#include "aether/ecs/archetype.h"

#include <algorithm>

namespace aether {

Archetype::Archetype(ComponentMask mask) : mask_(mask) {
    index_by_id_.fill(-1);

    for (ComponentId id = 0; id < kMaxComponentTypes; ++id) {
        if (mask.test(id)) {
            component_ids_.push_back(id);
        }
    }

    usize per_entity_size = sizeof(Entity);
    for (ComponentId id : component_ids_) {
        per_entity_size += GetComponentInfo(id).size;
    }
    capacity_ = static_cast<u32>(std::max<usize>(1, kChunkSize / std::max<usize>(1, per_entity_size)));

    // Lay out entity array + each component array back to back with proper
    // alignment padding, shrinking capacity_ until it actually fits in one
    // chunk (the size-only estimate above ignores padding).
    for (;;) {
        usize offset = AlignUp(0, alignof(Entity));
        entity_array_offset_ = offset;
        offset += sizeof(Entity) * capacity_;

        component_offsets_.assign(component_ids_.size(), 0);
        for (usize i = 0; i < component_ids_.size(); ++i) {
            const ComponentInfo& info = GetComponentInfo(component_ids_[i]);
            offset = AlignUp(offset, info.alignment);
            component_offsets_[i] = offset;
            offset += info.size * capacity_;
        }

        if (offset <= kChunkSize || capacity_ <= 1) {
            break;
        }
        --capacity_;
    }

    for (usize i = 0; i < component_ids_.size(); ++i) {
        index_by_id_[component_ids_[i]] = static_cast<i32>(i);
    }
}

Archetype::~Archetype() {
    for (auto& chunk : chunks_) {
        for (usize i = 0; i < component_ids_.size(); ++i) {
            const ComponentInfo& info = GetComponentInfo(component_ids_[i]);
            u8* array_base = chunk->buffer.get() + component_offsets_[i];
            for (u32 row = 0; row < chunk->count; ++row) {
                info.destruct(array_base + row * info.size);
            }
        }
    }
}

void* Archetype::ComponentArray(usize chunk_index, ComponentId id) const {
    i32 idx = index_by_id_[id];
    if (idx < 0) {
        return nullptr;
    }
    return chunks_[chunk_index]->buffer.get() + component_offsets_[static_cast<usize>(idx)];
}

Entity* Archetype::EntityArray(usize chunk_index) const {
    return reinterpret_cast<Entity*>(chunks_[chunk_index]->buffer.get() + entity_array_offset_);
}

Archetype::Chunk& Archetype::EnsureSpace() {
    if (chunks_.empty() || chunks_.back()->count == capacity_) {
        auto chunk = std::make_unique<Chunk>();
        chunk->buffer = std::make_unique<u8[]>(kChunkSize);
        chunks_.push_back(std::move(chunk));
    }
    return *chunks_.back();
}

Archetype::Location Archetype::AllocateSlot(Entity entity) {
    Chunk& chunk = EnsureSpace();
    u32 row = chunk.count++;
    EntityArray(chunks_.size() - 1)[row] = entity;
    return {static_cast<u32>(chunks_.size() - 1), row};
}

Entity Archetype::RemoveEntity(Location loc) {
    Chunk& chunk = *chunks_[loc.chunk_index];
    u32 last_chunk_index = static_cast<u32>(chunks_.size() - 1);
    Chunk& last_chunk = *chunks_[last_chunk_index];
    u32 last_row = last_chunk.count - 1;

    bool is_last_slot = (loc.chunk_index == last_chunk_index) && (loc.row == last_row);

    for (usize i = 0; i < component_ids_.size(); ++i) {
        const ComponentInfo& info = GetComponentInfo(component_ids_[i]);
        u8* dst = chunk.buffer.get() + component_offsets_[i] + loc.row * info.size;
        info.destruct(dst);

        if (!is_last_slot) {
            u8* src = last_chunk.buffer.get() + component_offsets_[i] + last_row * info.size;
            info.move(dst, src);
            info.destruct(src);
        }
    }

    Entity moved_entity = kNullEntity;
    if (!is_last_slot) {
        Entity* last_entities = EntityArray(last_chunk_index);
        moved_entity = last_entities[last_row];
        EntityArray(loc.chunk_index)[loc.row] = moved_entity;
    }

    last_chunk.count--;
    if (last_chunk.count == 0 && chunks_.size() > 1) {
        chunks_.pop_back();
    }

    return moved_entity;
}

void Archetype::AdoptFrom(const Archetype& src, Location src_loc, Location dst_loc) {
    Chunk& dst_chunk = *chunks_[dst_loc.chunk_index];
    const Chunk& src_chunk = *src.chunks_[src_loc.chunk_index];

    for (usize i = 0; i < component_ids_.size(); ++i) {
        ComponentId id = component_ids_[i];
        i32 src_idx = src.index_by_id_[id];
        if (src_idx < 0) {
            continue; // component unique to `this`; caller constructs it
        }

        const ComponentInfo& info = GetComponentInfo(id);
        u8* dst_ptr = dst_chunk.buffer.get() + component_offsets_[i] + dst_loc.row * info.size;
        u8* src_ptr = src_chunk.buffer.get() + src.component_offsets_[static_cast<usize>(src_idx)] +
                      src_loc.row * info.size;
        info.move(dst_ptr, src_ptr);
    }
}

} // namespace aether
