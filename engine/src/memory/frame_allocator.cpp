#include "aether/memory/frame_allocator.h"

#include "aether/core/profile.h"

namespace aether {

FrameAllocator::FrameAllocator(usize buffer_size_bytes) {
    Init(buffer_size_bytes);
}

FrameAllocator::~FrameAllocator() {
    prof::ReportMemory("Frame allocator", -static_cast<i64>(tracked_bytes_));
}

void FrameAllocator::Init(usize buffer_size_bytes) {
    prof::ReportMemory("Frame allocator", -static_cast<i64>(tracked_bytes_)); // a re-Init replaces the old storage
    tracked_bytes_ = buffer_size_bytes * kBufferCount;
    prof::ReportMemory("Frame allocator", static_cast<i64>(tracked_bytes_));
    storage_ = std::make_unique<u8[]>(buffer_size_bytes * kBufferCount);
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
