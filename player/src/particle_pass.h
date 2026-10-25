#pragma once

#include "aether/gfx/rhi/device.h"
#include "aether/vfx/render.h"

#include <array>
#include <functional>
#include <string>
#include <vector>

// The player's particle pass (M8.7). PackParticleVertices is the CPU side: a frame's particle draw data (vfx::ParticleRenderData) as triangle-list vertices for the
// scene renderer's two particle pipelines, one alpha-blended and one additive. Sprites are already camera-facing (BuildRenderData builds their
// axes from the camera), so each becomes two triangles here and the vertex shader only projects them. Alpha-blended sprites from every batch
// are sorted back to front by their distance along the view; additive ones are order-independent and keep their order. Ribbons are unrolled
// from their indices the same way. A batch's or ribbon's `material` names a texture in the package (its sprites' flipbook cells, cross-faded
// to the next cell when the batch blends frames); each vertex carries its texture's bindless index. Mesh particles are not drawn yet.

namespace aether::player {

// Matches the RHI's PositionNormalUvTangent layout (48 bytes): `params` rides in the normal slot and the colour in the tangent slot (straight
// alpha). params.x is 4 * the texture's bindless index (0: untextured) plus the kind: 0.25 for a ribbon, 1 + the frame blend (0..1) for a
// sprite (ParticleVertexTexture / ParticleVertexFrameBlend decode it); params.yz is where the next flipbook cell is, relative to this one
// (the cells are the same size). An untextured sprite draws a soft round falloff instead.
struct ParticleQuadVertex {
    f32 position[3];
    f32 params[3];
    f32 uv[2];
    f32 color[4];
};
static_assert(sizeof(ParticleQuadVertex) == sizeof(f32) * 12);
inline u32 ParticleVertexTexture(const ParticleQuadVertex& v) { return static_cast<u32>(v.params[0] / 4.0f); }
inline bool ParticleVertexIsSprite(const ParticleQuadVertex& v) { return v.params[0] - 4.0f * static_cast<f32>(ParticleVertexTexture(v)) > 0.5f; }
inline f32 ParticleVertexFrameBlend(const ParticleQuadVertex& v) {
    return ParticleVertexIsSprite(v) ? v.params[0] - 4.0f * static_cast<f32>(ParticleVertexTexture(v)) - 1.0f : 0.0f;
}

struct ParticleVertexStreams {
    std::vector<ParticleQuadVertex> alpha;    // Alpha, Premultiplied (converted to straight alpha) and Opaque (alpha 1)
    std::vector<ParticleQuadVertex> additive; // Additive
    void Clear() {
        alpha.clear();
        additive.clear();
    }
};

// A material (texture path) to its bindless texture index, 0 for none or one that couldn't be loaded. Called once per batch and ribbon.
using ParticleTextureResolver = std::function<u32(const std::string& material)>;

// Fills `out` (cleared first; its capacity is kept, so a renderer reusing it allocates only when a frame has more particles than before).
// Without a resolver nothing is textured.
void PackParticleVertices(const vfx::ParticleRenderData& data, const vfx::ParticleCamera& camera, ParticleVertexStreams& out,
                          const ParticleTextureResolver& textures = {});

// The GPU side: an alpha-blended and an additive pipeline (one HLSL source, compiled by the RHI for each backend, sampling the bindless
// texture table), depth-tested against the
// swap chain's depth without writing it, and per frame in flight a vertex buffer for each stream (grown to the largest frame seen, never
// shrunk; the RHI has no buffer destroy, so a replaced buffer stays with the device).
class ParticlePass {
public:
    static constexpr usize kFrameSlots = 8;
    ParticlePass(gfx::rhi::IDevice& device, gfx::rhi::ISwapChain& swap_chain);
    // How materials become textures (the scene renderer's texture cache); unset, particles draw untextured.
    void SetTextureResolver(ParticleTextureResolver resolver) { textures_ = std::move(resolver); }
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
    ParticleTextureResolver textures_;
};

} // namespace aether::player
