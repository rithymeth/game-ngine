// Proves the RHI abstraction (engine/include/aether/gfx/rhi) genuinely
// swaps backends: the exact same frame loop, written entirely against
// IDevice/ISwapChain/ICommandList, runs on both D3D12 and Vulkan depending
// on the AETHER_RHI_BACKEND environment variable ("d3d12" or "vulkan",
// default d3d12).
//
// Scope is deliberately narrow: clear the backbuffer to a color and present,
// every frame. No shaders, no pipelines, no draw calls — those differ
// enough between HLSL/root-signatures and SPIR-V/descriptor-sets that
// unifying them is a distinctly larger piece of work than this device/
// swapchain/command-submission layer (see the RHI header comments). This
// demo is the "does the device layer actually work on both backends" proof;
// the full sandbox/editor (bindless textures, compute culling, ImGui) stay
// D3D12-only.
//
// Set AETHER_RHI_DEMO_MAX_FRAMES=<N> to auto-close after N frames instead of
// waiting for the window to be closed, for scripted/automated verification.

#include "aether/core/log.h"
#include "aether/gfx/rhi/device.h"
#include "aether/platform/window.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace aether;
using namespace aether::gfx::rhi;

int main() {
    Backend backend = Backend::D3D12;
    if (const char* env = std::getenv("AETHER_RHI_BACKEND")) {
        if (std::strcmp(env, "vulkan") == 0) {
            backend = Backend::Vulkan;
        }
    }

    i32 max_frames = -1;
    if (const char* env = std::getenv("AETHER_RHI_DEMO_MAX_FRAMES")) {
        max_frames = std::atoi(env);
    }

    try {
        if (!IsBackendAvailable(backend)) {
            AETHER_LOG_FATAL("RHIDemo", "Requested backend is not available in this build");
            return 1;
        }

        WindowDesc window_desc;
        window_desc.title = (backend == Backend::Vulkan) ? "Aether RHI Demo - Vulkan" : "Aether RHI Demo - D3D12";
        window_desc.width = 800;
        window_desc.height = 600;
        Window window(window_desc);

        std::unique_ptr<IDevice> device = CreateDevice(backend, /*enable_debug_layer=*/true);
        if (!device) {
            AETHER_LOG_FATAL("RHIDemo", "Failed to create device");
            return 1;
        }

        std::unique_ptr<ISwapChain> swap_chain =
            device->CreateSwapChain(window.NativeHandle(), window.Width(), window.Height());

        window.on_resize = [&](u32 w, u32 h) { swap_chain->Resize(w, h); };

        std::vector<std::unique_ptr<ICommandList>> command_lists;
        std::vector<u64> frame_fences(swap_chain->BufferCount(), 0);
        // Vulkan swap chain images start life in an undefined layout;
        // D3D12's convention (matching the rest of this engine) is to treat
        // a fresh backbuffer as already PRESENT. Tracking "have we used this
        // slot yet" lets the same code transition correctly on both
        // backends without the RHI needing a full RenderGraph-style
        // automatic-state-tracking layer of its own.
        std::vector<bool> used_before(swap_chain->BufferCount(), false);
        for (u32 i = 0; i < swap_chain->BufferCount(); ++i) {
            command_lists.push_back(device->CreateCommandList());
        }

        AETHER_LOG_INFO("RHIDemo", "Entering main loop (backend=%s, %u buffers)",
                         backend == Backend::Vulkan ? "Vulkan" : "D3D12", swap_chain->BufferCount());

        bool test_resize = std::getenv("AETHER_RHI_DEMO_TEST_RESIZE") != nullptr;

        i32 frame_index = 0;
        f32 t = 0.0f;
        while (window.PumpMessages()) {
            if (window.IsMinimized()) {
                continue;
            }

            // Programmatic resize exercise, since triggering a real window
            // resize needs interactive input this demo can't script: proves
            // ISwapChain::Resize() (swapchain/image/semaphore recreation on
            // Vulkan, RTV recreation on D3D12) doesn't crash on either
            // backend without needing a human to drag the window edge.
            if (test_resize && frame_index == 10) {
                AETHER_LOG_INFO("RHIDemo", "Testing programmatic resize to 400x300");
                for (u64 fence : frame_fences) {
                    device->WaitForFence(fence);
                }
                swap_chain->Resize(400, 300);
                used_before.assign(swap_chain->BufferCount(), false);
            }

            swap_chain->AcquireNextImage();
            TextureHandle back_buffer = swap_chain->CurrentBackBuffer();

            // There's no direct "which slot is this" query on ISwapChain
            // (backends differ on whether that's the acquired image index or
            // a separate frame-in-flight counter — see VulkanSwapChain's
            // comments) so we key bookkeeping off the TextureHandle's own
            // index, which is stable and unique per backbuffer on both
            // backends.
            u32 slot = back_buffer.index % static_cast<u32>(frame_fences.size());
            device->WaitForFence(frame_fences[slot]);

            ICommandList& cmd = *command_lists[slot];
            cmd.Reset();

            ResourceState before = used_before[slot] ? ResourceState::Present : ResourceState::Undefined;
            cmd.TransitionTexture(back_buffer, before, ResourceState::RenderTarget);

            t += 0.01f;
            f32 pulse = 0.5f + 0.5f * std::sin(t);
            cmd.ClearRenderTarget(back_buffer, {0.05f, 0.05f * pulse, 0.15f + 0.1f * pulse, 1.0f});

            cmd.TransitionTexture(back_buffer, ResourceState::RenderTarget, ResourceState::Present);
            used_before[slot] = true;

            cmd.Close();
            frame_fences[slot] = device->Submit(cmd, swap_chain.get());
            swap_chain->Present(/*vsync=*/true);

            ++frame_index;
            if (max_frames >= 0 && frame_index >= max_frames) {
                AETHER_LOG_INFO("RHIDemo", "Reached AETHER_RHI_DEMO_MAX_FRAMES=%d, exiting", max_frames);
                break;
            }
        }

        for (u64 fence : frame_fences) {
            device->WaitForFence(fence);
        }

        AETHER_LOG_INFO("RHIDemo", "Shutting down cleanly");
    } catch (const std::exception& e) {
        AETHER_LOG_FATAL("RHIDemo", "Unhandled exception: %s", e.what());
        return 1;
    }

    return 0;
}
