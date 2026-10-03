#include "aether/memory/pool_allocator.h"

#include "aether/core/profiler.h"

namespace aether {

PoolAllocator::PoolAllocator(void* memory, usize size_bytes, usize block_size, usize block_alignment) {
    Init(memory, size_bytes, block_size, block_alignment);
}

void PoolAllocator::Init(void* memory, usize size_bytes, usize block_size, usize block_alignment) {
    AETHER_ASSERT(block_size >= sizeof(FreeNode));

    usize aligned_base = AlignUp(reinterpret_cast<usize>(memory), block_alignment);
    usize front_padding = aligned_base - reinterpret_cast<usize>(memory);
    usize aligned_block_size = AlignUp(block_size, block_alignment);

    base_ = reinterpret_cast<u8*>(aligned_base);
    block_size_ = aligned_block_size;
    block_count_ = (front_padding < size_bytes) ? (size_bytes - front_padding) / aligned_block_size : 0;
    free_count_ = block_count_;

    free_list_ = nullptr;
    for (usize i = block_count_; i > 0; --i) {
        auto* node = reinterpret_cast<FreeNode*>(base_ + (i - 1) * block_size_);
        node->next = free_list_;
        free_list_ = node;
    }
}

void* PoolAllocator::Allocate() {
    if (!free_list_) {
        return nullptr;
    }
    FreeNode* node = free_list_;
    free_list_ = node->next;
    --free_count_;
    if (category_) MemoryTracker::Get().Alloc(category_, block_size_);
    return node;
}

void PoolAllocator::Free(void* block) {
    if (!block) {
        return;
    }
    auto* node = static_cast<FreeNode*>(block);
    node->next = free_list_;
    free_list_ = node;
    ++free_count_;
    if (category_) MemoryTracker::Get().Free(category_, block_size_);
}

} // namespace aether
