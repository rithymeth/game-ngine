#include "aether/memory/linear_allocator.h"

#include <cstring>
#include <utility>

namespace aether {

LinearAllocator::LinearAllocator(void* memory, usize size_bytes) {
    Init(memory, size_bytes);
}

LinearAllocator::LinearAllocator(LinearAllocator&& other) noexcept
    : base_(other.base_), capacity_(other.capacity_), offset_(other.offset_) {
    other.base_ = nullptr;
    other.capacity_ = 0;
    other.offset_ = 0;
}

LinearAllocator& LinearAllocator::operator=(LinearAllocator&& other) noexcept {
    if (this != &other) {
        base_ = std::exchange(other.base_, nullptr);
        capacity_ = std::exchange(other.capacity_, 0);
        offset_ = std::exchange(other.offset_, 0);
    }
    return *this;
}

void LinearAllocator::Init(void* memory, usize size_bytes) {
    base_ = static_cast<u8*>(memory);
    capacity_ = size_bytes;
    offset_ = 0;
}

void* LinearAllocator::Allocate(usize size_bytes, usize alignment) {
    AETHER_ASSERT(base_ != nullptr);
    AETHER_ASSERT((alignment & (alignment - 1)) == 0);

    usize current = reinterpret_cast<usize>(base_) + offset_;
    usize aligned = AlignUp(current, alignment);
    usize padding = aligned - current;

    if (offset_ + padding + size_bytes > capacity_) {
        return nullptr;
    }

    offset_ += padding + size_bytes;
    return base_ + offset_ - size_bytes;
}

void LinearAllocator::Reset() {
    offset_ = 0;
}

} // namespace aether
