#include "aether/gfx/buffer.h"

#include "aether/gfx/device.h"

#include <cstring>

namespace aether::gfx {

Buffer::Buffer(Device& device, u64 size_bytes, BufferKind kind, bool allow_uav) : size_bytes_(size_bytes) {
    D3D12_HEAP_PROPERTIES heap_props{};
    switch (kind) {
        case BufferKind::Upload: heap_props.Type = D3D12_HEAP_TYPE_UPLOAD; break;
        case BufferKind::Readback: heap_props.Type = D3D12_HEAP_TYPE_READBACK; break;
        case BufferKind::Default: heap_props.Type = D3D12_HEAP_TYPE_DEFAULT; break;
    }

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size_bytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = allow_uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;

    D3D12_RESOURCE_STATES initial_state = D3D12_RESOURCE_STATE_COMMON;
    if (kind == BufferKind::Upload) {
        initial_state = D3D12_RESOURCE_STATE_GENERIC_READ;
    } else if (kind == BufferKind::Readback) {
        initial_state = D3D12_RESOURCE_STATE_COPY_DEST;
    }

    AETHER_D3D_CHECK(device.Handle()->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &desc, initial_state,
                                                               nullptr, IID_PPV_ARGS(&resource_)));

    if (kind == BufferKind::Upload || kind == BufferKind::Readback) {
        D3D12_RANGE no_read{0, 0};
        AETHER_D3D_CHECK(resource_->Map(0, kind == BufferKind::Upload ? &no_read : nullptr, &mapped_));
    }
}

void Buffer::Update(const void* data, u64 size_bytes, u64 offset) {
    AETHER_ASSERT(mapped_ != nullptr);
    std::memcpy(static_cast<u8*>(mapped_) + offset, data, size_bytes);
}

void Buffer::Read(void* out_data, u64 size_bytes, u64 offset) const {
    AETHER_ASSERT(mapped_ != nullptr);
    std::memcpy(out_data, static_cast<u8*>(mapped_) + offset, size_bytes);
}

} // namespace aether::gfx
