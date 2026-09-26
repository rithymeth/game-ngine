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

} // namespace aether::gfx::rhi
