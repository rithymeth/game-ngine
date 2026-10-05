#pragma once

#include "aether/core/base.h"

namespace aether {

// Fixed-size-block allocator backed by an intrusive free list threaded through
// unused blocks. O(1) allocate/free, zero fragmentation, no metadata overhead
// per live allocation. Intended for ECS component pools and other
// fixed-stride entity data.
class PoolAllocator {
public:
    PoolAllocator() = default;
    PoolAllocator(void* memory, usize size_bytes, usize block_size, usize block_alignment = alignof(std::max_align_t));

    PoolAllocator(const PoolAllocator&) = delete;
    PoolAllocator& operator=(const PoolAllocator&) = delete;

    void Init(void* memory, usize size_bytes, usize block_size, usize block_alignment = alignof(std::max_align_t));

    void* Allocate();
    void Free(void* block);

    usize BlockSize() const { return block_size_; }
    usize BlockCount() const { return block_count_; }
    usize FreeCount() const { return free_count_; }
    // Reports blocks in use to the MemoryTracker under `category` (Phase 23; null: off).
    void SetMemoryCategory(const char* category) { category_ = category; }

private:
    struct FreeNode {
        FreeNode* next;
    };

    u8* base_ = nullptr;
    usize block_size_ = 0;
    usize block_count_ = 0;
    usize free_count_ = 0;
    FreeNode* free_list_ = nullptr;
    const char* category_ = nullptr;
};

} // namespace aether
