#include "aether/gfx/parity.h"
#include "test_framework.h"

#include "aether/gfx/draw_submission.h"

#include "aether/math/math.h"
#include "aether/gfx/pipeline_cache.h"
#include "aether/gfx/shader_cache.h"

#include <cstdio>

using namespace aether;
using namespace aether::gfx;

// Phase 39.2: backend parity. The comparison and the CPU reference images run everywhere; the GPU comparison runs on every RHI backend
// the machine has (Vulkan on lavapipe in CI, D3D12 on the Windows job) and skips where there is none.

namespace {

const parity::Scene& SceneNamed(const char* name) {
    for (const parity::Scene& s : parity::Scenes()) {
        if (s.name == name) return s;
    }
    static const parity::Scene none;
    return none;
}

const u8* Pixel(const std::vector<u8>& rgba, u32 width, u32 x, u32 y) { return &rgba[(static_cast<usize>(y) * width + x) * 4]; }

} // namespace

AETHER_TEST(Parity_CompareImagesCountsBadPixelsAgainstTheTolerance) {
    const parity::Tolerance tolerance{2, 0.0};
    std::vector<u8> a(4 * 4 * 4, 100), b = a;
    parity::ImageDiff same = parity::CompareImages(a, b, 4, 4, tolerance);
    AETHER_CHECK(same.comparable && same.max_delta == 0 && same.bad_pixels == 0 && parity::Within(same, tolerance));

    b[5 * 4 + 1] = 102; // within the tolerance
    parity::ImageDiff near = parity::CompareImages(a, b, 4, 4, tolerance);
    AETHER_CHECK(near.max_delta == 2 && near.bad_pixels == 0 && parity::Within(near, tolerance));
    AETHER_CHECK(near.mean_delta > 0.0);

    b[9 * 4 + 2] = 110; // pixel 9 = (1, 2): past it
    parity::ImageDiff off = parity::CompareImages(a, b, 4, 4, tolerance);
    AETHER_CHECK(off.max_delta == 10 && off.bad_pixels == 1 && off.first_bad_x == 1 && off.first_bad_y == 2);
    AETHER_CHECK(!parity::Within(off, tolerance));
    // One bad pixel of sixteen is a match once the tolerance allows that share.
    AETHER_CHECK(parity::Within(off, parity::Tolerance{2, 0.07}));
    AETHER_CHECK(!parity::Within(off, parity::Tolerance{2, 0.05}));
    AETHER_CHECK(parity::Describe(off).find("1 bad pixels (first at 1,2)") != std::string::npos);

    // A different size, or a size that doesn't match the pixels, can't be compared.
    std::vector<u8> small(2 * 2 * 4, 100);
    AETHER_CHECK(!parity::CompareImages(a, small, 4, 4, tolerance).comparable);
    AETHER_CHECK(!parity::Within(parity::CompareImages(a, small, 4, 4, tolerance), parity::Tolerance{255, 1.0}));
    AETHER_CHECK(!parity::CompareImages(a, b, 0, 4, tolerance).comparable);
}

AETHER_TEST(Parity_ReferenceImagesAreExact) {
    constexpr u32 kW = 32, kH = 16;
    const std::vector<u8> flat = parity::ReferenceImage(SceneNamed("flat"), kW, kH);
    AETHER_CHECK(flat.size() == kW * kH * 4);
    AETHER_CHECK(Pixel(flat, kW, 0, 0)[0] == 51 && Pixel(flat, kW, 31, 15)[1] == 102 && Pixel(flat, kW, 7, 3)[2] == 153);
    AETHER_CHECK(Pixel(flat, kW, 5, 5)[3] == 255);

    // The gradient reads the pixel centres: red grows across, green grows down, blue stays at a quarter.
    const std::vector<u8> grad = parity::ReferenceImage(SceneNamed("gradient"), kW, kH);
    AETHER_CHECK(Pixel(grad, kW, 0, 0)[0] == 4 && Pixel(grad, kW, 31, 0)[0] == 251);   // (0.5/32, 31.5/32) * 255
    AETHER_CHECK(Pixel(grad, kW, 0, 0)[1] == 8 && Pixel(grad, kW, 0, 15)[1] == 247);   // (0.5/16, 15.5/16) * 255
    AETHER_CHECK(Pixel(grad, kW, 10, 10)[2] == 64);

    // The checker flips every 8 pixels, dark in the top-left cell.
    const std::vector<u8> check = parity::ReferenceImage(SceneNamed("checker"), kW, kH);
    AETHER_CHECK(Pixel(check, kW, 0, 0)[0] == 26 && Pixel(check, kW, 7, 7)[0] == 26);
    AETHER_CHECK(Pixel(check, kW, 8, 0)[0] == 230 && Pixel(check, kW, 0, 8)[0] == 230);
    AETHER_CHECK(Pixel(check, kW, 8, 8)[0] == 26);
}

AETHER_TEST(Parity_SceneHlslIsOneFullScreenTriangleWithTheSize) {
    for (const parity::Scene& scene : parity::Scenes()) {
        const std::string hlsl = parity::SceneHlsl(scene, 64, 48);
        AETHER_CHECK(hlsl.find("VSMain") != std::string::npos && hlsl.find("PSMain") != std::string::npos);
        AETHER_CHECK(hlsl.find("SV_VertexID") != std::string::npos);
    }
    const std::string grad = parity::SceneHlsl(SceneNamed("gradient"), 64, 48);
    AETHER_CHECK(grad.find("pos.x / 64.0") != std::string::npos && grad.find("pos.y / 48.0") != std::string::npos);
    AETHER_CHECK(parity::Scenes().size() == 3);
}

AETHER_TEST(Parity_AvailableBackendsMatchTheReferenceAndEachOther) {
    constexpr u32 kW = 64, kH = 48;
    const parity::Tolerance tolerance{2, 0.0};
    const std::vector<rhi::Backend> backends = {rhi::Backend::D3D12, rhi::Backend::Vulkan};
    usize rendered_scenes = 0;
    for (const parity::Scene& scene : parity::Scenes()) {
        const parity::SceneReport report = parity::RunScene(scene, kW, kH, backends, tolerance);
        for (const parity::BackendRun& run : report.runs) {
            if (!run.available) continue;
            if (!run.rendered || !parity::Within(run.vs_reference, tolerance)) {
                std::printf("    %s on %s: %s %s\n", scene.name.c_str(), parity::BackendName(run.backend),
                            run.rendered ? parity::Describe(run.vs_reference).c_str() : "did not render:", run.error.c_str());
            }
        }
        for (const parity::SceneReport::Pair& pair : report.pairs) {
            if (!parity::Within(pair.diff, tolerance)) {
                std::printf("    %s: %s vs %s: %s\n", scene.name.c_str(), parity::BackendName(pair.a), parity::BackendName(pair.b),
                            parity::Describe(pair.diff).c_str());
            }
        }
        AETHER_CHECK(report.Ok(tolerance));
        if (report.RenderedCount() > 0) ++rendered_scenes;
    }
    if (rendered_scenes == 0) std::printf("    (no RHI backend here: install a driver, or Mesa's lavapipe, to compare backends)\n");
}

// Phase 38 step 8: the shader cache. The compiler is injected, so this needs no DXC -- the fake
// counts how many times it was asked, which is the whole behaviour worth checking.
AETHER_TEST(Gfx_ShaderCacheCompilesOncePerKey) {
    usize compiles = 0;
    gfx::ShaderCache cache([&compiles](const gfx::ShaderKey& key) {
        ++compiles;
        gfx::ShaderBytecode bytecode;
        bytecode.data.assign(key.source.begin(), key.source.end()); // distinguishable per source
        return bytecode;
    });

    const gfx::ShaderKey key{"float4 main() : SV_Target { return 0; }", "main", "ps_6_0", gfx::ShaderBackend::Vulkan};
    const gfx::ShaderBytecode& first = cache.Get(key);
    AETHER_CHECK(compiles == 1 && cache.Misses() == 1 && cache.Hits() == 0 && cache.Size() == 1);
    const gfx::ShaderBytecode& second = cache.Get(key);
    AETHER_CHECK(compiles == 1 && cache.Hits() == 1); // the same key compiles once
    AETHER_CHECK(first.data == second.data && cache.Size() == 1);

    // Every part of the key is part of the identity: change one and it is a different shader.
    gfx::ShaderKey other = key;
    other.entry_point = "other";
    cache.Get(other);
    AETHER_CHECK(compiles == 2);
    other = key;
    other.profile = "ps_5_0";
    cache.Get(other);
    AETHER_CHECK(compiles == 3);
    other = key;
    other.backend = gfx::ShaderBackend::D3D12;
    cache.Get(other);
    AETHER_CHECK(compiles == 4);
    other = key;
    other.source = "float4 main() : SV_Target { return 1; }";
    cache.Get(other);
    AETHER_CHECK(compiles == 5 && cache.Size() == 5);

    // A compile that produced nothing is cached too: a broken shader stays broken, and asking again
    // must not turn one error into a log flood.
    const gfx::ShaderKey broken{"this is not hlsl", "main", "ps_6_0", gfx::ShaderBackend::D3D12};
    cache.Get(broken);
    cache.Get(broken);
    AETHER_CHECK(compiles == 6);

    cache.Clear();
    AETHER_CHECK(cache.Size() == 0);
}
// Phase 38 step 8: the pipeline cache. The creator is injected, so this needs no GPU -- the fake
// numbers the pipelines it makes, which is enough to see that each state is created once.
AETHER_TEST(Gfx_PipelineCacheCreatesOncePerState) {
    usize created = 0;
    gfx::PipelineCache<u64> cache([&created](const gfx::PipelineKey&) { return ++created; });

    const gfx::PipelineKey opaque{"vs", "ps", gfx::ShaderBackend::Vulkan, true, true, false, {1}};
    const u64 first = cache.Get(opaque);
    AETHER_CHECK(first == 1 && created == 1 && cache.Misses() == 1 && cache.Hits() == 0);
    AETHER_CHECK(cache.Get(opaque) == first && created == 1 && cache.Hits() == 1);

    // Blend, depth and the target formats are all part of the identity: the same shaders with
    // different state are a different pipeline.
    gfx::PipelineKey blended = opaque;
    blended.blend = true;
    AETHER_CHECK(cache.Get(blended) == 2 && created == 2);
    gfx::PipelineKey no_write = opaque;
    no_write.depth_write = false;
    AETHER_CHECK(cache.Get(no_write) == 3 && created == 3);
    gfx::PipelineKey other_target = opaque;
    other_target.render_target_formats = {2};
    AETHER_CHECK(cache.Get(other_target) == 4 && created == 4);

    // A pipeline that could not be created is cached too, so a bad state is reported once instead of
    // being retried on every draw.
    usize attempts = 0;
    gfx::PipelineCache<u64> failing([&attempts](const gfx::PipelineKey&) {
        ++attempts;
        return u64(0);
    });
    AETHER_CHECK(failing.Get(opaque) == 0 && failing.Get(opaque) == 0 && attempts == 1);

    cache.Clear();
    AETHER_CHECK(cache.Size() == 0 && cache.Hits() == 0 && cache.Misses() == 0);
}
// Phase 38 step 8: draw submission. Culling uses the frustum from math/frustum.h, ordering is by
// material so the pipeline changes as few times as possible, and the submitter is a fake, so this
// needs no GPU.
AETHER_TEST(Gfx_DrawSubmissionCullsAndOrdersByMaterial) {
    const Mat4 view_projection = Mat4::PerspectiveRH(Radians(60.0f), 16.0f / 9.0f, 0.1f, 100.0f);
    const Frustum frustum = Frustum::FromViewProjection(view_projection);

    gfx::DrawSubmission submission;
    submission.Begin();
    submission.Add({"mesh-a", "z-material", Vec3(0.0f, 0.0f, -5.0f), 0.5f, 1});
    submission.Add({"mesh-b", "a-material", Vec3(0.0f, 0.0f, -6.0f), 0.5f, 2});
    submission.Add({"mesh-c", "a-material", Vec3(0.0f, 0.0f, -7.0f), 0.5f, 3});
    submission.Add({"behind", "a-material", Vec3(0.0f, 0.0f, 50.0f), 0.5f, 4}); // behind the camera
    AETHER_CHECK(submission.Queued() == 4);

    std::vector<u64> order;
    const usize drawn = submission.Submit(frustum, [&order](const gfx::DrawItem& item) { order.push_back(item.user); });
    AETHER_CHECK(drawn == 3);
    AETHER_CHECK(submission.Submitted() == 3 && submission.Culled() == 1);

    // a-material before z-material, and within a material by mesh: mesh-b, mesh-c, then mesh-a.
    AETHER_CHECK(order.size() == 3);
    AETHER_CHECK(order[0] == 2 && order[1] == 3 && order[2] == 1);

    // Begin starts a fresh frame rather than piling onto the last one.
    submission.Begin();
    AETHER_CHECK(submission.Queued() == 0 && submission.Submitted() == 0 && submission.Culled() == 0);
    AETHER_CHECK(submission.Submit(frustum, {}) == 0); // no items, and a null submitter is fine
}