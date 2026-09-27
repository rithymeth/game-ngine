#pragma once

#include "aether/math/mat4.h"
#include "aether/vfx/simulation.h"

#include <string>
#include <vector>

namespace aether::vfx {

// Draw data for particles (Phase 19 step 2, §19.2): each emitter's render
// modules turn its live particles into sprite instances, mesh instances,
// ribbon strips and lights, in world space, sorted and ready for the
// renderer's particle pass (drawn on the GPU with the Windows renderer).

// The view particles are drawn for. `forward` is where the camera looks.
struct ParticleCamera {
    Vec3 position;
    Vec3 right{1.0f, 0.0f, 0.0f}, up{0.0f, 1.0f, 0.0f}, forward{0.0f, 0.0f, -1.0f};
    // From a view matrix (world to view, looking down -Z; Mat4::LookAtRH).
    static ParticleCamera FromView(const Mat4& view);
};

struct UvRect {
    f32 x = 0.0f, y = 0.0f, w = 1.0f, h = 1.0f;
};

// A camera-ready quad: its corners are center ± right ± up (half sizes
// already in the axes), texture from `uv`, and with frame blending `uv_next`
// mixed in by `frame_blend`.
struct SpriteInstance {
    Vec3 center;
    Vec3 right, up;
    LinearColor color;
    UvRect uv, uv_next;
    f32 frame_blend = 0.0f;
};

struct SpriteBatch {
    std::string material;
    BlendMode blend = BlendMode::Alpha;
    f32 soft_fade = 0.0f;
    bool blend_frames = false;
    std::vector<SpriteInstance> instances; // in drawing order
};

struct MeshBatch {
    std::string mesh, material;
    std::vector<Mat4> transforms; // in drawing order
    std::vector<LinearColor> colors;
};

struct ParticleVertex {
    Vec3 position;
    LinearColor color;
    f32 u = 0.0f, v = 0.0f;
};

struct RibbonStrip {
    std::string material;
    BlendMode blend = BlendMode::Alpha;
    std::vector<ParticleVertex> vertices; // two per point: the left then the right edge
    std::vector<u32> indices;             // triangles
};

struct ParticleLight {
    Vec3 position;
    LinearColor color;
    f32 radius = 1.0f;
    f32 intensity = 1.0f;
};

struct ParticleRenderData {
    std::vector<SpriteBatch> sprites;
    std::vector<MeshBatch> meshes;
    std::vector<RibbonStrip> ribbons;
    std::vector<ParticleLight> lights;
    void Clear();
    usize DrawnParticles() const; // sprites + meshes + ribbon points
};

// Appends what `emitter`'s render modules draw for `camera`.
void BuildRenderData(const EmitterInstance& emitter, const ParticleCamera& camera, ParticleRenderData& out);
void BuildRenderData(const ParticleSystemInstance& system, const ParticleCamera& camera, ParticleRenderData& out);

// The flipbook cell a sprite shows (frame `index` of columns x rows, left to
// right then top to bottom).
UvRect FlipbookCell(u32 index, u32 columns, u32 rows);
// Which frame a particle is on, and how far to the next, for a renderer's settings.
void FlipbookFrame(const SpriteRenderer& r, f32 normalized_age, f32 age, u32 seed, u32& frame, u32& next, f32& blend);

// Sprites as plain triangles (four vertices, six indices each), for
// renderers without instancing and for tests.
void ExpandSprites(const SpriteBatch& batch, std::vector<ParticleVertex>& vertices, std::vector<u32>& indices);

} // namespace aether::vfx
