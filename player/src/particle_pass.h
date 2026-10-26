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
// to the next cell when the batch blends frames); each vertex carries its texture's bindless index, and the pass draws each run of one texture separately. Mesh particles (MeshBatch) are expanded on the CPU too: each
// instance's transform applied to its mesh's triangles (a ParticleMeshResolver gives them), into a third, opaque stream drawn first with
// depth writes, so the blended streams test against them. Mesh particles are lit (M8.7) with the scene shader's sun and sky model
// (ParticleLighting, filled from the frame's DirectionalLight/SkyLight): each vertex's normal goes through the instance transform's
// inverse transpose and rides octahedral-encoded in params.yz. Expansion stays on the CPU: the RHI has no instance-rate vertex input or
// instanced draw, so GPU instancing is open (it needs ICommandList/PipelineDesc API on every backend).
// Soft particles: a sprite batch's soft_fade (SpriteRenderer::soft_fade in the .avfx, 0: off) fades its sprites out within that view
// distance of the opaque scene behind them, read from the swap chain's SceneDepthTexture; the pass copies the depth (ICommandList::
// CopySceneDepth) after the mesh particles, so they count as scene, and only on frames where some drawn sprite has a soft_fade.

namespace aether::player {

// Matches the RHI's PositionNormalUvTangent layout (48 bytes): `params` rides in the normal slot and the colour in the tangent slot (straight
// alpha). params.x is 4 * the texture's bindless index (0: untextured) plus the kind: 0.25 for a ribbon, 1 + the frame blend (0..1) for a
// sprite (ParticleVertexTexture / ParticleVertexFrameBlend decode it); params.yz is where the next flipbook cell is, relative to this one
// (the cells are the same size). An untextured sprite draws a soft round falloff instead. For a mesh particle (kind 0.25, opaque stream)
// params.yz is instead its world normal, octahedral-encoded (ParticleEncodeNormal; ParticleVertexNormal decodes it).
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

// Octahedral normal encoding (two values in -1..1 for a unit vector; the shader decodes the same way). A zero vector encodes as +Z.
void ParticleEncodeNormal(const Vec3& normal, f32& u, f32& v);
Vec3 ParticleDecodeNormal(f32 u, f32 v);
inline Vec3 ParticleVertexNormal(const ParticleQuadVertex& v) { return ParticleDecodeNormal(v.params[1], v.params[2]); }

// The light mesh particles get: the scene shader's sun (direction towards it, colour times intensity) and sky ambient, the same defaults
// as its fixed light. `lit` false draws them unlit (colour times texture), as before M8.7's lighting; the scene renderer always lights them.
struct ParticleLighting {
    bool lit = false;
    Vec3 sun_direction{-0.4f, 0.8f, -0.35f};
    Vec3 sun_color{3.0f, 3.0f, 3.0f};
    Vec3 sky_color{1.0f, 1.0f, 1.0f};
};
// The shader's lit mesh colour for a base colour (linear, texture included) and unit world normal: the scene shader's dielectric diffuse
// (sky fill 0.28 + 0.14 * max(n.y, 0) times the sky, plus 0.96 / pi * N.L times the sun; no specular) tone mapped like the scene
// (c / (c + 1)), before the sRGB encode. For tests and tools.
Vec3 ParticleLitColor(const ParticleLighting& lighting, const Vec3& base_color, const Vec3& normal);

struct ParticleVertexStreams {
    std::vector<ParticleQuadVertex> alpha;    // Alpha, Premultiplied (converted to straight alpha) and Opaque (alpha 1)
    std::vector<ParticleQuadVertex> additive; // Additive
    std::vector<ParticleQuadVertex> opaque;   // mesh particles (alpha 1), drawn first, writing depth
    // Each blended vertex's soft-fade distance (its batch's soft_fade; 0 for ribbons and batches without one), one per vertex of the stream.
    std::vector<f32> alpha_soft, additive_soft;
    void Clear() {
        alpha.clear();
        additive.clear();
        opaque.clear();
        alpha_soft.clear();
        additive_soft.clear();
    }
    bool AnySoft() const;
};

// A run of consecutive vertices with the same texture: the pass draws each separately (first-vertex Draw from the one buffer) so the
// texture index is uniform across a draw, with no non-uniform descriptor indexing. Runs follow the stream's order (back to front stays).
struct ParticleDrawRun {
    u32 first_vertex = 0;
    u32 vertex_count = 0;
    u32 texture = 0;
    f32 soft_fade = 0.0f; // uniform across the run too (a push constant)
};
// Fills `out` (cleared first) with `vertices`' runs: a run ends where the texture or, given `soft` (one per vertex), the soft distance changes.
void ParticleTextureRuns(const std::vector<ParticleQuadVertex>& vertices, std::vector<ParticleDrawRun>& out, const std::vector<f32>* soft = nullptr);

// Soft-fade depth reconstruction: the coefficients (a, b, c, d) of `projection` that map a clip-space depth d (0..1, what the depth buffer
// holds) back to the view-space z: z = (b - d * dd) / (d * c - a), with a = P[2][2], b = P[2][3], c = P[3][2], dd = P[3][3] (row, column).
// Perspective and orthographic alike. ParticleViewDistance is the shader's formula (|z|), for tests.
std::array<f32, 4> ParticleDepthToView(const Mat4& projection);
f32 ParticleViewDistance(const std::array<f32, 4>& depth_to_view, f32 depth);
// The fade a soft sprite fragment at `fragment_depth` gets in front of scene depth `scene_depth` (both 0..1): its view distance to the
// scene over `soft_fade`, clamped to 0..1; 1 when soft_fade <= 0. The shader computes the same.
f32 ParticleSoftFade(const std::array<f32, 4>& depth_to_view, f32 scene_depth, f32 fragment_depth, f32 soft_fade);

// A material (texture path) to its bindless texture index, 0 for none or one that couldn't be loaded. Called once per batch and ribbon.
using ParticleTextureResolver = std::function<u32(const std::string& material)>;

// A mesh particle's geometry: a triangle list in the mesh's own space (three entries per triangle), with texture coordinates.
struct ParticleMesh {
    std::vector<Vec3> positions;
    std::vector<f32> uvs; // two per position (empty: all 0)
    std::vector<Vec3> normals; // one per position, in the mesh's space (empty: each triangle's face normal, counter-clockwise front)
};
// A mesh path to its geometry, nullptr for none or one that couldn't be loaded (the batch is skipped). Called once per mesh batch; the
// mesh must outlive the PackParticleVertices call.
using ParticleMeshResolver = std::function<const ParticleMesh*(const std::string& mesh)>;

// Fills `out` (cleared first; its capacity is kept, so a renderer reusing it allocates only when a frame has more particles than before).
// Without a resolver nothing is textured; without a mesh resolver mesh particles are not drawn.
void PackParticleVertices(const vfx::ParticleRenderData& data, const vfx::ParticleCamera& camera, ParticleVertexStreams& out,
                          const ParticleTextureResolver& textures = {}, const ParticleMeshResolver& meshes = {});

// The GPU side: an opaque (mesh particles, writing depth), an alpha-blended and an additive pipeline (one HLSL source, compiled by the RHI for each backend, sampling the bindless
// texture table), depth-tested against the
// swap chain's depth without writing it (soft sprites also read its SceneDepthTexture copy), and per frame in flight a vertex buffer for each stream (grown to the largest frame seen, never
// shrunk; the RHI has no buffer destroy, so a replaced buffer stays with the device).
class ParticlePass {
public:
    static constexpr usize kFrameSlots = 8;
    ParticlePass(gfx::rhi::IDevice& device, gfx::rhi::ISwapChain& swap_chain);
    // How materials become textures (the scene renderer's texture cache); unset, particles draw untextured.
    void SetTextureResolver(ParticleTextureResolver resolver) { textures_ = std::move(resolver); }
    // How mesh paths become geometry (the scene renderer's model cache); unset, mesh particles are not drawn.
    void SetMeshResolver(ParticleMeshResolver resolver) { meshes_ = std::move(resolver); }
    // The light mesh particles are drawn with from the next Draw on (default: unlit).
    void SetLighting(const ParticleLighting& lighting) { lighting_ = lighting; }
    const ParticleLighting& Lighting() const { return lighting_; }
    bool Valid() const { return alpha_pipeline_.IsValid() && additive_pipeline_.IsValid() && opaque_pipeline_.IsValid(); }
    // Records the particles' draws inside the current render pass (after the opaque surfaces). `frame_index` picks the buffers, so the
    // previous use of that slot's frame must have finished on the GPU. `projection` (view_projection's projection part) reconstructs
    // view distances for soft sprites; with soft ones drawn and a scene depth texture the pass calls CopySceneDepth, so the caller's
    // bindings are gone afterwards.
    void Draw(gfx::rhi::ICommandList& commands, const vfx::ParticleRenderData& data, const vfx::ParticleCamera& camera,
              const Mat4& view_projection, const Mat4& projection, u32 frame_index);
    // The swap chain's scene depth (0 when the backend has none: soft sprites draw hard-edged).
    u32 SceneDepthTexture() const { return scene_depth_; }
    const ParticleVertexStreams& Vertices() const { return vertices_; } // the last Draw's

private:
    struct Buffer {
        gfx::rhi::BufferHandle handle;
        usize capacity = 0; // vertices
    };
    void DrawStream(gfx::rhi::ICommandList& commands, gfx::rhi::PipelineHandle pipeline, const std::vector<ParticleQuadVertex>& vertices,
                    const std::vector<f32>* soft, Buffer& buffer, const Mat4& view_projection, u32 scene_depth, bool lit = false);

    gfx::rhi::IDevice& device_;
    gfx::rhi::PipelineHandle alpha_pipeline_, additive_pipeline_, opaque_pipeline_;
    std::array<std::array<Buffer, 3>, kFrameSlots> buffers_{};
    ParticleVertexStreams vertices_;
    std::vector<ParticleDrawRun> runs_;
    u32 scene_depth_ = 0;
    std::array<f32, 4> depth_to_view_{};
    ParticleTextureResolver textures_;
    ParticleMeshResolver meshes_;
    ParticleLighting lighting_;
};

} // namespace aether::player
