#pragma once

#include "aether/gfx/d3d12_common.h"

#include <vector>

namespace aether::gfx {

class Device;

// A fixed-capacity descriptor heap with a simple free-list index allocator.
// Used for regular (non-shader-visible) RTV/DSV heaps and, notably, as the
// single shader-visible CBV/SRV/UAV heap backing the bindless texture table:
// in that case a texture's heap index IS its identity in shaders — the
// integer a root constant carries to index `Texture2D g_Textures[] :
// register(t0, space1)` — so allocation here doubles as bindless handle
// assignment. There's no per-draw descriptor binding to hide: the whole heap
// is bound once, and every draw just picks an index.
class DescriptorHeap {
public:
    static constexpr u32 kInvalidIndex = static_cast<u32>(-1);

    DescriptorHeap(Device& device, D3D12_DESCRIPTOR_HEAP_TYPE type, u32 capacity, bool shader_visible);

    u32 Allocate();
    void Free(u32 index);
    bool CanAllocate() const { return !free_list_.empty() || next_free_ < capacity_; }

    D3D12_CPU_DESCRIPTOR_HANDLE CPUHandle(u32 index) const;
    D3D12_GPU_DESCRIPTOR_HANDLE GPUHandle(u32 index) const;

    ID3D12DescriptorHeap* Heap() const { return heap_.Get(); }
    u32 Capacity() const { return capacity_; }

private:
    ComPtr<ID3D12DescriptorHeap> heap_;
    u32 descriptor_size_ = 0;
    u32 capacity_ = 0;
    u32 next_free_ = 0;
    std::vector<u32> free_list_;
    bool shader_visible_ = false;
};

} // namespace aether::gfx
