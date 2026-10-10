#include "aether/gfx/buffer.h"

#include "aether/gfx/device.h"

#include <cstring>
#include <utility>

namespace aether::gfx {

Buffer::Buffer(Device& device, u64 size_bytes, BufferKind kind, bool allow_uav)
    : size_bytes_(size_bytes), kind_(kind) {
    if (size_bytes == 0) {
        throw std::invalid_argument("D3D12 buffer size must be greater than zero");
    }
    if (allow_uav && kind != BufferKind::Default) {
        throw std::invalid_argument("D3D12 unordered-access buffers must use the default heap");
    }

    D3D12_HEAP_PROPERTIES heap_props{};
    switch (kind) {
        case BufferKind::Upload: heap_props.Type = D3D12_HEAP_TYPE_UPLOAD; break;
        case BufferKind::Readback: heap_props.Type = D3D12_HEAP_TYPE_READBACK; break;
        case BufferKind::Default: heap_props.Type = D3D12_HEAP_TYPE_DEFAULT; break;
        default: throw std::invalid_argument("Invalid D3D12 buffer heap kind");
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
        D3D12_RANGE readback_range{0, static_cast<SIZE_T>(size_bytes)};
        AETHER_D3D_CHECK(resource_->Map(0, kind == BufferKind::Upload ? &no_read : &readback_range, &mapped_));
    }
}

Buffer::~Buffer() { Unmap(); }

Buffer::Buffer(Buffer&& other) noexcept
    : resource_(std::move(other.resource_)), size_bytes_(std::exchange(other.size_bytes_, 0)),
      mapped_(std::exchange(other.mapped_, nullptr)), kind_(other.kind_) {}

Buffer& Buffer::operator=(Buffer&& other) noexcept {
    if (this == &other) return *this;

    Unmap();
    resource_ = std::move(other.resource_);
    size_bytes_ = std::exchange(other.size_bytes_, 0);
    mapped_ = std::exchange(other.mapped_, nullptr);
    kind_ = other.kind_;
    return *this;
}

void Buffer::Unmap() noexcept {
    if (!resource_ || !mapped_) return;

    // Upload heaps may have CPU writes; readback heaps are CPU-read only.
    D3D12_RANGE written_range{0, kind_ == BufferKind::Upload ? static_cast<SIZE_T>(size_bytes_) : 0};
    resource_->Unmap(0, &written_range);
    mapped_ = nullptr;
}

void Buffer::Update(const void* data, u64 size_bytes, u64 offset) {
    if (kind_ != BufferKind::Upload || !mapped_) {
        AETHER_LOG_ERROR("D3D12", "Buffer::Update requires a mapped upload-heap buffer");
        return;
    }
    if (!data || offset > size_bytes_ || size_bytes > size_bytes_ - offset) {
        AETHER_LOG_ERROR("D3D12", "Buffer::Update range is null or outside the buffer bounds");
        return;
    }
    std::memcpy(static_cast<u8*>(mapped_) + offset, data, size_bytes);
}

void Buffer::Read(void* out_data, u64 size_bytes, u64 offset) const {
    if (kind_ != BufferKind::Readback || !mapped_) {
        AETHER_LOG_ERROR("D3D12", "Buffer::Read requires a mapped readback-heap buffer");
        return;
    }
    if (!out_data || offset > size_bytes_ || size_bytes > size_bytes_ - offset) {
        AETHER_LOG_ERROR("D3D12", "Buffer::Read range is null or outside the buffer bounds");
        return;
    }
    std::memcpy(out_data, static_cast<u8*>(mapped_) + offset, size_bytes);
}

} // namespace aether::gfx
