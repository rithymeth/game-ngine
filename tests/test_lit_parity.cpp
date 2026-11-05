#include "aether/renderer/lit_parity.h"
#include "test_framework.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

using namespace aether;
using namespace aether::gfx;

// Phase 39.2/39.3/39.4: the lit-material scenes. The CPU reference is a C++ port of the HLSL; on a machine with an RHI backend the scene is
// drawn and compared with it. Set AETHER_PARITY_OUT to a directory to keep the screenshots (BMP) and a timing report.

namespace {

bool Contains(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

} // namespace

AETHER_TEST(LitParity_ReferencesAreFiniteAndVary) {
    constexpr u32 kW = 48, kH = 32;
    const std::vector<parity::Scene>& scenes = mat::LitParityScenes();
    AETHER_CHECK(scenes.size() == 6);
    for (const parity::Scene& scene : scenes) {
        AETHER_CHECK(scene.kind == parity::SceneKind::Custom && Contains(scene.name, "lit_"));
        const std::vector<u8> ref = parity::ReferenceImage(scene, kW, kH);
        AETHER_CHECK(ref.size() == kW * kH * 4);
        // Not flat in either direction: the light angle changes it across, the material parameter down (the clear coat's attenuation channel only
        // moves across, so compare per row and per column over all channels).
        u32 min_v = 255, max_v = 0;
        for (usize i = 0; i < ref.size(); ++i) {
            if (i % 4 == 3) continue;
            min_v = std::min<u32>(min_v, ref[i]);
            max_v = std::max<u32>(max_v, ref[i]);
        }
        AETHER_CHECK(max_v - min_v >= 8);
        AETHER_CHECK(ref[3] == 255 && ref[ref.size() - 1] == 255);
        // The shader is the library plus one full-screen draw with the size baked in.
        const std::string hlsl = parity::SceneHlsl(scene, kW, kH);
        AETHER_CHECK(Contains(hlsl, "AETHER_ADVANCED_PBR_HLSL") && Contains(hlsl, "AETHER_SHADING_MODELS_HLSL"));
        AETHER_CHECK(Contains(hlsl, "pos.x / 48.0") && Contains(hlsl, "pos.y / 32.0") && Contains(hlsl, "PSMain") && Contains(hlsl, "VSMain"));
    }
    // The reference is deterministic.
    AETHER_CHECK(parity::ReferenceImage(scenes[0], kW, kH) == parity::ReferenceImage(scenes[0], kW, kH));
}

AETHER_TEST(LitParity_WriteBmpRoundTripsTheHeaderAndPixels) {
    const std::vector<u8> rgba = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 10, 20, 30, 255, 40, 50, 60, 255, 70, 80, 90, 255};
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "aether_parity_test.bmp";
    AETHER_CHECK(parity::WriteBmp(path.string(), rgba, 3, 2));
    std::vector<unsigned char> file;
    {
        // Closed before the remove below: Windows refuses to delete a file that is still open, and remove() throws.
        std::ifstream in(path, std::ios::binary);
        file.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    AETHER_CHECK(file.size() == 54 + 12 * 2); // 3 pixels * 3 bytes = 9, padded to 12, two rows
    if (file.size() != 54 + 12 * 2) {         // a short read must fail the test, not index past the end
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        return;
    }
    AETHER_CHECK(file[0] == 'B' && file[1] == 'M' && file[18] == 3 && file[22] == 2 && file[28] == 24);
    // The first stored row is the image's bottom row, in BGR: (10, 20, 30) -> 30, 20, 10.
    AETHER_CHECK(file[54] == 30 && file[55] == 20 && file[56] == 10);
    AETHER_CHECK(file[54 + 12] == 0 && file[54 + 13] == 0 && file[54 + 14] == 255); // then the top row: red first
    AETHER_CHECK(!parity::WriteBmp(path.string(), rgba, 4, 2)); // the size doesn't match the pixels
    std::filesystem::remove(path);
}

AETHER_TEST(LitParity_AvailableBackendsMatchTheCppPort) {
    constexpr u32 kW = 128, kH = 96;
    const parity::Tolerance tolerance{8, 0.01};
    const std::vector<rhi::Backend> backends = {rhi::Backend::D3D12, rhi::Backend::Vulkan};
    const char* out_dir = std::getenv("AETHER_PARITY_OUT");
    std::ofstream report;
    if (out_dir && *out_dir) {
        std::filesystem::create_directories(out_dir);
        report.open(std::filesystem::path(out_dir) / "parity-report.txt");
        report << "scene backend frame_ms max_delta mean_delta bad_pixels\n";
    }
    usize rendered_scenes = 0;
    for (const parity::Scene& scene : mat::LitParityScenes()) {
        const parity::SceneReport result = parity::RunScene(scene, kW, kH, backends, tolerance);
        if (out_dir && *out_dir) {
            parity::WriteBmp((std::filesystem::path(out_dir) / (scene.name + "-reference.bmp")).string(), parity::ReferenceImage(scene, kW, kH), kW, kH);
        }
        for (const parity::BackendRun& run : result.runs) {
            if (!run.available) continue;
            if (!run.rendered) {
                std::printf("    %s on %s did not render: %s\n", scene.name.c_str(), parity::BackendName(run.backend), run.error.c_str());
                continue;
            }
            std::printf("    %s on %s: %s, %.2f ms a frame\n", scene.name.c_str(), parity::BackendName(run.backend),
                        parity::Describe(run.vs_reference).c_str(), run.frame_ms);
            if (out_dir && *out_dir) {
                parity::WriteBmp((std::filesystem::path(out_dir) / (scene.name + "-" + parity::BackendName(run.backend) + ".bmp")).string(), run.pixels, kW, kH);
                report << scene.name << ' ' << parity::BackendName(run.backend) << ' ' << run.frame_ms << ' ' << run.vs_reference.max_delta << ' '
                       << run.vs_reference.mean_delta << ' ' << run.vs_reference.bad_pixels << '\n';
            }
        }
        for (const parity::SceneReport::Pair& pair : result.pairs) {
            if (!parity::Within(pair.diff, tolerance)) {
                std::printf("    %s: %s vs %s: %s\n", scene.name.c_str(), parity::BackendName(pair.a), parity::BackendName(pair.b),
                            parity::Describe(pair.diff).c_str());
            }
        }
        AETHER_CHECK(result.Ok(tolerance));
        if (result.RenderedCount() > 0) ++rendered_scenes;
    }
    if (rendered_scenes == 0) std::printf("    (no RHI backend here: install a driver, or Mesa's lavapipe, to compare backends)\n");
}
