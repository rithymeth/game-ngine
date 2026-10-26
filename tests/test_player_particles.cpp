#include "../player/src/particle_pass.h"

#include "aether/gfx/rhi/command_list.h"
#include "aether/gfx/rhi/device.h"
#include "aether/vfx/emitter.h"
#include "test_framework.h"

#include <cmath>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>

// The player's particle pass, CPU side (player/src/particle_pass.h): sprites become two camera-facing triangles in the
// alpha or additive stream, alpha-blended sprites sort back to front across batches, premultiplied colours become
// straight alpha, and ribbons unroll from their indices. Materials resolve to bindless textures carried per vertex with the
// flipbook's next cell and frame blend, and (on a Vulkan or D3D12 device) the pass renders: textures, mesh occlusion, soft fade.

using namespace aether;
using namespace aether::player;

namespace {

bool Near(f32 a, f32 b, f32 e = 1e-5f) { return std::fabs(a - b) <= e; }

// The GPU render checks run on every backend this build has: Vulkan (lavapipe on Linux CI) and D3D12 (WARP on the hosted Windows CI
// runner, through the offscreen swap chain). A backend without a device skips with a note.
std::unique_ptr<gfx::rhi::IDevice> MakeRenderDevice(gfx::rhi::Backend backend) {
    std::unique_ptr<gfx::rhi::IDevice> device;
    if (gfx::rhi::IsBackendAvailable(backend)) {
        try {
            device = gfx::rhi::CreateDevice(backend, false);
        } catch (const std::exception&) {
        }
    }
    if (!device) {
        std::printf("    (no %s device: install a driver (Mesa's lavapipe for Vulkan, WARP for D3D12) to run this)\n",
                    backend == gfx::rhi::Backend::Vulkan ? "Vulkan" : "D3D12");
    }
    return device;
}

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

static void PlayerParticles_TexturedSpritesSampleTheirFlipbookCell(gfx::rhi::Backend backend) {
    std::unique_ptr<gfx::rhi::IDevice> device = MakeRenderDevice(backend);
    if (!device) return;
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
        pass.Draw(*cmd, data, vfx::ParticleCamera{}, Mat4::Identity(), Mat4::Identity(), 0);
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

AETHER_TEST(PlayerParticles_StreamsSplitIntoPerTextureRunsInOrder) {
    // Back to front interleaves two textures (A at z -6 and -2, B at -4): three runs, the sort kept, each a whole sprite's vertices.
    vfx::ParticleRenderData data;
    vfx::SpriteBatch a, b, plain;
    a.material = "a";
    a.instances = {Sprite(-2.0f), Sprite(-6.0f)};
    b.material = "b";
    b.instances = {Sprite(-4.0f)};
    plain.blend = vfx::BlendMode::Additive;
    plain.instances = {Sprite(-1.0f), Sprite(-3.0f)};
    data.sprites = {a, b, plain};
    ParticleVertexStreams out;
    PackParticleVertices(data, vfx::ParticleCamera{}, out, [](const std::string& m) -> u32 { return m == "a" ? 5u : m == "b" ? 9u : 0u; });
    std::vector<ParticleDrawRun> runs;
    ParticleTextureRuns(out.alpha, runs);
    AETHER_CHECK(runs.size() == 3);
    if (runs.size() == 3) {
        AETHER_CHECK(runs[0].first_vertex == 0 && runs[0].vertex_count == 6 && runs[0].texture == 5);
        AETHER_CHECK(runs[1].first_vertex == 6 && runs[1].vertex_count == 6 && runs[1].texture == 9);
        AETHER_CHECK(runs[2].first_vertex == 12 && runs[2].vertex_count == 6 && runs[2].texture == 5);
    }
    // Adjacent sprites with one texture share a draw; an empty stream has none.
    ParticleTextureRuns(out.additive, runs);
    AETHER_CHECK(runs.size() == 1 && runs[0].first_vertex == 0 && runs[0].vertex_count == 12 && runs[0].texture == 0);
    ParticleTextureRuns({}, runs);
    AETHER_CHECK(runs.empty());
}

static void PlayerParticles_EachSpriteSamplesItsOwnTextureInOneStream(gfx::rhi::Backend backend) {
    std::unique_ptr<gfx::rhi::IDevice> device = MakeRenderDevice(backend);
    if (!device) return;
    constexpr u32 kSize = 64;
    std::unique_ptr<gfx::rhi::ISwapChain> target = device->CreateSwapChain(nullptr, kSize, kSize, 2);
    ParticlePass pass(*device, *target);
    AETHER_CHECK(pass.Valid());
    // Green and magenta (both survive an RGBA/BGRA readback), 1 x 1, opaque.
    const u8 green[] = {0, 255, 0, 255}, magenta[] = {255, 0, 255, 255};
    const gfx::rhi::SampledTextureHandle tex_green = device->CreateTexture(1, 1, green), tex_magenta = device->CreateTexture(1, 1, magenta);
    AETHER_CHECK(tex_green.IsValid() && tex_magenta.IsValid() && tex_green.index != tex_magenta.index);
    pass.SetTextureResolver([&](const std::string& m) -> u32 { return m == "green" ? tex_green.index : m == "magenta" ? tex_magenta.index : 0u; });

    // Three sprites in one alpha stream, left to right green, magenta, green (three runs, so three first-vertex draws from one buffer).
    vfx::ParticleRenderData data;
    const auto sprite = [](f32 x, f32 z) {
        vfx::SpriteInstance s;
        s.center = Vec3(x, 0.0f, z);
        s.right = Vec3(0.2f, 0.0f, 0.0f);
        s.up = Vec3(0.0f, 0.5f, 0.0f);
        s.color = {1.0f, 1.0f, 1.0f, 1.0f};
        return s;
    };
    vfx::SpriteBatch g, m;
    g.material = "green";
    g.instances = {sprite(-0.6f, 0.3f), sprite(0.6f, 0.7f)};
    m.material = "magenta";
    m.instances = {sprite(0.0f, 0.5f)};
    data.sprites = {g, m};
    std::unique_ptr<gfx::rhi::ICommandList> cmd = device->CreateCommandList();
    target->AcquireNextImage();
    cmd->Reset();
    cmd->BeginRenderPass(*target, gfx::rhi::ClearColor{0.0f, 0.0f, 0.0f, 1.0f});
    pass.Draw(*cmd, data, vfx::ParticleCamera{}, Mat4::Identity(), Mat4::Identity(), 0);
    cmd->EndRenderPass();
    cmd->Close();
    device->WaitForFence(device->Submit(*cmd, target.get()));
    std::vector<u8> out;
    AETHER_CHECK(target->ReadBack(out) && out.size() == kSize * kSize * 4);
    target->Present(false);
    if (out.size() != kSize * kSize * 4) return;
    const auto at = [&](f32 ndc_x) { return (static_cast<usize>(kSize / 2) * kSize + static_cast<usize>((ndc_x * 0.5f + 0.5f) * kSize)) * 4; };
    const usize left = at(-0.6f), middle = at(0.0f), right = at(0.6f), gap = at(-0.3f);
    AETHER_CHECK(out[left + 1] > 200 && out[left] < 40 && out[left + 2] < 40);
    AETHER_CHECK(out[middle] > 200 && out[middle + 1] < 40 && out[middle + 2] > 200);
    AETHER_CHECK(out[right + 1] > 200 && out[right] < 40 && out[right + 2] < 40);
    AETHER_CHECK(out[gap] < 40 && out[gap + 1] < 40 && out[gap + 2] < 40);
}

namespace {

// A unit quad in the mesh's XY plane (two triangles, -0.5..0.5), uv 0..1.
ParticleMesh QuadMesh() {
    ParticleMesh mesh;
    const Vec3 bl(-0.5f, -0.5f, 0.0f), br(0.5f, -0.5f, 0.0f), tr(0.5f, 0.5f, 0.0f), tl(-0.5f, 0.5f, 0.0f);
    mesh.positions = {bl, br, tr, bl, tr, tl};
    mesh.uvs = {0, 1, 1, 1, 1, 0, 0, 1, 1, 0, 0, 0};
    return mesh;
}

Mat4 Placed(f32 x, f32 z, f32 scale) {
    Mat4 m = Mat4::Identity();
    m.cols[0] = Vec4(scale, 0, 0, 0);
    m.cols[1] = Vec4(0, scale, 0, 0);
    m.cols[2] = Vec4(0, 0, scale, 0);
    m.cols[3] = Vec4(x, 0, z, 1);
    return m;
}

} // namespace

AETHER_TEST(PlayerParticles_MeshInstancesExpandIntoTheOpaqueStream) {
    const ParticleMesh quad = QuadMesh();
    vfx::ParticleRenderData data;
    vfx::MeshBatch batch, missing;
    batch.mesh = "quad";
    batch.material = "tex";
    batch.transforms = {Placed(1.0f, -3.0f, 2.0f), Placed(-1.0f, -5.0f, 1.0f)};
    batch.colors = {{0.5f, 0.25f, 1.0f, 0.2f}, {1.0f, 1.0f, 1.0f, 1.0f}};
    missing.mesh = "nope";
    missing.transforms = {Placed(0.0f, 0.0f, 1.0f)};
    data.meshes = {batch, missing};
    const auto meshes = [&](const std::string& m) -> const ParticleMesh* { return m == "quad" ? &quad : nullptr; };
    const auto textures = [](const std::string& m) -> u32 { return m == "tex" ? 7u : 0u; };

    // Without a mesh resolver mesh particles aren't drawn.
    ParticleVertexStreams out;
    PackParticleVertices(data, vfx::ParticleCamera{}, out, textures);
    AETHER_CHECK(out.opaque.empty());

    PackParticleVertices(data, vfx::ParticleCamera{}, out, textures, meshes);
    AETHER_CHECK(out.alpha.empty() && out.additive.empty() && out.opaque.size() == 12); // two instances; the unresolved batch skipped
    if (out.opaque.size() != 12) return;
    // The first instance: scaled by 2 and moved to (1, 0, -3); its colour with alpha forced to 1; textured, not a sprite.
    const ParticleQuadVertex& bl = out.opaque[0];
    const ParticleQuadVertex& tr = out.opaque[2];
    AETHER_CHECK(Near(bl.position[0], 0.0f) && Near(bl.position[1], -1.0f) && Near(bl.position[2], -3.0f));
    AETHER_CHECK(Near(tr.position[0], 2.0f) && Near(tr.position[1], 1.0f));
    AETHER_CHECK(Near(bl.uv[0], 0.0f) && Near(bl.uv[1], 1.0f) && Near(tr.uv[0], 1.0f) && Near(tr.uv[1], 0.0f));
    AETHER_CHECK(Near(bl.color[0], 0.5f) && Near(bl.color[1], 0.25f) && Near(bl.color[3], 1.0f));
    AETHER_CHECK(ParticleVertexTexture(bl) == 7 && !ParticleVertexIsSprite(bl));
    AETHER_CHECK(Near(out.opaque[6].position[0], -1.5f) && Near(out.opaque[6].position[2], -5.0f));
    std::vector<ParticleDrawRun> runs;
    ParticleTextureRuns(out.opaque, runs);
    AETHER_CHECK(runs.size() == 1 && runs[0].vertex_count == 12 && runs[0].texture == 7);
}

static void PlayerParticles_MeshParticlesDrawOpaqueAndOccludeSpritesBehind(gfx::rhi::Backend backend) {
    std::unique_ptr<gfx::rhi::IDevice> device = MakeRenderDevice(backend);
    if (!device) return;
    constexpr u32 kSize = 64;
    std::unique_ptr<gfx::rhi::ISwapChain> target = device->CreateSwapChain(nullptr, kSize, kSize, 2);
    ParticlePass pass(*device, *target);
    AETHER_CHECK(pass.Valid());
    const ParticleMesh quad = QuadMesh();
    pass.SetMeshResolver([&](const std::string& m) -> const ParticleMesh* { return m == "quad" ? &quad : nullptr; });

    // A green mesh quad on the left half, in front (depth 0.3), and a magenta alpha sprite across the whole width behind it (0.7): the
    // mesh writes depth, so the sprite shows only on the right.
    vfx::ParticleRenderData data;
    vfx::MeshBatch batch;
    batch.mesh = "quad";
    batch.transforms = {Placed(-0.5f, 0.3f, 1.0f)};
    batch.colors = {{0.0f, 1.0f, 0.0f, 0.1f}}; // alpha ignored: opaque
    data.meshes = {batch};
    vfx::SpriteBatch sprites;
    vfx::SpriteInstance s;
    s.center = Vec3(0.0f, 0.0f, 0.7f);
    s.right = Vec3(1.0f, 0.0f, 0.0f);
    s.up = Vec3(0.0f, 1.0f, 0.0f);
    s.color = {1.0f, 0.0f, 1.0f, 1.0f};
    sprites.instances = {s};
    data.sprites = {sprites};

    std::unique_ptr<gfx::rhi::ICommandList> cmd = device->CreateCommandList();
    target->AcquireNextImage();
    cmd->Reset();
    cmd->BeginRenderPass(*target, gfx::rhi::ClearColor{0.0f, 0.0f, 0.0f, 1.0f});
    pass.Draw(*cmd, data, vfx::ParticleCamera{}, Mat4::Identity(), Mat4::Identity(), 0);
    cmd->EndRenderPass();
    cmd->Close();
    device->WaitForFence(device->Submit(*cmd, target.get()));
    std::vector<u8> out;
    AETHER_CHECK(target->ReadBack(out) && out.size() == kSize * kSize * 4);
    target->Present(false);
    if (out.size() != kSize * kSize * 4) return;
    AETHER_CHECK(pass.Vertices().opaque.size() == 6);
    const auto at = [&](f32 ndc_x) { return (static_cast<usize>(kSize / 2) * kSize + static_cast<usize>((ndc_x * 0.5f + 0.5f) * kSize)) * 4; };
    const usize mesh_px = at(-0.5f), sprite_px = at(0.25f);
    // The mesh: pure green, opaque, not tinted by the sprite behind it. (The sprite's soft falloff still leaves magenta near its centre.)
    AETHER_CHECK(out[mesh_px + 1] > 200 && out[mesh_px] < 40 && out[mesh_px + 2] < 40);
    AETHER_CHECK(out[sprite_px] > 60 && out[sprite_px + 2] > 60 && out[sprite_px + 1] < 40);
}

AETHER_TEST(PlayerParticles_SoftFadeRidesPerVertexAndSplitsRuns) {
    // Two alpha batches with one texture, one soft (0.5) and one not, interleaved by depth; an additive soft batch with a bad value
    // (NaN: treated as off) and one negative; a ribbon (never soft).
    vfx::ParticleRenderData data;
    vfx::SpriteBatch soft, hard, nan_soft, negative;
    soft.soft_fade = 0.5f;
    soft.instances = {Sprite(-2.0f), Sprite(-6.0f)};
    hard.instances = {Sprite(-4.0f)};
    nan_soft.blend = vfx::BlendMode::Additive;
    nan_soft.soft_fade = std::nanf("");
    nan_soft.instances = {Sprite(-1.0f)};
    negative.blend = vfx::BlendMode::Additive;
    negative.soft_fade = -1.0f;
    negative.instances = {Sprite(-1.0f)};
    vfx::RibbonStrip ribbon;
    ribbon.vertices.resize(3);
    ribbon.indices = {0, 1, 2};
    data.sprites = {soft, hard, nan_soft, negative};
    data.ribbons = {ribbon};
    ParticleVertexStreams out;
    PackParticleVertices(data, vfx::ParticleCamera{}, out);
    AETHER_CHECK(out.alpha.size() == 21 && out.alpha_soft.size() == out.alpha.size());
    AETHER_CHECK(out.additive.size() == 12 && out.additive_soft.size() == out.additive.size());
    if (out.alpha_soft.size() != 21 || out.additive_soft.size() != 12) return;
    // Back to front: soft (-6), hard (-4), soft (-2), then the ribbon.
    AETHER_CHECK(Near(out.alpha_soft[0], 0.5f) && Near(out.alpha_soft[5], 0.5f) && Near(out.alpha_soft[6], 0.0f) && Near(out.alpha_soft[12], 0.5f));
    AETHER_CHECK(Near(out.alpha_soft[18], 0.0f) && Near(out.alpha_soft[20], 0.0f));
    for (f32 f : out.additive_soft) AETHER_CHECK(f == 0.0f);
    AETHER_CHECK(out.AnySoft());
    // Same texture, but the soft distance changes: four runs; without the soft array the texture alone makes one.
    std::vector<ParticleDrawRun> runs;
    ParticleTextureRuns(out.alpha, runs, &out.alpha_soft);
    AETHER_CHECK(runs.size() == 4);
    if (runs.size() == 4) {
        AETHER_CHECK(runs[0].vertex_count == 6 && Near(runs[0].soft_fade, 0.5f) && Near(runs[1].soft_fade, 0.0f));
        AETHER_CHECK(runs[2].first_vertex == 12 && Near(runs[2].soft_fade, 0.5f) && runs[3].first_vertex == 18 && runs[3].vertex_count == 3);
    }
    ParticleTextureRuns(out.alpha, runs);
    AETHER_CHECK(runs.size() == 1 && runs[0].soft_fade == 0.0f);
    // A size mismatch is ignored rather than read out of bounds.
    const std::vector<f32> short_soft(3, 1.0f);
    ParticleTextureRuns(out.alpha, runs, &short_soft);
    AETHER_CHECK(runs.size() == 1);
    out.Clear();
    AETHER_CHECK(!out.AnySoft() && out.alpha_soft.empty());
}

AETHER_TEST(PlayerParticles_SoftFadeReconstructsViewDistanceFromDepth) {
    // A perspective projection: the depth a view-space point lands at maps back to its distance; the fade ramps over soft_fade.
    const Mat4 projection = Mat4::PerspectiveRH(1.0f, 1.5f, 0.1f, 200.0f);
    const std::array<f32, 4> k = ParticleDepthToView(projection);
    const auto depth_at = [&](f32 distance) {
        const Vec4 clip = projection * Vec4(0.3f, -0.2f, -distance, 1.0f);
        return clip.z / clip.w;
    };
    for (f32 d : {0.5f, 3.0f, 17.0f, 150.0f}) AETHER_CHECK(Near(ParticleViewDistance(k, depth_at(d)), d, d * 1e-3f));
    const f32 scene = depth_at(10.0f);
    AETHER_CHECK(Near(ParticleSoftFade(k, scene, depth_at(9.0f), 2.0f), 0.5f, 1e-2f));
    AETHER_CHECK(Near(ParticleSoftFade(k, scene, depth_at(9.9f), 2.0f), 0.05f, 1e-2f));
    AETHER_CHECK(ParticleSoftFade(k, scene, depth_at(5.0f), 2.0f) == 1.0f);  // far enough in front: untouched
    AETHER_CHECK(ParticleSoftFade(k, scene, depth_at(11.0f), 2.0f) == 0.0f); // behind (the depth test drops it anyway)
    AETHER_CHECK(ParticleSoftFade(k, scene, depth_at(9.99f), 0.0f) == 1.0f); // off: hard edge
    // Identity (the clip-space tests below): the depth is the distance.
    const std::array<f32, 4> id = ParticleDepthToView(Mat4::Identity());
    AETHER_CHECK(Near(ParticleViewDistance(id, 0.25f), 0.25f) && Near(ParticleSoftFade(id, 0.8f, 0.5f, 0.6f), 0.5f));
}

AETHER_TEST(PlayerParticles_SoftFadeRoundTripsThroughTheEmitterJson) {
    vfx::Emitter emitter;
    vfx::SpriteRenderer sprite;
    sprite.soft_fade = 0.75f;
    emitter.render = {sprite};
    const nlohmann::json j = vfx::EmitterToJson(emitter);
    vfx::Emitter back;
    std::string error;
    AETHER_CHECK(vfx::EmitterFromJson(j, back, &error));
    AETHER_CHECK(back.render.size() == 1 && std::holds_alternative<vfx::SpriteRenderer>(back.render[0]));
    if (back.render.size() == 1 && std::holds_alternative<vfx::SpriteRenderer>(back.render[0])) {
        AETHER_CHECK(std::get<vfx::SpriteRenderer>(back.render[0]).soft_fade == 0.75f);
    }
    AETHER_CHECK(j.dump().find("\"soft_fade\":0.75") != std::string::npos);
}

static void PlayerParticles_SoftSpritesFadeNearOpaqueDepth(gfx::rhi::Backend backend) {
    std::unique_ptr<gfx::rhi::IDevice> device = MakeRenderDevice(backend);
    if (!device) return;
    constexpr u32 kSize = 64;
    std::unique_ptr<gfx::rhi::ISwapChain> target = device->CreateSwapChain(nullptr, kSize, kSize, 2);
    ParticlePass pass(*device, *target);
    AETHER_CHECK(pass.Valid());
    if (pass.SceneDepthTexture() == 0) {
        std::printf("    (this device can't copy and sample its depth format: soft particles are off)\n");
        return;
    }
    const u8 white[] = {255, 255, 255, 255};
    const gfx::rhi::SampledTextureHandle tex_white = device->CreateTexture(1, 1, white);
    AETHER_CHECK(tex_white.IsValid());
    pass.SetTextureResolver([&](const std::string& m) -> u32 { return m == "white" ? tex_white.index : 0u; });
    // An opaque black mesh quad tilted in depth across the screen (clip space, identity matrices): depth 0.5 + 0.4 x, so 0.1 on the left
    // and 0.9 on the right. A white sprite at depth 0.5 crosses it at x = 0: hidden left of that, in front right of it.
    ParticleMesh tilted;
    const Vec3 bl(-1.0f, -1.0f, 0.1f), br(1.0f, -1.0f, 0.9f), tr(1.0f, 1.0f, 0.9f), tl(-1.0f, 1.0f, 0.1f);
    tilted.positions = {bl, br, tr, bl, tr, tl};
    pass.SetMeshResolver([&](const std::string& m) -> const ParticleMesh* { return m == "tilted" ? &tilted : nullptr; });

    const auto draw = [&](f32 soft_fade, std::vector<u8>& out) {
        vfx::ParticleRenderData data;
        vfx::MeshBatch mesh;
        mesh.mesh = "tilted";
        mesh.transforms = {Mat4::Identity()};
        mesh.colors = {{0.0f, 0.0f, 0.0f, 1.0f}};
        data.meshes = {mesh};
        vfx::SpriteBatch batch;
        batch.material = "white";
        batch.soft_fade = soft_fade;
        vfx::SpriteInstance s;
        s.center = Vec3(0.0f, 0.0f, 0.5f);
        s.right = Vec3(0.95f, 0.0f, 0.0f);
        s.up = Vec3(0.0f, 0.5f, 0.0f);
        s.color = {1.0f, 1.0f, 1.0f, 1.0f};
        batch.instances = {s};
        data.sprites = {batch};
        std::unique_ptr<gfx::rhi::ICommandList> cmd = device->CreateCommandList();
        target->AcquireNextImage();
        cmd->Reset();
        cmd->BeginRenderPass(*target, gfx::rhi::ClearColor{0.0f, 0.0f, 0.0f, 1.0f});
        pass.Draw(*cmd, data, vfx::ParticleCamera{}, Mat4::Identity(), Mat4::Identity(), 0);
        cmd->EndRenderPass();
        cmd->Close();
        device->WaitForFence(device->Submit(*cmd, target.get()));
        const bool read = target->ReadBack(out);
        target->Present(false);
        return read && out.size() == kSize * kSize * 4;
    };
    std::vector<u8> hard, soft;
    AETHER_CHECK(draw(0.0f, hard));
    AETHER_CHECK(draw(0.3f, soft));
    if (hard.size() != kSize * kSize * 4 || soft.size() != kSize * kSize * 4) return;
    // Green channel (the same in RGBA and BGRA) along the middle row at pixel column `px`.
    const auto green = [&](const std::vector<u8>& img, u32 px) { return static_cast<int>(img[(static_cast<usize>(kSize / 2) * kSize + px) * 4 + 1]); };
    // Left of the intersection (column 28: x ~ -0.11) both are hidden behind the mesh.
    AETHER_CHECK(green(hard, 28) < 10 && green(soft, 28) < 10);
    // Hard: full white right after the intersection (column 34: x ~ 0.08, depth gap 0.03). Soft: the gap over 0.3 is ~0.1, faint.
    AETHER_CHECK(green(hard, 34) > 245);
    AETHER_CHECK(green(soft, 34) > 5 && green(soft, 34) < 80);
    // Soft ramps up with the depth gap, reaching full white once it passes 0.3 (column 56: x ~ 0.77, gap 0.31).
    int previous = -1;
    for (u32 px = 33; px <= 56; px += 3) {
        AETHER_CHECK(green(soft, px) + 2 >= previous);
        previous = green(soft, px);
        AETHER_CHECK(green(hard, px) > 245);
    }
    AETHER_CHECK(green(soft, 56) > 235);
    AETHER_CHECK(green(soft, 40) > green(soft, 34) + 30 && green(soft, 40) < 220);
}

AETHER_TEST(PlayerParticles_TexturedSpritesSampleTheirFlipbookCellOnVulkan) { PlayerParticles_TexturedSpritesSampleTheirFlipbookCell(gfx::rhi::Backend::Vulkan); }
AETHER_TEST(PlayerParticles_TexturedSpritesSampleTheirFlipbookCellOnD3D12) { PlayerParticles_TexturedSpritesSampleTheirFlipbookCell(gfx::rhi::Backend::D3D12); }

AETHER_TEST(PlayerParticles_EachSpriteSamplesItsOwnTextureInOneStreamOnVulkan) { PlayerParticles_EachSpriteSamplesItsOwnTextureInOneStream(gfx::rhi::Backend::Vulkan); }
AETHER_TEST(PlayerParticles_EachSpriteSamplesItsOwnTextureInOneStreamOnD3D12) { PlayerParticles_EachSpriteSamplesItsOwnTextureInOneStream(gfx::rhi::Backend::D3D12); }

AETHER_TEST(PlayerParticles_MeshParticlesDrawOpaqueAndOccludeSpritesBehindOnVulkan) { PlayerParticles_MeshParticlesDrawOpaqueAndOccludeSpritesBehind(gfx::rhi::Backend::Vulkan); }
AETHER_TEST(PlayerParticles_MeshParticlesDrawOpaqueAndOccludeSpritesBehindOnD3D12) { PlayerParticles_MeshParticlesDrawOpaqueAndOccludeSpritesBehind(gfx::rhi::Backend::D3D12); }

AETHER_TEST(PlayerParticles_SoftSpritesFadeNearOpaqueDepthOnVulkan) { PlayerParticles_SoftSpritesFadeNearOpaqueDepth(gfx::rhi::Backend::Vulkan); }
AETHER_TEST(PlayerParticles_SoftSpritesFadeNearOpaqueDepthOnD3D12) { PlayerParticles_SoftSpritesFadeNearOpaqueDepth(gfx::rhi::Backend::D3D12); }
