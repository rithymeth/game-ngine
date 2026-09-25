#pragma once

#include "aether/core/base.h"

namespace aether {

// Bump-pointer allocator over a fixed block of memory. Individual allocations
// cannot be freed; the whole arena is reclaimed at once via Reset().
// Used both as the long-lived Global Arena and as the backing storage for the
// per-frame Frame Allocator.
class LinearAllocator {
public:
    LinearAllocator() = default;
    LinearAllocator(void* memory, usize size_bytes);
    ~LinearAllocator() = default;

    LinearAllocator(const LinearAllocator&) = delete;
    LinearAllocator& operator=(const LinearAllocator&) = delete;

    LinearAllocator(LinearAllocator&& other) noexcept;
    LinearAllocator& operator=(LinearAllocator&& other) noexcept;

    void Init(void* memory, usize size_bytes);

    // Allocates size_bytes aligned to `alignment` (must be a power of two).
    // Returns nullptr if the arena is out of space.
    void* Allocate(usize size_bytes, usize alignment = alignof(std::max_align_t));

    template <typename T, typename... Args>
    T* AllocateObject(Args&&... args) {
        void* mem = Allocate(sizeof(T), alignof(T));
        if (!mem) {
            return nullptr;
        }
        return new (mem) T(static_cast<Args&&>(args)...);
    }

    // Reclaims all memory in the arena. Does not run destructors; the arena is
    // intended for POD/transient data or externally-managed lifetimes.
    void Reset();

    usize Capacity() const { return capacity_; }
    usize Used() const { return offset_; }
    usize Remaining() const { return capacity_ - offset_; }
    void* Base() const { return base_; }

private:
    u8* base_ = nullptr;
    usize capacity_ = 0;
    usize offset_ = 0;
};

} // namespace aether
