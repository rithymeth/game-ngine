#include "aether/gfx/texture.h"

#include "aether/gfx/command_list.h"
#include "aether/gfx/descriptor_heap.h"
#include "aether/gfx/device.h"
#include "aether/core/log.h"

#include <cstring>
#include <stdexcept>

namespace aether::gfx {

Texture::Texture(Device& device, DescriptorHeap& bindless_heap, ID3D12GraphicsCommandList* upload_cmd, u32 width,
                  u32 height, const u8* pixels) {
    if (!bindless_heap.CanAllocate()) {
        AETHER_LOG_ERROR("Texture", "Cannot create texture: bindless descriptor heap is exhausted");
        throw std::length_error("Cannot create texture: bindless descriptor heap is exhausted");
    }

    D3D12_HEAP_PROPERTIES default_heap{};
    default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC tex_desc{};
    tex_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    tex_desc.Width = width;
    tex_desc.Height = height;
    tex_desc.DepthOrArraySize = 1;
    tex_desc.MipLevels = 1;
    tex_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    tex_desc.SampleDesc.Count = 1;
    tex_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    AETHER_D3D_CHECK(device.Handle()->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &tex_desc,
                                                               D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                               IID_PPV_ARGS(&resource_)));

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT num_rows = 0;
    UINT64 row_size = 0;
    UINT64 total_bytes = 0;
    device.Handle()->GetCopyableFootprints(&tex_desc, 0, 1, 0, &footprint, &num_rows, &row_size, &total_bytes);

    D3D12_HEAP_PROPERTIES upload_heap{};
    upload_heap.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC upload_desc{};
    upload_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    upload_desc.Width = total_bytes;
    upload_desc.Height = 1;
    upload_desc.DepthOrArraySize = 1;
    upload_desc.MipLevels = 1;
    upload_desc.Format = DXGI_FORMAT_UNKNOWN;
    upload_desc.SampleDesc.Count = 1;
    upload_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    AETHER_D3D_CHECK(device.Handle()->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &upload_desc,
                                                               D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                               IID_PPV_ARGS(&upload_staging_)));

    u8* mapped = nullptr;
    D3D12_RANGE no_read{0, 0};
    AETHER_D3D_CHECK(upload_staging_->Map(0, &no_read, reinterpret_cast<void**>(&mapped)));
    for (u32 row = 0; row < height; ++row) {
        std::memcpy(mapped + footprint.Offset + static_cast<u64>(row) * footprint.Footprint.RowPitch,
                    pixels + static_cast<u64>(row) * width * 4, static_cast<usize>(width) * 4);
    }
    upload_staging_->Unmap(0, nullptr);

    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = resource_.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = upload_staging_.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint = footprint;

    upload_cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    D3D12_RESOURCE_BARRIER barrier =
        TransitionBarrier(resource_.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    upload_cmd->ResourceBarrier(1, &barrier);

    bindless_index_ = bindless_heap.Allocate();
    if (bindless_index_ == DescriptorHeap::kInvalidIndex) {
        AETHER_LOG_ERROR("Texture", "Cannot create texture: bindless descriptor allocation failed");
        throw std::length_error("Cannot create texture: bindless descriptor allocation failed");
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc{};
    srv_desc.Format = tex_desc.Format;
    srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv_desc.Texture2D.MipLevels = 1;
    device.Handle()->CreateShaderResourceView(resource_.Get(), &srv_desc, bindless_heap.CPUHandle(bindless_index_));
}

} // namespace aether::gfx
