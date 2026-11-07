#include "aether/gfx/rhi/device.h"
#include "aether/core/log.h"
#include "test_framework.h"

#include <limits>
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

AETHER_TEST(RHI_D3D12DestroyTextureFreesTheSlotForReuse) {
    auto device = CreateDevice(Backend::D3D12, /*enable_debug_layer=*/false);
    const u8 px[4] = {255, 0, 0, 255};
    const SampledTextureHandle a = device->CreateTexture(1, 1, px);
    const SampledTextureHandle b = device->CreateTexture(1, 1, px);
    AETHER_CHECK(a.IsValid() && b.IsValid() && a.index != b.index);
    AETHER_CHECK(device->DestroyTexture(a));
    AETHER_CHECK(!device->DestroyTexture(a));           // already gone
    AETHER_CHECK(!device->DestroyTexture(SampledTextureHandle{})); // not a texture
    const SampledTextureHandle c = device->CreateTexture(1, 1, px);
    AETHER_CHECK(c.index == a.index);                    // the slot came back
    // Churn well past the table's capacity: only live textures count.
    for (u32 i = 0; i < kMaxUserBindlessTextures * 2; ++i) {
        const SampledTextureHandle t = device->CreateTexture(1, 1, px);
        AETHER_CHECK(t.IsValid());
        AETHER_CHECK(device->DestroyTexture(t));
    }
}

AETHER_TEST(RHI_D3D12HasNoAstc) {
    auto device = CreateDevice(Backend::D3D12, /*enable_debug_layer=*/false);
    AETHER_CHECK(!device->SupportsCompressedTexture(CompressedTextureFormat::ASTC_4x4) && !device->SupportsCompressedTexture(CompressedTextureFormat::ASTC_8x8));
    const std::vector<u8> block(16, 0);
    const CompressedMip mip{block.data(), 4, 4, 16};
    AETHER_CHECK(!device->CreateCompressedTexture(CompressedTextureFormat::ASTC_4x4, 4, 4, std::span<const CompressedMip>(&mip, 1)).IsValid());
    AETHER_CHECK(CompressedLevelBytes(CompressedTextureFormat::ASTC_6x6, 13, 7) == 3 * 2 * 16 && CompressedBlockWidth(CompressedTextureFormat::ASTC_8x8) == 8);
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

AETHER_TEST(RHI_VulkanCreatesAstcTexturesWhereTheGpuHasThem) {
    if (!IsBackendAvailable(Backend::Vulkan)) return;
    auto device = CreateDevice(Backend::Vulkan, /*enable_debug_layer=*/false);
    if (!device) return;
    for (const CompressedTextureFormat format : {CompressedTextureFormat::ASTC_4x4, CompressedTextureFormat::ASTC_6x6, CompressedTextureFormat::ASTC_8x8}) {
        // 30 x 18: 30x18, 15x9, 7x4, 3x2, 1x1; each level a whole number of blocks.
        const u32 sizes[5][2] = {{30, 18}, {15, 9}, {7, 4}, {3, 2}, {1, 1}};
        std::vector<std::vector<u8>> data;
        std::vector<CompressedMip> mips;
        for (const auto& s : sizes) {
            data.emplace_back(static_cast<usize>(CompressedLevelBytes(format, s[0], s[1])), 0);
            mips.push_back({data.back().data(), s[0], s[1], data.back().size()});
        }
        if (!device->SupportsCompressedTexture(format)) {
            AETHER_CHECK(!device->CreateCompressedTexture(format, 30, 18, mips).IsValid()); // unsupported is an answer, not an error
            continue;
        }
        AETHER_CHECK(device->CreateCompressedTexture(format, 30, 18, mips).IsValid());
    }
}

AETHER_TEST(RHI_VulkanDestroyTextureFreesTheSlotForReuse) {
    if (!IsBackendAvailable(Backend::Vulkan)) return;
    auto device = CreateDevice(Backend::Vulkan, /*enable_debug_layer=*/false);
    if (!device) return;
    const u8 px[4] = {255, 0, 0, 255};
    const SampledTextureHandle a = device->CreateTexture(1, 1, px);
    const SampledTextureHandle b = device->CreateTexture(1, 1, px);
    AETHER_CHECK(a.IsValid() && b.IsValid() && a.index != b.index);
    AETHER_CHECK(device->DestroyTexture(a));
    AETHER_CHECK(!device->DestroyTexture(a));
    AETHER_CHECK(!device->DestroyTexture(SampledTextureHandle{}));
    AETHER_CHECK(device->CreateTexture(1, 1, px).index == a.index);
    for (u32 i = 0; i < kMaxUserBindlessTextures * 2; ++i) {
        const SampledTextureHandle t = device->CreateTexture(1, 1, px);
        AETHER_CHECK(t.IsValid());
        AETHER_CHECK(device->DestroyTexture(t));
    }
}

// M8 108: the queue's validation increments were recorded as "implemented locally; runtime check
// unverified". These are the runtime checks, against a real D3D12 device.
//
// What the check found: the queue says these rejections "return an invalid handle". They do not.
// CreateTexture logs and throws std::invalid_argument for an unsupported extent or null pixels, and
// returns an invalid handle only when the descriptor heaps are exhausted. The test below pins the
// behaviour that exists rather than the behaviour the row predicted -- the row is corrected in
// M8_PRODUCTION_TECHNOLOGY.md.
AETHER_TEST(RHI_D3D12RejectsInvalidTextureArguments) {
    auto device = CreateDevice(Backend::D3D12, /*enable_debug_layer=*/false);
    AETHER_CHECK(device != nullptr);
    std::vector<u8> pixels(16 * 16 * 4, 255);

    // Control: a representable extent with real pixels allocates and frees. Without this the rejections
    // below would pass just as happily if every call failed.
    const SampledTextureHandle control = device->CreateTexture(16, 16, pixels.data());
    AETHER_CHECK(control.IsValid());
    AETHER_CHECK(device->DestroyTexture(control));

    const auto rejects = [&](u32 width, u32 height, const u8* data) {
        try {
            device->CreateTexture(width, height, data);
        } catch (const std::invalid_argument&) {
            return true;
        } catch (...) {
            return false;
        }
        return false;
    };
    // Rows 4 and 39: zero extents, a null pixel pointer, and an extent no device can represent are all
    // rejected before anything is allocated.
    AETHER_CHECK(rejects(0, 0, pixels.data()));
    AETHER_CHECK(rejects(0, 16, pixels.data()));
    AETHER_CHECK(rejects(16, 0, pixels.data()));
    AETHER_CHECK(rejects(16, 16, nullptr));
    AETHER_CHECK(rejects(100000, 100000, pixels.data()));

    // A rejected argument must not leave the device unusable.
    const SampledTextureHandle after = device->CreateTexture(8, 8, pixels.data());
    AETHER_CHECK(after.IsValid());
    AETHER_CHECK(device->DestroyTexture(after));
}

// Row 29: compute dispatch dimensions outside 1..65535 are rejected before native recording, and a rejected
// dispatch leaves the list recordable and submittable.
AETHER_TEST(RHI_D3D12RejectsInvalidComputeDispatchDimensions) {
    auto device = CreateDevice(Backend::D3D12, /*enable_debug_layer=*/false);
    AETHER_CHECK(device != nullptr);
    auto cmd = device->CreateComputeCommandList();
    AETHER_CHECK(cmd != nullptr);
    cmd->Reset();

    aether::Logger::Instance().ClearRecent();
    cmd->Dispatch(0, 1, 1);
    AETHER_CHECK(aether::test::LoggedErrorContaining("dispatch group counts"));

    aether::Logger::Instance().ClearRecent();
    cmd->Dispatch(1, 1, 70000);
    AETHER_CHECK(aether::test::LoggedErrorContaining("dispatch group counts"));

    // The rejected calls recorded nothing bad: the list still closes and submits.
    cmd->Close();
    const u64 fence = device->SubmitCompute(*cmd);
    device->WaitForComputeFence(fence);
    AETHER_CHECK(device->IsComputeFenceComplete(fence));
}

// Row 22: recording entry points require a recording command list; recording after Close is reported
// instead of being silently dropped.
AETHER_TEST(RHI_D3D12RejectsRecordingOnAClosedCommandList) {
    auto device = CreateDevice(Backend::D3D12, /*enable_debug_layer=*/false);
    AETHER_CHECK(device != nullptr);
    auto cmd = device->CreateComputeCommandList();
    AETHER_CHECK(cmd != nullptr);
    cmd->Reset();
    cmd->Close();

    aether::Logger::Instance().ClearRecent();
    cmd->Dispatch(1, 1, 1);
    AETHER_CHECK(aether::test::LoggedErrorContaining("requires a recording command list"));

    // Submitting the closed list still works, so the rejected call changed nothing.
    const u64 fence = device->SubmitCompute(*cmd);
    device->WaitForComputeFence(fence);
    AETHER_CHECK(device->IsComputeFenceComplete(fence));
}
// Rows 31, 32 and 33: viewport/scissor bounds, clear colours and resource-state enums. Each of these
// reports and returns, so the check is the report plus the list still submitting afterwards.
//
// Recording limitation, stated rather than implied: the D3D12 backend uses ONE message for "no active
// pass" and "bad bounds", and one for "bad texture" and "bad enum", so these checks prove the call was
// rejected and could not be reached, not which clause rejected it. A distinct message per clause would
// make them sharper; that is a change to the diagnostic, not to the validation.
AETHER_TEST(RHI_D3D12RejectsInvalidViewportAndScissor) {
    auto device = CreateDevice(Backend::D3D12, /*enable_debug_layer=*/false);
    AETHER_CHECK(device != nullptr);
    auto cmd = device->CreateCommandList();
    AETHER_CHECK(cmd != nullptr);
    cmd->Reset();

    const Rect scissor{0, 0, 640, 360};
    const Viewport good{0.0f, 0.0f, 640.0f, 360.0f, 0.0f, 1.0f};
    const auto rejects = [&](const Viewport& viewport, const Rect& rect) {
        aether::Logger::Instance().ClearRecent();
        cmd->SetViewportAndScissor(viewport, rect);
        return aether::test::LoggedErrorContaining("valid viewport/scissor bounds");
    };
    AETHER_CHECK(rejects(Viewport{0.0f, 0.0f, 0.0f, 360.0f, 0.0f, 1.0f}, scissor));  // zero width
    AETHER_CHECK(rejects(Viewport{0.0f, 0.0f, 640.0f, -1.0f, 0.0f, 1.0f}, scissor)); // negative height
    AETHER_CHECK(rejects(Viewport{0.0f, 0.0f, 640.0f, 360.0f, 1.0f, 0.0f}, scissor)); // depth range inverted
    AETHER_CHECK(rejects(Viewport{0.0f, 0.0f, 640.0f, 360.0f, 0.0f, 1.5f}, scissor)); // depth above 1
    AETHER_CHECK(rejects(good, Rect{400, 0, 100, 360}));                               // scissor right below left

    cmd->Close();
    const u64 fence = device->Submit(*cmd);
    device->WaitForFence(fence);
    AETHER_CHECK(device->IsFenceComplete(fence));
}

AETHER_TEST(RHI_D3D12RejectsInvalidClearColorAndTexture) {
    auto device = CreateDevice(Backend::D3D12, /*enable_debug_layer=*/false);
    AETHER_CHECK(device != nullptr);
    auto cmd = device->CreateCommandList();
    AETHER_CHECK(cmd != nullptr);
    cmd->Reset();

    const TextureHandle invalid_texture{};
    aether::Logger::Instance().ClearRecent();
    cmd->ClearRenderTarget(invalid_texture, ClearColor{0.0f, 0.0f, 0.0f, 1.0f});
    AETHER_CHECK(aether::test::LoggedErrorContaining("valid texture/color"));

    aether::Logger::Instance().ClearRecent();
    cmd->ClearRenderTarget(invalid_texture, ClearColor{std::nanf(""), 0.0f, 0.0f, 1.0f});
    AETHER_CHECK(aether::test::LoggedErrorContaining("valid texture/color"));

    cmd->Close();
    const u64 fence = device->Submit(*cmd);
    device->WaitForFence(fence);
    AETHER_CHECK(device->IsFenceComplete(fence));
}

AETHER_TEST(RHI_D3D12RejectsInvalidTextureTransitionStates) {
    auto device = CreateDevice(Backend::D3D12, /*enable_debug_layer=*/false);
    AETHER_CHECK(device != nullptr);
    auto cmd = device->CreateCommandList();
    AETHER_CHECK(cmd != nullptr);
    cmd->Reset();

    const TextureHandle invalid_texture{};
    // An unknown state value must be rejected before it is mapped to a native state.
    const auto unknown = static_cast<ResourceState>(0xFF);
    aether::Logger::Instance().ClearRecent();
    cmd->TransitionTexture(invalid_texture, ResourceState::Present, unknown);
    AETHER_CHECK(aether::test::LoggedErrorContaining("valid texture/resource states"));

    aether::Logger::Instance().ClearRecent();
    cmd->TransitionTexture(invalid_texture, unknown, ResourceState::RenderTarget);
    AETHER_CHECK(aether::test::LoggedErrorContaining("valid texture/resource states"));

    cmd->Close();
    const u64 fence = device->Submit(*cmd);
    device->WaitForFence(fence);
    AETHER_CHECK(device->IsFenceComplete(fence));
}
// Rows 12, 17, 27, 28 and 34: handles are validated before the backend indexes its registries, so an invalid
// handle is reported rather than resolved into whatever record happens to sit at that index.
AETHER_TEST(RHI_D3D12RejectsInvalidComputeBindingHandles) {
    auto device = CreateDevice(Backend::D3D12, /*enable_debug_layer=*/false);
    AETHER_CHECK(device != nullptr);
    auto compute = device->CreateComputeCommandList();
    AETHER_CHECK(compute != nullptr);
    compute->Reset();

    // Row 12: an invalid compute-pipeline handle.
    aether::Logger::Instance().ClearRecent();
    compute->BindComputePipeline(ComputePipelineHandle{});
    AETHER_CHECK(aether::test::LoggedErrorContaining("valid pipeline handle"));

    // Row 28: a barrier on a handle that is not a storage buffer.
    aether::Logger::Instance().ClearRecent();
    compute->ComputeBufferBarrier(BufferHandle{});
    AETHER_CHECK(aether::test::LoggedErrorContaining("storage buffer"));

    compute->Close();
    const u64 fence = device->SubmitCompute(*compute);
    device->WaitForComputeFence(fence);
    AETHER_CHECK(device->IsComputeFenceComplete(fence));

    // Row 27: the same calls on a graphics list are refused for the queue, not silently recorded.
    auto graphics = device->CreateCommandList();
    AETHER_CHECK(graphics != nullptr);
    graphics->Reset();
    aether::Logger::Instance().ClearRecent();
    graphics->BindComputePipeline(ComputePipelineHandle{});
    AETHER_CHECK(aether::test::LoggedErrorContaining("valid pipeline handle"));
    graphics->Close();
    const u64 graphics_fence = device->Submit(*graphics);
    device->WaitForFence(graphics_fence);
    AETHER_CHECK(device->IsFenceComplete(graphics_fence));
}

AETHER_TEST(RHI_D3D12RejectsInvalidIndexBufferFormatAndHandle) {
    auto device = CreateDevice(Backend::D3D12, /*enable_debug_layer=*/false);
    AETHER_CHECK(device != nullptr);
    auto cmd = device->CreateCommandList();
    AETHER_CHECK(cmd != nullptr);
    cmd->Reset();

    // Row 34: an index format outside the two declared values.
    aether::Logger::Instance().ClearRecent();
    cmd->BindIndexBuffer(BufferHandle{}, static_cast<IndexFormat>(0xFF));
    AETHER_CHECK(aether::test::LoggedErrorContaining("invalid format, state, or buffer handle"));

    // Row 17: a valid format with an invalid buffer handle.
    aether::Logger::Instance().ClearRecent();
    cmd->BindIndexBuffer(BufferHandle{}, IndexFormat::UInt32);
    AETHER_CHECK(aether::test::LoggedErrorContaining("invalid format, state, or buffer handle"));

    // The same for vertex buffers, which shares the binding-readiness checks (row 25's input path).
    aether::Logger::Instance().ClearRecent();
    cmd->BindVertexBuffer(BufferHandle{}, 32);
    AETHER_CHECK(aether::test::LoggedErrorContaining("invalid handle or a stride"));

    cmd->Close();
    const u64 fence = device->Submit(*cmd);
    device->WaitForFence(fence);
    AETHER_CHECK(device->IsFenceComplete(fence));
}
// Row 41 and the submission-queue checks: sizes a backend cannot represent are refused before any allocation, and a
// command list submitted to the wrong queue is refused rather than executed.
AETHER_TEST(RHI_D3D12RejectsInvalidBufferSizes) {
    auto device = CreateDevice(Backend::D3D12, /*enable_debug_layer=*/false);
    AETHER_CHECK(device != nullptr);
    const u8 bytes[16] = {};
    const auto rejects = [&](u64 size, u32 stride) {
        try {
            device->CreateStorageBuffer(size, stride, bytes);
        } catch (const std::invalid_argument&) {
            return true;
        } catch (...) {
            return false;
        }
        return false;
    };
    AETHER_CHECK(rejects(0, 4));                                        // zero
    AETHER_CHECK(rejects(std::numeric_limits<u64>::max(), 4));          // beyond the buffer view limit
    AETHER_CHECK(rejects(6, 4));                                        // not a multiple of its stride
    AETHER_CHECK(rejects(64, 3));                                       // stride that does not divide the size
    // Control: a representable size allocates and frees, so the rejections are not "every call fails".
    const BufferHandle ok = device->CreateStorageBuffer(16, 4, bytes);
    AETHER_CHECK(ok.IsValid());
    AETHER_CHECK(device->DestroyBuffer(ok));
}

AETHER_TEST(RHI_D3D12RejectsQueueMismatchedSubmissions) {
    auto device = CreateDevice(Backend::D3D12, /*enable_debug_layer=*/false);
    AETHER_CHECK(device != nullptr);

    auto compute = device->CreateComputeCommandList();
    AETHER_CHECK(compute != nullptr);
    compute->Reset();
    compute->Close();
    aether::Logger::Instance().ClearRecent();
    AETHER_CHECK(device->Submit(*compute) == 0);  // a compute list on the graphics queue
    AETHER_CHECK(aether::test::LoggedErrorContaining("closed graphics command list"));

    auto graphics = device->CreateCommandList();
    AETHER_CHECK(graphics != nullptr);
    graphics->Reset();
    graphics->Close();
    aether::Logger::Instance().ClearRecent();
    AETHER_CHECK(device->SubmitCompute(*graphics) == 0);  // a graphics list on the compute queue
    AETHER_CHECK(aether::test::LoggedErrorContaining("closed compute command list"));
}
// Rows 15 and 16: sampled-texture descriptor capacity is enforced before allocation, and a refused creation does not
// consume a slot. The control at the end -- free one, create one -- is what proves the slots are accounted for rather
// than simply leaking on every failure.
AETHER_TEST(RHI_D3D12RejectsTextureCreationWhenDescriptorsAreExhausted) {
    auto device = CreateDevice(Backend::D3D12, /*enable_debug_layer=*/false);
    AETHER_CHECK(device != nullptr);
    const u8 pixel[4] = {255, 255, 255, 255};
    std::vector<SampledTextureHandle> held;
    aether::Logger::Instance().ClearRecent();
    for (u32 i = 0; i < kMaxUserBindlessTextures + 4; ++i) {
        const SampledTextureHandle texture = device->CreateTexture(1, 1, pixel);
        if (texture.IsValid()) held.push_back(texture);
    }
    // The capacity held: no more than the declared maximum were handed out, and the refusal was reported.
    AETHER_CHECK(held.size() <= kMaxUserBindlessTextures);
    AETHER_CHECK(aether::test::LoggedErrorContaining("exhausted"));

    // A refused creation must not have consumed a slot: freeing one allows exactly one more.
    if (!held.empty()) {
        AETHER_CHECK(device->DestroyTexture(held.back()));
        held.pop_back();
        const SampledTextureHandle again = device->CreateTexture(1, 1, pixel);
        AETHER_CHECK(again.IsValid());
        if (again.IsValid()) held.push_back(again);
    }
    for (const SampledTextureHandle texture : held) AETHER_CHECK(device->DestroyTexture(texture));
}

// The fault hook the remaining verification-debt rows need. AETHER_RHI_FAIL_ALLOCATION_AT makes the Nth buffer or
// texture registry reservation fail, which is the only practical way to reach partial-construction and error paths.
// It is opt-in through the environment, so a normal run never sees it, and this test proves both that it fires and
// that disarming it leaves the device usable.
AETHER_TEST(RHI_D3D12FaultInjectionFailsTheArmedAllocation) {
    auto device = CreateDevice(Backend::D3D12, /*enable_debug_layer=*/false);
    AETHER_CHECK(device != nullptr);
    if (!device) return;
    const u8 bytes[64] = {};
#if defined(_WIN32)
    _putenv_s("AETHER_RHI_FAIL_ALLOCATION_AT", "1");
#else
    setenv("AETHER_RHI_FAIL_ALLOCATION_AT", "1", 1);
#endif
    bool threw = false;
    try {
        (void)device->CreateVertexBuffer(bytes, 40);
    } catch (const std::exception&) {
        threw = true;
    }
#if defined(_WIN32)
    _putenv_s("AETHER_RHI_FAIL_ALLOCATION_AT", "");
#else
    unsetenv("AETHER_RHI_FAIL_ALLOCATION_AT");
#endif
    AETHER_CHECK(threw);
    const BufferHandle after = device->CreateVertexBuffer(bytes, 40);
    AETHER_CHECK(after.IsValid());
    if (after.IsValid()) AETHER_CHECK(device->DestroyBuffer(after));
}
