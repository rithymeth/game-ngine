#include "aether/gfx/descriptor_heap.h"

#include "aether/gfx/device.h"

namespace aether::gfx {

DescriptorHeap::DescriptorHeap(Device& device, D3D12_DESCRIPTOR_HEAP_TYPE type, u32 capacity, bool shader_visible)
    : capacity_(capacity), shader_visible_(shader_visible) {
    D3D12_DESCRIPTOR_HEAP_DESC desc{};
    desc.Type = type;
    desc.NumDescriptors = capacity;
    desc.Flags = shader_visible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    AETHER_D3D_CHECK(device.Handle()->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&heap_)));
    descriptor_size_ = device.Handle()->GetDescriptorHandleIncrementSize(type);
}

u32 DescriptorHeap::Allocate() {
    if (!free_list_.empty()) {
        u32 index = free_list_.back();
        free_list_.pop_back();
        return index;
    }
    if (next_free_ >= capacity_) {
        AETHER_LOG_ERROR("DescriptorHeap", "Exhausted (capacity=%u)", capacity_);
        return kInvalidIndex;
    }
    return next_free_++;
}

void DescriptorHeap::Free(u32 index) {
    free_list_.push_back(index);
}

D3D12_CPU_DESCRIPTOR_HANDLE DescriptorHeap::CPUHandle(u32 index) const {
    D3D12_CPU_DESCRIPTOR_HANDLE handle = heap_->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(index) * descriptor_size_;
    return handle;
}

D3D12_GPU_DESCRIPTOR_HANDLE DescriptorHeap::GPUHandle(u32 index) const {
    AETHER_ASSERT(shader_visible_);
    D3D12_GPU_DESCRIPTOR_HANDLE handle = heap_->GetGPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<UINT64>(index) * descriptor_size_;
    return handle;
}

} // namespace aether::gfx
