#pragma once

#include "aether/gfx/d3d12_common.h"

namespace aether::gfx {

class Device;

enum class BufferKind { Upload, Default, Readback };

// A generic GPU buffer. Upload/Readback-heap buffers stay persistently
// mapped, meant for small CPU<->GPU data (constants, instance data at demo
// scale) — real streaming of large per-frame data through a ring buffer is
// future work. Default-heap buffers are GPU-only and can optionally get a
// UAV, for compute-writable resources like an indirect argument buffer.
class Buffer {
public:
    Buffer(Device& device, u64 size_bytes, BufferKind kind, bool allow_uav = false);

    ID3D12Resource* Handle() const { return resource_.Get(); }
    D3D12_GPU_VIRTUAL_ADDRESS GPUAddress() const { return resource_->GetGPUVirtualAddress(); }
    u64 Size() const { return size_bytes_; }

    // Upload buffers: memcpy into the persistently-mapped pointer.
    void Update(const void* data, u64 size_bytes, u64 offset = 0);

    // Readback buffers: memcpy out of the persistently-mapped pointer, after
    // the caller has waited on the fence covering the GPU-side copy into it.
    void Read(void* out_data, u64 size_bytes, u64 offset = 0) const;

private:
    ComPtr<ID3D12Resource> resource_;
    u64 size_bytes_ = 0;
    void* mapped_ = nullptr;
};

} // namespace aether::gfx
