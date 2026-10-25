#include "../player/src/particle_pass.h"

#include "aether/gfx/rhi/command_list.h"
#include "aether/gfx/rhi/device.h"
#include "test_framework.h"

#include <cmath>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>

// The player's particle pass, CPU side (player/src/particle_pass.h): sprites become two camera-facing triangles in the
// alpha or additive stream, alpha-blended sprites sort back to front across batches, premultiplied colours become
// straight alpha, and ribbons unroll from their indices. Materials resolve to bindless textures carried per vertex with the
// flipbook's next cell and frame blend, and (on a Vulkan device) a textured sprite samples its texture.

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
    AETHER_CHECK(Near(out.additive[5].position[0], 3.0f) && Near(out.additive[5].uv[0], 1.0f) && Near(out.additive[5].params[0], 0.25f));
    AETHER_CHECK(!ParticleVertexIsSprite(out.additive[5]) && ParticleVertexTexture(out.additive[5]) == 0);
}

AETHER_TEST(PlayerParticles_MaterialsResolveToTexturesWithFlipbookBlend) {
    vfx::ParticleCamera camera;
    vfx::ParticleRenderData data;
    vfx::SpriteBatch smoke, sparks, plain;
    smoke.material = "textures/smoke_4x4.png";
    smoke.blend_frames = true;
    vfx::SpriteInstance s = Sprite(-4.0f);
    s.uv = vfx::FlipbookCell(1, 4, 4);
    s.uv_next = vfx::FlipbookCell(2, 4, 4);
    s.frame_blend = 0.25f;
    smoke.instances = {s};
    sparks.material = "textures/spark.png";
    sparks.blend = vfx::BlendMode::Additive;
    sparks.blend_frames = false; // the cross-fade is off: the frame blend is dropped
    sparks.instances = {s};
    plain.instances = {Sprite(-2.0f)}; // no material
    data.sprites = {smoke, sparks, plain};
    vfx::RibbonStrip trail;
    trail.material = "textures/trail.png";
    trail.blend = vfx::BlendMode::Additive;
    trail.vertices.resize(3);
    trail.indices = {0, 1, 2};
    data.ribbons.push_back(trail);

    int calls = 0;
    const ParticleTextureResolver resolver = [&](const std::string& material) -> u32 {
        ++calls;
        if (material == "textures/smoke_4x4.png") return 3;
        if (material == "textures/spark.png") return 7;
        if (material == "textures/trail.png") return 31;
        return 0;
    };
    ParticleVertexStreams out;
    PackParticleVertices(data, camera, out, resolver);
    AETHER_CHECK(calls == 3); // once per batch and ribbon with a material
    AETHER_CHECK(out.alpha.size() == 12 && out.additive.size() == 9);
    // Back to front: the smoke (z -4) then the untextured sprite (z -2).
    const ParticleQuadVertex& smoke_bl = out.alpha[0];
    AETHER_CHECK(ParticleVertexIsSprite(smoke_bl) && ParticleVertexTexture(smoke_bl) == 3 && Near(ParticleVertexFrameBlend(smoke_bl), 0.25f, 1e-4f));
    // The next cell is one column to the right; uv is cell 1 (v down: bottom-left is the cell's bottom).
    AETHER_CHECK(Near(smoke_bl.params[1], 0.25f) && Near(smoke_bl.params[2], 0.0f));
    AETHER_CHECK(Near(smoke_bl.uv[0], 0.25f) && Near(smoke_bl.uv[1], 0.25f) && Near(out.alpha[2].uv[0], 0.5f) && Near(out.alpha[2].uv[1], 0.0f));
    const ParticleQuadVertex& plain_bl = out.alpha[6];
    AETHER_CHECK(ParticleVertexIsSprite(plain_bl) && ParticleVertexTexture(plain_bl) == 0 && Near(plain_bl.params[0], 1.0f));
    const ParticleQuadVertex& spark = out.additive[0];
    AETHER_CHECK(ParticleVertexTexture(spark) == 7 && Near(ParticleVertexFrameBlend(spark), 0.0f) && Near(spark.params[1], 0.0f));
    AETHER_CHECK(!ParticleVertexIsSprite(out.additive[6]) && ParticleVertexTexture(out.additive[6]) == 31);

    // Indices outside the bindless table draw untextured; no resolver, nothing is textured.
    const ParticleTextureResolver too_big = [](const std::string&) -> u32 { return gfx::rhi::kMaxBindlessTextures; };
    PackParticleVertices(data, camera, out, too_big);
    AETHER_CHECK(ParticleVertexTexture(out.alpha[0]) == 0 && ParticleVertexIsSprite(out.alpha[0]));
    PackParticleVertices(data, camera, out);
    AETHER_CHECK(ParticleVertexTexture(out.alpha[0]) == 0 && ParticleVertexTexture(out.additive[6]) == 0);
}

AETHER_TEST(PlayerParticles_TexturedSpritesSampleTheirFlipbookCellOnVulkan) {
    std::unique_ptr<gfx::rhi::IDevice> device;
    try {
        device = gfx::rhi::CreateDevice(gfx::rhi::Backend::Vulkan, false);
    } catch (const std::exception&) {
    }
    if (!device) {
        std::printf("    (no Vulkan device: install a driver, or Mesa's lavapipe, to run this)\n");
        return;
    }
    constexpr u32 kSize = 64;
    std::unique_ptr<gfx::rhi::ISwapChain> target = device->CreateSwapChain(nullptr, kSize, kSize, 2);
    ParticlePass pass(*device, *target);
    AETHER_CHECK(pass.Valid());
    // A 2 x 1 flipbook: frame 0 green, frame 1 black (both opaque). Green survives an RGBA/BGRA readback either way.
    const u8 pixels[] = {0, 255, 0, 255, 0, 0, 0, 255};
    const gfx::rhi::SampledTextureHandle flipbook = device->CreateTexture(2, 1, pixels);
    AETHER_CHECK(flipbook.IsValid() && flipbook.index != 0);
    pass.SetTextureResolver([&](const std::string& material) -> u32 { return material == "flipbook" ? flipbook.index : 0u; });

    // One full-screen-ish sprite straight in clip space (identity view-projection), white, opaque.
    const auto draw = [&](const std::string& material, u32 frame, f32 blend, std::vector<u8>& out) {
        vfx::ParticleRenderData data;
        vfx::SpriteBatch batch;
        batch.material = material;
        batch.blend_frames = blend > 0.0f;
        vfx::SpriteInstance s;
        s.center = Vec3(0.0f, 0.0f, 0.5f);
        s.right = Vec3(0.8f, 0.0f, 0.0f);
        s.up = Vec3(0.0f, 0.8f, 0.0f);
        s.color = {1.0f, 1.0f, 1.0f, 1.0f};
        s.uv = vfx::FlipbookCell(frame, 2, 1);
        s.uv_next = vfx::FlipbookCell(frame + 1, 2, 1);
        s.frame_blend = blend;
        batch.instances = {s};
        data.sprites.push_back(batch);
        std::unique_ptr<gfx::rhi::ICommandList> cmd = device->CreateCommandList();
        target->AcquireNextImage();
        cmd->Reset();
        cmd->BeginRenderPass(*target, gfx::rhi::ClearColor{0.0f, 0.0f, 0.0f, 1.0f});
        pass.Draw(*cmd, data, vfx::ParticleCamera{}, Mat4::Identity(), 0);
        cmd->EndRenderPass();
        cmd->Close();
        device->WaitForFence(device->Submit(*cmd, target.get()));
        const bool read = target->ReadBack(out);
        target->Present(false);
        return read && out.size() == kSize * kSize * 4;
    };
    // The centre pixel, a little off the sprite's middle so the untextured falloff is still bright there.
    const usize centre = (static_cast<usize>(kSize / 2 + 2) * kSize + kSize / 2 + 2) * 4;
    std::vector<u8> untextured, frame0, blended;
    AETHER_CHECK(draw("", 0, 0.0f, untextured));
    AETHER_CHECK(draw("flipbook", 0, 0.0f, frame0));
    AETHER_CHECK(draw("flipbook", 0, 0.9f, blended));
    if (untextured.empty() || frame0.empty() || blended.empty()) return;
    // Untextured: the white falloff (every channel lit). Frame 0: green only. Blended 90% into frame 1: nearly black.
    AETHER_CHECK(untextured[centre] > 128 && untextured[centre + 1] > 128 && untextured[centre + 2] > 128);
    AETHER_CHECK(frame0[centre + 1] > 200 && frame0[centre] < 40 && frame0[centre + 2] < 40);
    AETHER_CHECK(blended[centre + 1] < 140 && blended[centre + 1] + 40 < frame0[centre + 1]);
}
