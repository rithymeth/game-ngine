#include "aether/gfx/rhi/device.h"
#include "test_framework.h"

#include <span>
#include <stdexcept>
#include <vector>

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

AETHER_TEST(RHI_D3D12ComputeCommandListLifecycle) {
    auto device = CreateDevice(Backend::D3D12, /*enable_debug_layer=*/false);
    auto cmd = device->CreateComputeCommandList();
    AETHER_CHECK(cmd != nullptr);
    cmd->Reset();
    cmd->Close();

    u64 fence_value = device->SubmitCompute(*cmd);
    device->WaitForComputeFence(fence_value);
    AETHER_CHECK(device->IsComputeFenceComplete(fence_value));
}

AETHER_TEST(RHI_D3D12GraphicsQueueWaitOnComputeDoesNotDeadlock) {
    auto device = CreateDevice(Backend::D3D12, /*enable_debug_layer=*/false);

    auto compute_cmd = device->CreateComputeCommandList();
    compute_cmd->Reset();
    compute_cmd->Close();
    u64 compute_fence = device->SubmitCompute(*compute_cmd);

    device->GraphicsQueueWaitOnCompute(compute_fence);
    auto graphics_cmd = device->CreateCommandList();
    graphics_cmd->Reset();
    graphics_cmd->Close();
    u64 graphics_fence = device->Submit(*graphics_cmd);

    device->WaitForFence(graphics_fence);
    AETHER_CHECK(device->IsFenceComplete(graphics_fence));
    AETHER_CHECK(device->IsComputeFenceComplete(compute_fence));
}

AETHER_TEST(RHI_D3D12ComputeQueueWaitOnGraphicsDoesNotDeadlock) {
    auto device = CreateDevice(Backend::D3D12, /*enable_debug_layer=*/false);

    auto graphics_cmd = device->CreateCommandList();
    graphics_cmd->Reset();
    graphics_cmd->Close();
    u64 graphics_fence = device->Submit(*graphics_cmd);

    device->ComputeQueueWaitOnGraphics(graphics_fence);
    auto compute_cmd = device->CreateComputeCommandList();
    compute_cmd->Reset();
    compute_cmd->Close();
    u64 compute_fence = device->SubmitCompute(*compute_cmd);

    device->WaitForComputeFence(compute_fence);
    AETHER_CHECK(device->IsComputeFenceComplete(compute_fence));
    AETHER_CHECK(device->IsFenceComplete(graphics_fence));
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

AETHER_TEST(RHI_VulkanComputeCommandListLifecycle) {
    auto device = CreateDevice(Backend::Vulkan, /*enable_debug_layer=*/false);
    auto cmd = device->CreateComputeCommandList();
    AETHER_CHECK(cmd != nullptr);
    cmd->Reset();
    cmd->Close();

    u64 fence_value = device->SubmitCompute(*cmd);
    device->WaitForComputeFence(fence_value);
    AETHER_CHECK(device->IsComputeFenceComplete(fence_value));
}

AETHER_TEST(RHI_VulkanGraphicsQueueWaitOnComputeDoesNotDeadlock) {
    auto device = CreateDevice(Backend::Vulkan, /*enable_debug_layer=*/false);

    auto compute_cmd = device->CreateComputeCommandList();
    compute_cmd->Reset();
    compute_cmd->Close();
    u64 compute_fence = device->SubmitCompute(*compute_cmd);

    device->GraphicsQueueWaitOnCompute(compute_fence);
    auto graphics_cmd = device->CreateCommandList();
    graphics_cmd->Reset();
    graphics_cmd->Close();
    u64 graphics_fence = device->Submit(*graphics_cmd);

    device->WaitForFence(graphics_fence);
    AETHER_CHECK(device->IsFenceComplete(graphics_fence));
    AETHER_CHECK(device->IsComputeFenceComplete(compute_fence));
}

AETHER_TEST(RHI_VulkanComputeQueueWaitOnGraphicsDoesNotDeadlock) {
    auto device = CreateDevice(Backend::Vulkan, /*enable_debug_layer=*/false);

    auto graphics_cmd = device->CreateCommandList();
    graphics_cmd->Reset();
    graphics_cmd->Close();
    u64 graphics_fence = device->Submit(*graphics_cmd);

    device->ComputeQueueWaitOnGraphics(graphics_fence);
    auto compute_cmd = device->CreateComputeCommandList();
    compute_cmd->Reset();
    compute_cmd->Close();
    u64 compute_fence = device->SubmitCompute(*compute_cmd);

    device->WaitForComputeFence(compute_fence);
    AETHER_CHECK(device->IsComputeFenceComplete(compute_fence));
    AETHER_CHECK(device->IsFenceComplete(graphics_fence));
}

#else

AETHER_TEST(RHI_VulkanBackendUnavailableReturnsNull) {
    AETHER_CHECK(!IsBackendAvailable(Backend::Vulkan));
    auto device = CreateDevice(Backend::Vulkan, /*enable_debug_layer=*/false);
    AETHER_CHECK(device == nullptr);
}

#endif

AETHER_TEST(RHI_D3D12FrameConstantBuffersAreCreatedAndUpdated) {
    auto device = CreateDevice(Backend::D3D12, /*enable_debug_layer=*/false);
    AETHER_CHECK(device->SupportsFrameConstants());
    const BufferHandle buffer = device->CreateConstantBuffer(80); // padded to 256 inside
    AETHER_CHECK(buffer.IsValid());
    const f32 data[20] = {1.0f, 2.0f, 3.0f, 4.0f};
    device->UpdateConstantBuffer(buffer, data, sizeof(data)); // in range: accepted
    device->UpdateConstantBuffer(buffer, data, 4096);          // out of range: logged and ignored, not a crash
    bool threw = false;
    try {
        device->CreateConstantBuffer(0);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    AETHER_CHECK(threw);
}

AETHER_TEST(RHI_VulkanFrameConstantBuffersAreCreatedAndUpdated) {
    if (!IsBackendAvailable(Backend::Vulkan)) return;
    auto device = CreateDevice(Backend::Vulkan, /*enable_debug_layer=*/false);
    if (!device) return;
    AETHER_CHECK(device->SupportsFrameConstants());
    const BufferHandle buffer = device->CreateConstantBuffer(80); // padded to 256 inside
    AETHER_CHECK(buffer.IsValid());
    const f32 data[20] = {1.0f, 2.0f, 3.0f, 4.0f};
    device->UpdateConstantBuffer(buffer, data, sizeof(data));
    device->UpdateConstantBuffer(buffer, data, 4096); // out of range: logged and ignored
    bool threw = false;
    try {
        device->CreateConstantBuffer(0);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    AETHER_CHECK(threw);
}

AETHER_TEST(RHI_VulkanCreatesBlockCompressedTexturesWhereTheGpuHasThem) {
    if (!IsBackendAvailable(Backend::Vulkan)) return;
    auto device = CreateDevice(Backend::Vulkan, /*enable_debug_layer=*/false);
    if (!device) return;
    const u32 sizes[5][2] = {{20, 12}, {10, 6}, {5, 3}, {2, 1}, {1, 1}};
    std::vector<std::vector<u8>> data;
    std::vector<CompressedMip> mips;
    for (const auto& s : sizes) {
        data.emplace_back(static_cast<usize>((s[0] + 3) / 4) * ((s[1] + 3) / 4) * 16, 0x55);
        mips.push_back({data.back().data(), s[0], s[1], data.back().size()});
    }
    if (!device->SupportsCompressedTexture(CompressedTextureFormat::BC7)) {
        // A GPU without BC (a phone's): no support is the answer, not an error.
        AETHER_CHECK(!device->CreateCompressedTexture(CompressedTextureFormat::BC7, 20, 12, mips).IsValid());
        return;
    }
    const SampledTextureHandle texture = device->CreateCompressedTexture(CompressedTextureFormat::BC7, 20, 12, mips);
    AETHER_CHECK(texture.IsValid() && texture.index >= kFirstUserBindlessTextureIndex);
    std::vector<CompressedMip> wrong = mips;
    wrong[1].size_bytes -= 1;
    bool threw = false;
    try {
        device->CreateCompressedTexture(CompressedTextureFormat::BC7, 20, 12, wrong);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    AETHER_CHECK(threw);
}

AETHER_TEST(RHI_D3D12CreatesBlockCompressedTexturesWithMips) {
    auto device = CreateDevice(Backend::D3D12, /*enable_debug_layer=*/false);
    AETHER_CHECK(device->SupportsCompressedTexture(CompressedTextureFormat::BC1) && device->SupportsCompressedTexture(CompressedTextureFormat::BC7));
    // 20 x 12: the mip chain 20x12, 10x6, 5x3, 2x1, 1x1 (levels below one block still take a whole block).
    const u32 sizes[5][2] = {{20, 12}, {10, 6}, {5, 3}, {2, 1}, {1, 1}};
    std::vector<std::vector<u8>> data;
    std::vector<CompressedMip> mips;
    for (const auto& s : sizes) {
        data.emplace_back(static_cast<usize>((s[0] + 3) / 4) * ((s[1] + 3) / 4) * 16, 0x55);
        mips.push_back({data.back().data(), s[0], s[1], data.back().size()});
    }
    const SampledTextureHandle texture = device->CreateCompressedTexture(CompressedTextureFormat::BC7, 20, 12, mips);
    AETHER_CHECK(texture.IsValid() && texture.index >= kFirstUserBindlessTextureIndex);
    // BC1 blocks are 8 bytes.
    std::vector<std::vector<u8>> bc1_data;
    std::vector<CompressedMip> bc1;
    for (const auto& s : sizes) {
        bc1_data.emplace_back(static_cast<usize>((s[0] + 3) / 4) * ((s[1] + 3) / 4) * 8, 0x22);
        bc1.push_back({bc1_data.back().data(), s[0], s[1], bc1_data.back().size()});
    }
    AETHER_CHECK(device->CreateCompressedTexture(CompressedTextureFormat::BC1, 20, 12, bc1).IsValid());
    // A level of the wrong size, a missing level list and a zero size are refused.
    std::vector<CompressedMip> wrong = mips;
    wrong[1].size_bytes -= 1;
    bool threw = false;
    try {
        device->CreateCompressedTexture(CompressedTextureFormat::BC7, 20, 12, wrong);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    AETHER_CHECK(threw);
    threw = false;
    try {
        device->CreateCompressedTexture(CompressedTextureFormat::BC7, 20, 12, std::span<const CompressedMip>());
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    AETHER_CHECK(threw);
}
