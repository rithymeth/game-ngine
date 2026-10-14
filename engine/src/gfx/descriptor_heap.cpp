#include "aether/gfx/descriptor_heap.h"

#include "aether/gfx/device.h"

namespace aether::gfx {

DescriptorHeap::DescriptorHeap(Device& device, D3D12_DESCRIPTOR_HEAP_TYPE type, u32 capacity, bool shader_visible)
    : capacity_(capacity), allocated_(capacity, 0), shader_visible_(shader_visible) {
    // Free() must not need to grow its bookkeeping after the native heap has
    // been created. At most one free-list entry can exist per descriptor.
    free_list_.reserve(capacity_);

    D3D12_DESCRIPTOR_HEAP_DESC desc{};
    desc.Type = type;
    desc.NumDescriptors = capacity;
    desc.Flags = shader_visible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    AETHER_D3D_CHECK(device.Handle()->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&heap_)));
    descriptor_size_ = device.Handle()->GetDescriptorHandleIncrementSize(type);
}

u32 DescriptorHeap::Allocate() {
    std::lock_guard<std::mutex> lock(allocation_mutex_);
    if (!free_list_.empty()) {
        u32 index = free_list_.back();
        free_list_.pop_back();
        allocated_[index] = 1;
        return index;
    }
    if (next_free_ >= capacity_) {
        AETHER_LOG_ERROR("DescriptorHeap", "Exhausted (capacity=%u)", capacity_);
        return kInvalidIndex;
    }
    const u32 index = next_free_++;
    allocated_[index] = 1;
    return index;
}

void DescriptorHeap::Free(u32 index) {
    std::lock_guard<std::mutex> lock(allocation_mutex_);
    if (index >= next_free_ || index >= capacity_ || allocated_[index] == 0) {
        AETHER_LOG_ERROR("DescriptorHeap", "Cannot free invalid or already-free descriptor index %u", index);
        return;
    }
    free_list_.push_back(index);
    allocated_[index] = 0;
}

bool DescriptorHeap::CanAllocate() const {
    std::lock_guard<std::mutex> lock(allocation_mutex_);
    return !free_list_.empty() || next_free_ < capacity_;
}

D3D12_CPU_DESCRIPTOR_HANDLE DescriptorHeap::CPUHandle(u32 index) const {
    if (!heap_ || index >= capacity_) {
        AETHER_LOG_ERROR("DescriptorHeap", "Cannot resolve CPU handle for invalid descriptor index %u", index);
        return {};
    }
    D3D12_CPU_DESCRIPTOR_HANDLE handle = heap_->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(index) * descriptor_size_;
    return handle;
}

D3D12_GPU_DESCRIPTOR_HANDLE DescriptorHeap::GPUHandle(u32 index) const {
    if (!shader_visible_ || !heap_ || index >= capacity_) {
        AETHER_LOG_ERROR("DescriptorHeap", "Cannot resolve GPU handle for invalid or non-visible descriptor index %u",
                         index);
        return {};
    }
    D3D12_GPU_DESCRIPTOR_HANDLE handle = heap_->GetGPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<UINT64>(index) * descriptor_size_;
    return handle;
}

} // namespace aether::gfx
