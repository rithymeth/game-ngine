#pragma once

#include "aether/core/base.h"
#include "aether/memory/linear_allocator.h"

#include <memory>

namespace aether {

// Double-buffered ring of linear allocators for transient per-frame data
// (temporary strings, command lists, scratch buffers). Call Swap() once per
// frame after the render thread has finished consuming the previous frame's
// buffer; the buffer two frames back is reset and reused, so data written
// this frame remains valid until the *next* Swap().
class FrameAllocator {
public:
    FrameAllocator() = default;
    explicit FrameAllocator(usize buffer_size_bytes);
    ~FrameAllocator();

    FrameAllocator(const FrameAllocator&) = delete;
    FrameAllocator& operator=(const FrameAllocator&) = delete;

    void Init(usize buffer_size_bytes);

    void* Allocate(usize size_bytes, usize alignment = alignof(std::max_align_t));

    template <typename T, typename... Args>
    T* AllocateObject(Args&&... args) {
        return Current().AllocateObject<T>(static_cast<Args&&>(args)...);
    }

    // Advances to the next buffer and resets it, ready for the new frame.
    void Swap();

    LinearAllocator& Current() { return buffers_[current_index_]; }
    const LinearAllocator& Current() const { return buffers_[current_index_]; }

private:
    static constexpr usize kBufferCount = 2;

    std::unique_ptr<u8[]> storage_;
    LinearAllocator buffers_[kBufferCount];
    usize current_index_ = 0;
    usize reserved_ = 0;
};

} // namespace aether
