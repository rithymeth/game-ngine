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
    // the fence before the texture is sampled). The staging buffer is kept
    // alive for this Texture's lifetime rather than freed once the copy
    // completes — acceptable at demo scale; a real asset pipeline would pool
    // and recycle staging buffers instead.
    Texture(Device& device, DescriptorHeap& bindless_heap, ID3D12GraphicsCommandList* upload_cmd, u32 width,
            u32 height, const u8* pixels);

    u32 BindlessIndex() const { return bindless_index_; }
    ID3D12Resource* Handle() const { return resource_.Get(); }

private:
    ComPtr<ID3D12Resource> resource_;
    ComPtr<ID3D12Resource> upload_staging_;
    u32 bindless_index_ = static_cast<u32>(-1);
};

} // namespace aether::gfx
