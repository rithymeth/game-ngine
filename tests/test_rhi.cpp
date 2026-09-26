#include "aether/gfx/rhi/device.h"
#include "test_framework.h"

using namespace aether;
using namespace aether::gfx::rhi;

AETHER_TEST(RHI_D3D12BackendIsAlwaysAvailable) {
    AETHER_CHECK(IsBackendAvailable(Backend::D3D12));
}

AETHER_TEST(RHI_CreateD3D12DeviceSucceeds) {
    auto device = CreateDevice(Backend::D3D12, /*enable_debug_layer=*/false);
    AETHER_CHECK(device != nullptr);
    AETHER_CHECK(device->GetBackend() == Backend::D3D12);
    AETHER_CHECK(device->NativeHandle() != nullptr);
}

AETHER_TEST(RHI_D3D12CommandListLifecycle) {
    auto device = CreateDevice(Backend::D3D12, /*enable_debug_layer=*/false);
    auto cmd = device->CreateCommandList();
    AETHER_CHECK(cmd != nullptr);
    cmd->Reset();
    cmd->Close();

    u64 fence_value = device->Submit(*cmd);
    device->WaitForFence(fence_value);
    AETHER_CHECK(device->IsFenceComplete(fence_value));
}

#if defined(AETHER_HAS_VULKAN)

AETHER_TEST(RHI_VulkanBackendIsAvailableWhenBuilt) {
    AETHER_CHECK(IsBackendAvailable(Backend::Vulkan));
}

AETHER_TEST(RHI_CreateVulkanDeviceSucceeds) {
    auto device = CreateDevice(Backend::Vulkan, /*enable_debug_layer=*/false);
    AETHER_CHECK(device != nullptr);
    AETHER_CHECK(device->GetBackend() == Backend::Vulkan);
    AETHER_CHECK(device->NativeHandle() != nullptr);
}

AETHER_TEST(RHI_VulkanCommandListLifecycle) {
    auto device = CreateDevice(Backend::Vulkan, /*enable_debug_layer=*/false);
    auto cmd = device->CreateCommandList();
    AETHER_CHECK(cmd != nullptr);
    cmd->Reset();
    cmd->Close();

    u64 fence_value = device->Submit(*cmd);
    device->WaitForFence(fence_value);
    AETHER_CHECK(device->IsFenceComplete(fence_value));
}

#else

AETHER_TEST(RHI_VulkanBackendUnavailableReturnsNull) {
    AETHER_CHECK(!IsBackendAvailable(Backend::Vulkan));
    auto device = CreateDevice(Backend::Vulkan, /*enable_debug_layer=*/false);
    AETHER_CHECK(device == nullptr);
}

#endif
