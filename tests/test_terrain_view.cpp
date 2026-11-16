#include "../player/src/terrain_view.h"
#include "test_framework.h"

#include "aether/gfx/parity.h"
#include "aether/gfx/rhi/device.h"

#include <cstdio>
#include <memory>
#include <set>
#include <vector>

using namespace aether;
using namespace aether::player;

// The player's drawing of a ProceduralTerrain: the settings it stands for, and the terrain through a real device (a driver, or Mesa's lavapipe).

AETHER_TEST(TerrainView_SettingsAreClampedToWhatTheGeneratorCanDo) {
    ProceduralTerrain t;
    t.samples = 3;
    AETHER_CHECK(ToGenSettings(t).width == 9 && ToGenSettings(t).height == 9);
    t.samples = 4000;
    AETHER_CHECK(ToGenSettings(t).width == 1025);
    t.samples = 257;
    t.seed = 99;
    t.height_scale = 80.0f;
    const terrain::TerrainGenSettings s = ToGenSettings(t);
    AETHER_CHECK(s.width == 257 && s.seed == 99 && s.height_scale == 80.0f);
}

namespace {

struct NamedDevice {
    const char* name;
    std::unique_ptr<gfx::rhi::IDevice> device;
};

std::vector<NamedDevice> Devices() {
    std::vector<NamedDevice> out;
    for (const gfx::rhi::Backend backend : {gfx::rhi::Backend::D3D12, gfx::rhi::Backend::Vulkan}) {
        if (!gfx::rhi::IsBackendAvailable(backend)) continue;
        try {
            if (auto device = gfx::rhi::CreateDevice(backend, false)) out.push_back({gfx::parity::BackendName(backend), std::move(device)});
        } catch (const std::exception&) {
        }
    }
    return out;
}

} // namespace

AETHER_TEST(TerrainView_DrawsAGeneratedTerrainThroughARealDevice) {
    constexpr u32 kW = 192, kH = 128;
    std::vector<NamedDevice> devices = Devices();
    if (devices.empty()) std::printf("    (no RHI backend here: install a driver, or Mesa's lavapipe, to run this)\n");
    for (NamedDevice& named : devices) {
        gfx::rhi::IDevice& device = *named.device;
        std::unique_ptr<gfx::rhi::ISwapChain> target = device.CreateSwapChain(nullptr, kW, kH, 2);
        TerrainView view(device, *target, false);
        AETHER_CHECK(view.Valid());
        if (!view.Valid()) continue;

        ProceduralTerrain terrain;
        terrain.samples = 129; // 128 m, four chunks a side
        terrain.height_scale = 30.0f;
        terrain.hydraulic_droplets = 3000;
        const Vec3 eye(64, 110, 150);
        AETHER_CHECK(view.Update(terrain, eye));
        AETHER_CHECK(view.GetStats().builds == 1);
        // The same terrain again is not generated again; a different seed is.
        AETHER_CHECK(view.Update(terrain, eye) && view.GetStats().builds == 1);
        ProceduralTerrain other = terrain;
        other.seed = 7;
        AETHER_CHECK(view.Update(other, eye) && view.GetStats().builds == 2);
        AETHER_CHECK(view.Update(terrain, eye) && view.GetStats().builds == 3);

        const Mat4 proj = Mat4::PerspectiveRH(60.0f * 3.14159265f / 180.0f, static_cast<f32>(kW) / kH, 0.1f, 600.0f);
        const Mat4 vp = proj * Mat4::LookAtRH(eye, Vec3(64, 5, 40), Vec3(0, 1, 0));

        std::unique_ptr<gfx::rhi::ICommandList> cmd = device.CreateCommandList();
        target->AcquireNextImage();
        cmd->Reset();
        cmd->BeginRenderPass(*target, {0.0f, 0.0f, 0.0f, 1.0f});
        const u32 drawn = view.Draw(*cmd, vp, eye, Vec3(-0.4f, 0.8f, -0.35f), 3.0f);
        cmd->EndRenderPass();
        cmd->Close();
        device.WaitForFence(device.Submit(*cmd, target.get()));
        std::vector<u8> pixels;
        AETHER_CHECK(target->ReadBack(pixels));
        target->Present(false);
        AETHER_CHECK(drawn > 0 && pixels.size() == static_cast<usize>(kW) * kH * 4);
        if (pixels.size() != static_cast<usize>(kW) * kH * 4) continue;

        // Ground fills the view: most pixels are not the black clear colour, and the image is not one flat colour.
        usize lit = 0;
        std::set<u32> colours;
        for (usize i = 0; i < pixels.size(); i += 4) {
            if (pixels[i] > 3 || pixels[i + 1] > 3 || pixels[i + 2] > 3) ++lit;
            colours.insert((static_cast<u32>(pixels[i]) >> 3) << 16 | (static_cast<u32>(pixels[i + 1]) >> 3) << 8 | (static_cast<u32>(pixels[i + 2]) >> 3));
        }
        std::printf("    %s: %u chunks, %llu triangles, %zu%% of pixels covered, %zu colours, built in %.0f ms\n", named.name, drawn,
                    static_cast<unsigned long long>(view.GetStats().triangles_drawn), lit * 100 / (static_cast<usize>(kW) * kH), colours.size(),
                    view.GetStats().build_ms);
        AETHER_CHECK(lit * 100 / (static_cast<usize>(kW) * kH) >= 40);
        AETHER_CHECK(colours.size() >= 8);
    }
}
