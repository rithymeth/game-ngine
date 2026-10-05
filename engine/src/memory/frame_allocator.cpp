#include "aether/memory/frame_allocator.h"

#include "aether/core/profiler.h"

namespace aether {

FrameAllocator::FrameAllocator(usize buffer_size_bytes) {
    Init(buffer_size_bytes);
}

FrameAllocator::~FrameAllocator() {
    if (storage_) MemoryTracker::Get().Free("Frame allocator (reserved)", reserved_);
}

void FrameAllocator::Init(usize buffer_size_bytes) {
    if (storage_) MemoryTracker::Get().Free("Frame allocator (reserved)", reserved_);
    reserved_ = buffer_size_bytes * kBufferCount;
    storage_ = std::make_unique<u8[]>(reserved_);
    MemoryTracker::Get().Alloc("Frame allocator (reserved)", reserved_);
    for (usize i = 0; i < kBufferCount; ++i) {
        buffers_[i].Init(storage_.get() + i * buffer_size_bytes, buffer_size_bytes);
    }
    current_index_ = 0;
}

void* FrameAllocator::Allocate(usize size_bytes, usize alignment) {
    return Current().Allocate(size_bytes, alignment);
}

void FrameAllocator::Swap() {
    current_index_ = (current_index_ + 1) % kBufferCount;
    Current().Reset();
}

} // namespace aether
