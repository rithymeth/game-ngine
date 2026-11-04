#pragma once

#include "aether/core/base.h"
#include "aether/gfx/rhi/types.h"

#include <string>
#include <vector>

namespace aether::gfx::rhi {
class IDevice;
}

namespace aether::gfx::parity {

// Backend parity (Phase 39.2, docs/design/UPGRADE_PLAN.md): the same scene drawn through every RHI backend that is
// available and compared with a tolerance. Each scene is a full-screen draw whose every pixel the CPU can compute
// exactly (ReferenceImage), so a backend is checked against the reference as well as against the other backends.
// Lavapipe covers Vulkan in CI, the Windows job covers D3D12, the macOS job covers MoltenVK.

// --- Comparing images --------------------------------------------------------------------------------------------
struct Tolerance {
    u32 max_channel_delta = 2;    // a pixel is bad when any channel differs by more than this (0..255)
    f64 max_bad_fraction = 0.0;   // the share of bad pixels a comparison may still call a match
};

struct ImageDiff {
    bool comparable = false; // both images are width x height RGBA8
    u32 width = 0, height = 0;
    u32 max_delta = 0;       // the largest channel difference anywhere
    f64 mean_delta = 0.0;    // the mean channel difference
    usize bad_pixels = 0;    // pixels with a channel past the tolerance
    u32 first_bad_x = 0, first_bad_y = 0;
    f64 BadFraction() const { return comparable && width && height ? static_cast<f64>(bad_pixels) / (static_cast<f64>(width) * height) : 1.0; }
};
// Compares two RGBA8 images (rows top to bottom). Not comparable when either size is not width * height * 4.
ImageDiff CompareImages(const std::vector<u8>& a, const std::vector<u8>& b, u32 width, u32 height, const Tolerance& tolerance);
bool Within(const ImageDiff& diff, const Tolerance& tolerance);
std::string Describe(const ImageDiff& diff); // "max 3, mean 0.41, 12 bad pixels (first at 4,7)"

// --- Scenes --------------------------------------------------------------------------------------------------------
enum class SceneKind : u8 { Flat, Gradient, Checker };
struct Scene {
    SceneKind kind = SceneKind::Flat;
    std::string name; // "flat", "gradient", "checker"
};
const std::vector<Scene>& Scenes();

// The pixels a correct backend produces, RGBA8, rows top to bottom.
std::vector<u8> ReferenceImage(const Scene& scene, u32 width, u32 height);
// The scene's HLSL (VSMain, PSMain) for a target of this size: a full-screen triangle, no resources.
std::string SceneHlsl(const Scene& scene, u32 width, u32 height);

// Draws the scene on an offscreen target and reads it back. False with a message where the device can't.
bool RenderScene(rhi::IDevice& device, const Scene& scene, u32 width, u32 height, std::vector<u8>& pixels, std::string* error = nullptr);

// --- Running ---------------------------------------------------------------------------------------------------------
struct BackendRun {
    rhi::Backend backend = rhi::Backend::Vulkan;
    bool available = false; // the build and the machine have this backend
    bool rendered = false;
    std::string error;
    std::vector<u8> pixels;
    ImageDiff vs_reference;
};
struct SceneReport {
    std::string scene;
    u32 width = 0, height = 0;
    std::vector<BackendRun> runs;
    // Every pair of backends that rendered, compared with each other.
    struct Pair {
        rhi::Backend a, b;
        ImageDiff diff;
    };
    std::vector<Pair> pairs;
    usize RenderedCount() const;
    // Every backend that rendered matches the reference, and every pair matches each other.
    bool Ok(const Tolerance& tolerance) const;
};
const char* BackendName(rhi::Backend backend);
SceneReport RunScene(const Scene& scene, u32 width, u32 height, const std::vector<rhi::Backend>& backends, const Tolerance& tolerance);

} // namespace aether::gfx::parity
