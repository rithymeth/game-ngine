#include "aether/core/log.h"
#if defined(_WIN32)
#include "aether/gfx/rhi/d3d12/d3d12_backend.h"
#endif
#include "aether/gfx/rhi/device.h"

#if defined(AETHER_HAS_VULKAN)
#include "aether/gfx/rhi/vulkan/vulkan_backend.h"
#endif

namespace aether::gfx::rhi {

bool IsBackendAvailable(Backend backend) {
    switch (backend) {
        case Backend::D3D12:
#if defined(_WIN32)
            return true;
#else
            return false;
#endif
        case Backend::Vulkan:
#if defined(AETHER_HAS_VULKAN)
            return true;
#else
            return false;
#endif
    }
    return false;
}

std::unique_ptr<IDevice> CreateDevice(Backend backend, bool enable_debug_layer) {
    switch (backend) {
        case Backend::D3D12:
#if defined(_WIN32)
            return std::make_unique<d3d12_backend::D3D12Device>(enable_debug_layer);
#else
            AETHER_LOG_ERROR("RHI", "D3D12 is Windows-only");
            return nullptr;
#endif
        case Backend::Vulkan:
#if defined(AETHER_HAS_VULKAN)
            return std::make_unique<vulkan_backend::VulkanDevice>(enable_debug_layer);
#else
            AETHER_LOG_ERROR("RHI", "Vulkan backend requested but not built (no Vulkan SDK/loader found at "
                                     "configure time)");
            return nullptr;
#endif
    }
    return nullptr;
}

} // namespace aether::gfx::rhi
