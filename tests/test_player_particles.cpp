#include "../player/src/particle_pass.h"

#include "test_framework.h"

#include <cmath>

// The player's particle pass, CPU side (player/src/particle_pass.h): sprites become two camera-facing triangles in the
// alpha or additive stream, alpha-blended sprites sort back to front across batches, premultiplied colours become
// straight alpha, and ribbons unroll from their indices.

using namespace aether;
using namespace aether::player;

namespace {

bool Near(f32 a, f32 b, f32 e = 1e-5f) { return std::fabs(a - b) <= e; }

vfx::SpriteInstance Sprite(f32 z, vfx::LinearColor color = {}) {
    vfx::SpriteInstance s;
    s.center = Vec3(0.0f, 0.0f, z);
    s.right = Vec3(0.5f, 0.0f, 0.0f);
    s.up = Vec3(0.0f, 0.25f, 0.0f);
    s.color = color;
    return s;
}

} // namespace

AETHER_TEST(PlayerParticles_SpritesBecomeCameraFacingQuadsByBlend) {
    vfx::ParticleCamera camera; // at the origin, looking down -Z
    vfx::ParticleRenderData data;
    vfx::SpriteBatch additive;
    additive.blend = vfx::BlendMode::Additive;
    additive.instances = {Sprite(-2.0f, {0.0f, 0.35f, 0.85f, 1.0f}), Sprite(-9.0f)};
    data.sprites.push_back(additive);
    ParticleVertexStreams out;
    PackParticleVertices(data, camera, out);
    AETHER_CHECK(out.alpha.empty() && out.additive.size() == 12);
    // Additive keeps its order; corners are center -/+ right -/+ up, uv v down; colour and the sprite flag ride along.
    const ParticleQuadVertex& bl = out.additive[0];
    const ParticleQuadVertex& tr = out.additive[2];
    AETHER_CHECK(Near(bl.position[0], -0.5f) && Near(bl.position[1], -0.25f) && Near(bl.position[2], -2.0f));
    AETHER_CHECK(Near(tr.position[0], 0.5f) && Near(tr.position[1], 0.25f));
    AETHER_CHECK(Near(bl.uv[0], 0.0f) && Near(bl.uv[1], 1.0f) && Near(tr.uv[0], 1.0f) && Near(tr.uv[1], 0.0f));
    AETHER_CHECK(Near(bl.color[1], 0.35f) && Near(bl.color[2], 0.85f) && Near(bl.color[3], 1.0f) && Near(bl.params[0], 1.0f));
    AETHER_CHECK(Near(out.additive[6].position[2], -9.0f));
    // Two triangles sharing the diagonal: (bl, br, tr) and (bl, tr, tl).
    AETHER_CHECK(Near(out.additive[3].position[0], bl.position[0]) && Near(out.additive[4].position[0], tr.position[0]));

    // Repacking clears first.
    PackParticleVertices(data, camera, out);
    AETHER_CHECK(out.additive.size() == 12);
}

AETHER_TEST(PlayerParticles_AlphaSpritesSortBackToFrontAcrossBatches) {
    vfx::ParticleCamera camera;
    vfx::ParticleRenderData data;
    vfx::SpriteBatch near_batch, far_batch;
    near_batch.blend = vfx::BlendMode::Alpha;
    near_batch.instances = {Sprite(-1.0f), Sprite(-5.0f)};
    far_batch.blend = vfx::BlendMode::Premultiplied;
    far_batch.instances = {Sprite(-3.0f, {0.2f, 0.1f, 0.0f, 0.5f}), Sprite(-0.0f, {0.3f, 0.3f, 0.3f, 0.0f})};
    data.sprites = {near_batch, far_batch};
    ParticleVertexStreams out;
    PackParticleVertices(data, camera, out);
    AETHER_CHECK(out.additive.empty() && out.alpha.size() == 24);
    const f32 expected[] = {-5.0f, -3.0f, -1.0f, 0.0f};
    for (usize i = 0; i < 4; ++i) AETHER_CHECK(Near(out.alpha[i * 6].position[2], expected[i]));
    // Premultiplied colours become straight alpha (none adds nothing).
    AETHER_CHECK(Near(out.alpha[6].color[0], 0.4f) && Near(out.alpha[6].color[1], 0.2f) && Near(out.alpha[6].color[3], 0.5f));
    AETHER_CHECK(Near(out.alpha[18].color[0], 0.0f) && Near(out.alpha[18].color[3], 0.0f));

    // Opaque draws with alpha 1 in the alpha stream.
    vfx::SpriteBatch opaque;
    opaque.blend = vfx::BlendMode::Opaque;
    opaque.instances = {Sprite(-2.0f, {1.0f, 1.0f, 1.0f, 0.25f})};
    data.sprites = {opaque};
    PackParticleVertices(data, camera, out);
    AETHER_CHECK(out.alpha.size() == 6 && Near(out.alpha[0].color[3], 1.0f));
}

AETHER_TEST(PlayerParticles_RibbonsUnrollFromIndices) {
    vfx::ParticleCamera camera;
    vfx::ParticleRenderData data;
    vfx::RibbonStrip ribbon;
    ribbon.blend = vfx::BlendMode::Additive;
    for (int i = 0; i < 4; ++i) {
        vfx::ParticleVertex v;
        v.position = Vec3(static_cast<f32>(i), 0.0f, -1.0f);
        v.u = static_cast<f32>(i) / 3.0f;
        ribbon.vertices.push_back(v);
    }
    ribbon.indices = {0, 1, 2, 2, 1, 3, 0, 9, 1}; // the last triangle names a vertex that isn't there: skipped
    data.ribbons.push_back(ribbon);
    ParticleVertexStreams out;
    PackParticleVertices(data, camera, out);
    AETHER_CHECK(out.additive.size() == 6 && out.alpha.empty());
    AETHER_CHECK(Near(out.additive[5].position[0], 3.0f) && Near(out.additive[5].uv[0], 1.0f) && Near(out.additive[5].params[0], 0.0f));
}
