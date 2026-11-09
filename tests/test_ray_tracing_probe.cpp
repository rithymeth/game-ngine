#include "aether/core/log.h"
#include "aether/gfx/rhi/device.h"
#include "test_framework.h"

#include <cstdio>
#include <memory>
#include <stdexcept>

using namespace aether;
namespace rhi = aether::gfx::rhi;

// IDevice::SupportsRayTracing is a probe: it must answer without throwing, and say the same thing every time. What the answer is depends on
// the GPU, so the test reports it rather than demanding it.

static void RayTracingProbe_AnswersConsistently(rhi::Backend backend, const char* name) {
    if (!rhi::IsBackendAvailable(backend)) return;
    std::unique_ptr<rhi::IDevice> device;
    try {
        device = rhi::CreateDevice(backend, false);
    } catch (const std::exception&) {
    }
    if (!device) return;
    const bool first = device->SupportsRayTracing();
    AETHER_CHECK(device->SupportsRayTracing() == first);
    std::fprintf(stderr, "[ray tracing] %s: %s\n", name, first ? "supported" : "not supported");
}

AETHER_TEST(RayTracingProbe_AnswersConsistently_D3D12) { RayTracingProbe_AnswersConsistently(rhi::Backend::D3D12, "D3D12"); }
AETHER_TEST(RayTracingProbe_AnswersConsistently_Vulkan) { RayTracingProbe_AnswersConsistently(rhi::Backend::Vulkan, "Vulkan"); }
