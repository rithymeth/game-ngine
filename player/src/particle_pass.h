#pragma once

#include "aether/gfx/rhi/device.h"
#include "aether/vfx/render.h"

#include <array>
#include <vector>

// The player's particle pass (M8.7). PackParticleVertices is the CPU side: a frame's particle draw data (vfx::ParticleRenderData) as triangle-list vertices for the
// scene renderer's two particle pipelines, one alpha-blended and one additive. Sprites are already camera-facing (BuildRenderData builds their
// axes from the camera), so each becomes two triangles here and the vertex shader only projects them. Alpha-blended sprites from every batch
// are sorted back to front by their distance along the view; additive ones are order-independent and keep their order. Ribbons are unrolled
// from their indices the same way. Mesh particles are not drawn yet.

namespace aether::player {

// Matches the RHI's PositionNormalUvTangent layout (48 bytes): `params` rides in the normal slot (x: 1 for a sprite, whose uv draws a soft
// round falloff, 0 for a ribbon) and the colour in the tangent slot (straight alpha).
struct ParticleQuadVertex {
    f32 position[3];
    f32 params[3];
    f32 uv[2];
    f32 color[4];
};
static_assert(sizeof(ParticleQuadVertex) == sizeof(f32) * 12);

struct ParticleVertexStreams {
    std::vector<ParticleQuadVertex> alpha;    // Alpha, Premultiplied (converted to straight alpha) and Opaque (alpha 1)
    std::vector<ParticleQuadVertex> additive; // Additive
    void Clear() {
        alpha.clear();
        additive.clear();
    }
};

// Fills `out` (cleared first; its capacity is kept, so a renderer reusing it allocates only when a frame has more particles than before).
void PackParticleVertices(const vfx::ParticleRenderData& data, const vfx::ParticleCamera& camera, ParticleVertexStreams& out);

// The GPU side: an alpha-blended and an additive pipeline (one HLSL source, compiled by the RHI for each backend), depth-tested against the
// swap chain's depth without writing it, and per frame in flight a vertex buffer for each stream (grown to the largest frame seen, never
// shrunk; the RHI has no buffer destroy, so a replaced buffer stays with the device).
class ParticlePass {
public:
    static constexpr usize kFrameSlots = 8;
    ParticlePass(gfx::rhi::IDevice& device, gfx::rhi::ISwapChain& swap_chain);
    bool Valid() const { return alpha_pipeline_.IsValid() && additive_pipeline_.IsValid(); }
    // Records the particles' draws inside the current render pass (after the opaque surfaces). `frame_index` picks the buffers, so the
    // previous use of that slot's frame must have finished on the GPU.
    void Draw(gfx::rhi::ICommandList& commands, const vfx::ParticleRenderData& data, const vfx::ParticleCamera& camera,
              const Mat4& view_projection, u32 frame_index);
    const ParticleVertexStreams& Vertices() const { return vertices_; } // the last Draw's

private:
    struct Buffer {
        gfx::rhi::BufferHandle handle;
        usize capacity = 0; // vertices
    };
    void DrawStream(gfx::rhi::ICommandList& commands, gfx::rhi::PipelineHandle pipeline, const std::vector<ParticleQuadVertex>& vertices,
                    Buffer& buffer, const Mat4& view_projection);

    gfx::rhi::IDevice& device_;
    gfx::rhi::PipelineHandle alpha_pipeline_, additive_pipeline_;
    std::array<std::array<Buffer, 2>, kFrameSlots> buffers_{};
    ParticleVertexStreams vertices_;
};

} // namespace aether::player
