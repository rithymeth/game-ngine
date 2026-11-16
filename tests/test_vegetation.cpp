#include "../player/src/terrain_view.h"
#include "../player/src/vegetation_batch.h"
#include "../player/src/vegetation_pass.h"
#include "test_framework.h"

#include "aether/gfx/parity.h"

#include <cmath>
#include <cstdio>
#include <memory>
#include <set>
#include <vector>

using namespace aether;
using namespace aether::player;

// Vegetation for a ProceduralTerrain: the placeholder plant meshes, the batching into per-chunk meshes, the budget rules, and the pass on a real device.

namespace {

terrain::ScatterInstance At(f32 x, f32 y, f32 z, u32 rule, f32 scale = 1.0f, f32 yaw = 0.0f) {
    terrain::ScatterInstance i;
    i.position = Vec3(x, y, z);
    i.rotation = Quaternion::FromAxisAngle(Vec3(0, 1, 0), yaw);
    i.scale = scale;
    i.rule = rule;
    return i;
}

const std::vector<PlantKind> kAllKinds = {PlantKind::Pine, PlantKind::Birch, PlantKind::Shrub, PlantKind::Grass, PlantKind::Boulder, PlantKind::Reeds};

} // namespace

AETHER_TEST(Vegetation_EveryPlantMeshIsWellFormed) {
    for (u32 k = 0; k < kPlantKindCount; ++k) {
        const PlantKind kind = static_cast<PlantKind>(k);
        const PlantMesh& m = PlantPrototype(kind);
        std::printf("  %-8s %3zu vertices, %3zu triangles, %.1f m tall\n", PlantKindName(kind), m.vertices.size(), m.indices.size() / 3,
                    static_cast<double>(m.height));
        AETHER_CHECK(!m.vertices.empty() && !m.indices.empty() && m.indices.size() % 3 == 0);
        AETHER_CHECK(m.palette_span.size() == m.vertices.size());
        AETHER_CHECK(m.vertices.size() < 600); // a few hundred vertices a plant: thousands of them merge into megabytes, not hundreds
        for (const u32 i : m.indices) AETHER_CHECK(i < m.vertices.size());
        f32 min_y = 1e9f, max_y = -1e9f;
        for (const VegVertex& v : m.vertices) {
            const f32 len = std::sqrt(v.normal[0] * v.normal[0] + v.normal[1] * v.normal[1] + v.normal[2] * v.normal[2]);
            AETHER_CHECK(std::fabs(len - 1.0f) < 0.01f);
            AETHER_CHECK(v.uv[0] > 0.0f && v.uv[0] < 1.0f && v.uv[1] >= 0.0f && v.uv[1] <= 1.0f);
            min_y = std::min(min_y, v.position[1]);
            max_y = std::max(max_y, v.position[1]);
        }
        // Stands on the ground. A boulder and a shrub are bedded in a little on purpose (a rock sitting on a lawn looks dropped there): at most 0.3 m.
        AETHER_CHECK(min_y >= -0.3f && min_y < 0.5f);
        AETHER_CHECK(max_y <= m.height + 0.01f && max_y > m.height * 0.5f);
    }
}

AETHER_TEST(Vegetation_RigidPartsDoNotSwayAndTipsDo) {
    // A trunk and a rock stay put; the top of a tuft or a crown moves most.
    for (const PlantKind kind : {PlantKind::Boulder}) {
        for (const VegVertex& v : PlantPrototype(kind).vertices) AETHER_CHECK(v.uv[1] == 0.0f);
    }
    for (const PlantKind kind : {PlantKind::Grass, PlantKind::Reeds, PlantKind::Pine, PlantKind::Birch, PlantKind::Shrub}) {
        f32 most = 0.0f;
        for (const VegVertex& v : PlantPrototype(kind).vertices) most = std::max(most, v.uv[1]);
        AETHER_CHECK(most > 0.2f);
    }
    // The pine's lowest vertices (the trunk's foot) are rigid.
    bool foot_rigid = true;
    for (const VegVertex& v : PlantPrototype(PlantKind::Pine).vertices) {
        if (v.position[1] < 0.01f && v.uv[1] != 0.0f) foot_rigid = false;
    }
    AETHER_CHECK(foot_rigid);
}

AETHER_TEST(Vegetation_BatchingMergesPerChunkAndKindAndKeepsEveryVertexInsideItsBounds) {
    const f32 chunk = 32.0f;
    std::vector<terrain::ScatterInstance> inst = {At(5, 2, 5, 0), At(20, 3, 10, 0), At(40, 1, 5, 0), At(5, 2, 40, 3, 1.0f), At(6, 2, 41, 3, 1.5f, 1.0f), At(7, 2, 7, 4)};
    const std::vector<PlantKind> kinds = {PlantKind::Pine, PlantKind::Birch, PlantKind::Shrub, PlantKind::Grass, PlantKind::Boulder};
    const VegetationBatch b = BatchVegetation(inst, kinds, chunk);
    AETHER_CHECK(b.instances == 6 && b.chunks.size() == 4); // pines in (0,0) and (1,0), grass in (0,1), a boulder in (0,0)
    u64 vertices = 0;
    for (const VegChunk& c : b.chunks) {
        vertices += c.vertices.size();
        AETHER_CHECK(c.instances > 0 && c.indices.size() % 3 == 0);
        for (const u32 i : c.indices) AETHER_CHECK(i < c.vertices.size());
        for (const VegVertex& v : c.vertices) {
            AETHER_CHECK(v.position[0] >= c.bounds_min.x - 1e-3f && v.position[0] <= c.bounds_max.x + 1e-3f);
            AETHER_CHECK(v.position[1] >= c.bounds_min.y - 1e-3f && v.position[1] <= c.bounds_max.y + 1e-3f);
            AETHER_CHECK(v.position[2] >= c.bounds_min.z - 1e-3f && v.position[2] <= c.bounds_max.z + 1e-3f);
        }
    }
    AETHER_CHECK(vertices == b.vertices);
    const u64 expected = 3 * PlantPrototype(PlantKind::Pine).vertices.size() + 2 * PlantPrototype(PlantKind::Grass).vertices.size() +
                         PlantPrototype(PlantKind::Boulder).vertices.size();
    AETHER_CHECK(vertices == expected);
    AETHER_CHECK(b.instances_by_kind[static_cast<usize>(PlantKind::Pine)] == 3 && b.instances_by_kind[static_cast<usize>(PlantKind::Grass)] == 2);
    // An instance whose rule has no kind is left out, not a crash.
    const std::vector<terrain::ScatterInstance> orphan = {At(1, 1, 1, 9)};
    const VegetationBatch skipped = BatchVegetation(orphan, kinds, chunk);
    AETHER_CHECK(skipped.instances == 0 && skipped.chunks.empty());
}

AETHER_TEST(Vegetation_AnInstanceIsScaledTurnedAndPlacedOnTheGround) {
    const PlantMesh& pine = PlantPrototype(PlantKind::Pine);
    const std::vector<terrain::ScatterInstance> one = {At(100, 7, 50, 0, 2.0f, 1.2f)};
    const std::vector<PlantKind> pines = {PlantKind::Pine};
    const VegetationBatch b = BatchVegetation(one, pines, 32.0f);
    AETHER_CHECK(b.chunks.size() == 1);
    if (b.chunks.size() != 1) return;
    const VegChunk& c = b.chunks[0];
    AETHER_CHECK_NEAR(c.bounds_min.y, 7.0f, 0.05);                       // the foot is at the ground
    AETHER_CHECK_NEAR(c.bounds_max.y, 7.0f + pine.height * 2.0f, 0.05);  // twice the height
    AETHER_CHECK_NEAR((c.bounds_min.x + c.bounds_max.x) * 0.5f, 100.0f, pine.radius * 2.0f);
    // Turning keeps normals unit length.
    for (const VegVertex& v : c.vertices) {
        AETHER_CHECK(std::fabs(std::sqrt(v.normal[0] * v.normal[0] + v.normal[1] * v.normal[1] + v.normal[2] * v.normal[2]) - 1.0f) < 0.01f);
    }
}

AETHER_TEST(Vegetation_BatchingIsDeterministicAndPlantsTakeTheirOwnShade) {
    std::vector<terrain::ScatterInstance> inst;
    for (int i = 0; i < 60; ++i) inst.push_back(At(static_cast<f32>(i) * 1.7f, 0, static_cast<f32>(i) * 0.9f, 0));
    const std::vector<PlantKind> kinds = {PlantKind::Grass};
    const VegetationBatch a = BatchVegetation(inst, kinds, 32.0f), b = BatchVegetation(inst, kinds, 32.0f);
    AETHER_CHECK(a.chunks.size() == b.chunks.size() && a.vertices == b.vertices);
    bool same = a.chunks.size() == b.chunks.size();
    for (usize i = 0; same && i < a.chunks.size(); ++i) {
        same = a.chunks[i].vertices.size() == b.chunks[i].vertices.size() &&
               std::memcmp(a.chunks[i].vertices.data(), b.chunks[i].vertices.data(), a.chunks[i].vertices.size() * sizeof(VegVertex)) == 0;
    }
    AETHER_CHECK(same);
    // Sixty tufts do not all take one green.
    std::set<u32> columns;
    for (const VegChunk& c : a.chunks) {
        for (const VegVertex& v : c.vertices) columns.insert(static_cast<u32>(v.uv[0] * static_cast<f32>(kPaletteColumns)));
    }
    AETHER_CHECK(columns.size() >= 2 && columns.size() <= 3);
}

AETHER_TEST(Vegetation_TheBudgetCapsEachCategoryAndFollowsTheDensity) {
    ProceduralTerrain t;
    const VegetationRules full = MakeVegetationRules(t);
    AETHER_CHECK(full.rules.size() == full.kinds.size() && full.rules.size() == 6);
    u32 trees = 0, plants = 0, rocks = 0;
    for (usize i = 0; i < full.rules.size(); ++i) {
        const u32 cap = full.rules[i].max_instances;
        switch (full.kinds[i]) {
        case PlantKind::Pine:
        case PlantKind::Birch: trees += cap; break;
        case PlantKind::Boulder: rocks += cap; break;
        default: plants += cap; break;
        }
    }
    AETHER_CHECK(trees <= 1500 && plants <= 30000 && rocks <= 1000);
    t.vegetation_density = 0.5f;
    const VegetationRules half = MakeVegetationRules(t);
    for (usize i = 0; i < half.rules.size(); ++i) AETHER_CHECK(half.rules[i].max_instances * 2 <= full.rules[i].max_instances + 1);
    t.vegetation_density = 0.0f;
    for (const terrain::ScatterRule& r : MakeVegetationRules(t).rules) AETHER_CHECK(r.max_instances == 0);
    // A temperate valley has no cacti.
    for (const terrain::ScatterRule& r : full.rules) AETHER_CHECK(r.name != "Cactus" && r.name != "DeadTree" && r.name != "SnowRock");
}

AETHER_TEST(Vegetation_DrawDistancesRunFromGrassOutToTrees) {
    AETHER_CHECK(VegetationDrawDistance(PlantKind::Grass) < VegetationDrawDistance(PlantKind::Reeds));
    AETHER_CHECK(VegetationDrawDistance(PlantKind::Reeds) < VegetationDrawDistance(PlantKind::Shrub));
    AETHER_CHECK(VegetationDrawDistance(PlantKind::Shrub) < VegetationDrawDistance(PlantKind::Boulder));
    AETHER_CHECK(VegetationDrawDistance(PlantKind::Boulder) < VegetationDrawDistance(PlantKind::Pine));
}

AETHER_TEST(Vegetation_OnlyTheHdrShaderDefinesTheHdrTarget) {
    // (The shader compiling is shown by the real-device test below; this is the one switch between the two outputs.)
    AETHER_CHECK(VegetationShaderSource(false).find("#define AETHER_HDR_TARGET") == std::string::npos);
    AETHER_CHECK(VegetationShaderSource(true).rfind("#define AETHER_HDR_TARGET 1", 0) == 0);
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

std::vector<u8> Render(gfx::rhi::IDevice& device, gfx::rhi::ISwapChain& target, TerrainView& view, const Vec3& eye, const Mat4& vp, f32 time) {
    std::unique_ptr<gfx::rhi::ICommandList> cmd = device.CreateCommandList();
    target.AcquireNextImage();
    cmd->Reset();
    cmd->BeginRenderPass(target, {0.0f, 0.0f, 0.0f, 1.0f});
    view.Draw(*cmd, vp, eye, Vec3(-0.4f, 0.8f, -0.35f), 3.0f, time);
    cmd->EndRenderPass();
    cmd->Close();
    device.WaitForFence(device.Submit(*cmd, &target));
    std::vector<u8> pixels;
    target.ReadBack(pixels);
    target.Present(false);
    return pixels;
}

} // namespace

AETHER_TEST(Vegetation_PlantsAppearOverTheGroundAndSwayWithTime) {
    constexpr u32 kW = 192, kH = 128;
    std::vector<NamedDevice> devices = Devices();
    if (devices.empty()) std::printf("    (no RHI backend here: install a driver, or Mesa's lavapipe, to run this)\n");
    for (NamedDevice& named : devices) {
        gfx::rhi::IDevice& device = *named.device;
        std::unique_ptr<gfx::rhi::ISwapChain> target = device.CreateSwapChain(nullptr, kW, kH, 2);
        ProceduralTerrain bare;
        bare.samples = 129;
        bare.height_scale = 30.0f;
        bare.hydraulic_droplets = 3000;
        ProceduralTerrain planted = bare;
        planted.vegetation = true;

        const Vec3 eye(64, 26, 120);
        const Mat4 vp = Mat4::PerspectiveRH(60.0f * 3.14159265f / 180.0f, static_cast<f32>(kW) / kH, 0.1f, 600.0f) *
                        Mat4::LookAtRH(eye, Vec3(64, 6, 50), Vec3(0, 1, 0));
        TerrainView ground_only(device, *target, false), with_plants(device, *target, false);
        AETHER_CHECK(ground_only.Valid() && with_plants.Valid());
        if (!ground_only.Valid() || !with_plants.Valid()) continue;
        AETHER_CHECK(ground_only.Update(bare, eye) && with_plants.Update(planted, eye));
        const auto stats = with_plants.GetStats();
        std::printf("    %s: %u plants, %u meshes, %llu vertices, placed in %.0f ms\n", named.name, stats.plants, stats.plant_meshes,
                    static_cast<unsigned long long>(stats.plant_vertices), stats.plants_build_ms);
        AETHER_CHECK(ground_only.GetStats().plants == 0);
        AETHER_CHECK(stats.plants > 100 && stats.plant_meshes > 0);

        const std::vector<u8> a = Render(device, *target, ground_only, eye, vp, 0.0f);
        const std::vector<u8> b = Render(device, *target, with_plants, eye, vp, 0.0f);
        const std::vector<u8> c = Render(device, *target, with_plants, eye, vp, 1.3f);
        AETHER_CHECK(a.size() == b.size() && b.size() == c.size() && a.size() == static_cast<usize>(kW) * kH * 4);
        if (a.size() != b.size() || b.size() != c.size() || a.empty()) continue;
        usize plants_differ = 0, sway_differs = 0;
        for (usize i = 0; i < a.size(); i += 4) {
            if (std::abs(a[i] - b[i]) + std::abs(a[i + 1] - b[i + 1]) + std::abs(a[i + 2] - b[i + 2]) > 12) ++plants_differ;
            if (std::abs(b[i] - c[i]) + std::abs(b[i + 1] - c[i + 1]) + std::abs(b[i + 2] - c[i + 2]) > 12) ++sway_differs;
        }
        const usize total = static_cast<usize>(kW) * kH;
        std::printf("    %s: plants change %zu%% of the picture; 1.3 s of wind changes %zu%%; %u of %u meshes drawn\n", named.name, plants_differ * 100 / total,
                    sway_differs * 100 / total, with_plants.GetStats().plant_meshes_drawn, stats.plant_meshes);
        AETHER_CHECK(plants_differ * 100 / total >= 2);  // plants cover some of the view
        AETHER_CHECK(sway_differs > 0);                   // and move with time
        AETHER_CHECK(with_plants.GetStats().plant_meshes_drawn > 0 && with_plants.GetStats().plant_meshes_drawn <= stats.plant_meshes);
    }
}
