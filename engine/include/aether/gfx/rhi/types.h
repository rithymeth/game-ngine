#pragma once

#include "aether/core/base.h"

namespace aether::gfx::rhi {

enum class Backend { D3D12, Vulkan };

// Opaque handle into a backend device's internal texture table — never
// dereferenced by calling code, only passed back into IDevice/ICommandList
// calls. This (rather than exposing ID3D12Resource*/VkImage directly) is
// what lets code written against these interfaces run unchanged on either
// backend.
struct TextureHandle {
    u32 index = static_cast<u32>(-1);
    bool IsValid() const { return index != static_cast<u32>(-1); }
    bool operator==(const TextureHandle& o) const { return index == o.index; }
};

// A deliberately small state set — just enough for the swapchain
// present/clear lifecycle this abstraction layer covers today. Extending it
// (render targets you draw into with a pipeline, shader resources, UAVs,
// ...) is exactly the next layer of work; see the RHI docs in render_graph.h
// and this module's README section for what's D3D12-only for now.
enum class ResourceState { Undefined, RenderTarget, Present, CopyDest, CopySource };

struct ClearColor {
    f32 r, g, b, a;
};

struct Viewport {
    f32 x, y, width, height, min_depth, max_depth;
};

struct Rect {
    i32 left, top, right, bottom;
};

// Opaque handle into a backend device's internal pipeline table — see
// IDevice::CreatePipeline. Never dereferenced by calling code.
struct PipelineHandle {
    u32 index = static_cast<u32>(-1);
    bool IsValid() const { return index != static_cast<u32>(-1); }
};

// Opaque handle into a backend device's internal vertex/index buffer table —
// see IDevice::CreateVertexBuffer/CreateIndexBuffer. Never dereferenced by
// calling code.
struct BufferHandle {
    u32 index = static_cast<u32>(-1);
    bool IsValid() const { return index != static_cast<u32>(-1); }
};

// Opaque handle into a backend device's global bindless texture table — see
// IDevice::CreateTexture. Unlike TextureHandle (render-target/backbuffer
// textures, tracked in a separate table), a SampledTextureHandle's `index`
// IS the value a shader receives to select this texture, matching the
// bindless convention gfx::DescriptorHeap/gfx::Texture already use
// elsewhere in this engine (see descriptor_heap.h's comment) — there's no
// per-draw descriptor rebinding, ICommandList::BindBindlessTextures() binds
// the whole table once and a pipeline's push constants carry the index.
// Index zero always selects the white fallback; real texture handles start at 1.
struct SampledTextureHandle {
    u32 index = static_cast<u32>(-1);
    bool IsValid() const { return index != static_cast<u32>(-1); }
};

// Slot zero is the permanent white fallback on both backends. This fixed
// table capacity avoids Vulkan descriptor-set-layout recreation; the table
// is built once and every descriptor is valid before any real texture loads.
constexpr u32 kMaxBindlessTextures = 32;
constexpr u32 kBindlessFallbackTextureIndex = 0;
constexpr u32 kFirstUserBindlessTextureIndex = kBindlessFallbackTextureIndex + 1;
constexpr u32 kMaxUserBindlessTextures = kMaxBindlessTextures - kFirstUserBindlessTextureIndex;

enum class IndexFormat { UInt16, UInt32 };

} // namespace aether::gfx::rhi
