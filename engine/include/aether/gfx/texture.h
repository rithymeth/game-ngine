#pragma once

#include "aether/gfx/d3d12_common.h"

namespace aether::gfx {

class Device;
class DescriptorHeap;

// A 2D texture with an SRV allocated into a caller-provided (shader-visible)
// bindless descriptor heap. BindlessIndex() is the value shaders receive —
// via a root constant — to index `Texture2D g_Textures[] : register(t0,
// space1)` and select this texture. That index *is* the bindless handle:
// there's no per-draw descriptor table rebinding, the whole heap is bound
// once and every draw just picks an element.
class Texture {
public:
    // Creates a DEFAULT-heap 2D RGBA8 texture and uploads `pixels` (tightly
    // packed, width * height * 4 bytes) via a temporary UPLOAD-heap staging
    // buffer, recorded onto `upload_cmd` (the caller submits it and waits on
    // the fence before the texture is sampled). Keep the upload staging buffer
    // until that fence completes, then call ReleaseUploadStaging() so it does
    // not occupy GPU memory for the texture's full lifetime.
    Texture(Device& device, DescriptorHeap& bindless_heap, ID3D12GraphicsCommandList* upload_cmd, u32 width,
            u32 height, const u8* pixels);

    // The same for a block-compressed texture: `format` is a BC DXGI format, and `mips` hold each level's blocks (row pitch `row_bytes[i]`
    // bytes of blocks, `block_rows[i]` rows), largest first. The texture gets that many mip levels.
    struct MipData {
        const u8* data = nullptr;
        u32 width = 0, height = 0;
        u32 row_bytes = 0;   // one row of blocks, tightly packed
        u32 block_rows = 0;
    };
    Texture(Device& device, DescriptorHeap& bindless_heap, ID3D12GraphicsCommandList* upload_cmd, DXGI_FORMAT format, u32 width, u32 height,
            const MipData* mips, u32 mip_count);

    u32 BindlessIndex() const { return bindless_index_; }
    ID3D12Resource* Handle() const { return resource_.Get(); }

    // Call only after the command list containing this texture's upload has
    // completed on the GPU. Safe to call more than once.
    void ReleaseUploadStaging() { upload_staging_.Reset(); }

private:
    ComPtr<ID3D12Resource> resource_;
    ComPtr<ID3D12Resource> upload_staging_;
    u32 bindless_index_ = static_cast<u32>(-1);
};

} // namespace aether::gfx
