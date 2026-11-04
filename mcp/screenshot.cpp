#include "screenshot.h"

#include "aether/gfx/rhi/device.h"
#include "aether/platform/window.h"
#include "aether/player/scene_renderer.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include <stb_image_write.h>

namespace aether::mcp {

using namespace gfx;

struct GameScreenshotter::Impl {
    std::unique_ptr<Window> window;
    std::unique_ptr<rhi::IDevice> device;
    std::unique_ptr<rhi::ISwapChain> swap_chain;
    std::unique_ptr<rhi::ICommandList> commands;
    std::unique_ptr<player::SceneRenderer> renderer;
    u64 fence = 0;
    u32 width = 0, height = 0;
    std::string backend;

    void Release() {
        if (device && fence != 0) device->WaitForFence(fence);
        renderer.reset();
        commands.reset();
        swap_chain.reset();
        device.reset();
        window.reset();
        fence = 0;
        width = height = 0;
    }
};

GameScreenshotter::GameScreenshotter() : impl_(std::make_unique<Impl>()) {}
GameScreenshotter::~GameScreenshotter() { impl_->Release(); }
void GameScreenshotter::Reset() { impl_->Release(); }

GameScreenshotter::Result GameScreenshotter::Capture(player::Game& game, const player::GamePackage& package, u32 width, u32 height,
                                                     const std::string& backend_name) {
    Result result;
    Impl& im = *impl_;
#if defined(_WIN32)
    const std::string backend_key = backend_name.empty() ? "d3d12" : backend_name;
#else
    const std::string backend_key = backend_name.empty() ? "vulkan" : backend_name;
#endif
    if (backend_key != "d3d12" && backend_key != "vulkan") {
        result.error = "backend must be \"d3d12\" or \"vulkan\"";
        return result;
    }

    if (!im.renderer || im.width != width || im.height != height || im.backend != backend_key) {
        im.Release();
        WindowDesc desc;
        desc.title = "Aether MCP capture";
        desc.width = width;
        desc.height = height;
        desc.visible = false;
        im.window = std::make_unique<Window>(desc);
        if (!im.window->IsValid()) {
            im.Release();
            result.error = "Could not create an off-screen window";
            return result;
        }
#if defined(_WIN32)
        void* native = im.window->NativeHandle();
#else
        void* native = im.window->PlatformWindow();
#endif
        const rhi::Backend backend = backend_key == "vulkan" ? rhi::Backend::Vulkan : rhi::Backend::D3D12;
        im.device = rhi::CreateDevice(backend, false);
        if (!im.device) {
            im.Release();
            result.error = "No " + backend_key + " graphics device is available on this machine";
            return result;
        }
        im.swap_chain = im.device->CreateSwapChain(native, im.window->Width(), im.window->Height(), 2);
        if (!im.swap_chain) {
            im.Release();
            result.error = "Could not create a swap chain";
            return result;
        }
        im.commands = im.device->CreateCommandList();
        im.renderer = std::make_unique<player::SceneRenderer>(*im.device, *im.swap_chain, package);
        im.width = width;
        im.height = height;
        im.backend = backend_key;
    }

    im.swap_chain->AcquireNextImage();
    im.commands->Reset();
    im.renderer->Draw(game, *im.commands, 0);
    im.commands->Close();
    im.fence = im.device->Submit(*im.commands, im.swap_chain.get());

    std::vector<u8> rgba;
    if (!im.swap_chain->ReadBack(rgba)) {
        im.swap_chain->Present(false);
        result.error = "This graphics backend cannot read the frame back";
        return result;
    }
    im.swap_chain->Present(false);

    const u32 w = im.swap_chain->Width(), h = im.swap_chain->Height();
    if (rgba.size() != static_cast<usize>(w) * h * 4) {
        result.error = "The frame read back has an unexpected size";
        return result;
    }
    // The back buffer's alpha is not meaningful for a picture: make it opaque.
    for (usize i = 3; i < rgba.size(); i += 4) rgba[i] = 255;
    stbi_write_png_to_func(
        [](void* context, void* data, int size) {
            auto* out = static_cast<std::vector<u8>*>(context);
            out->insert(out->end(), static_cast<u8*>(data), static_cast<u8*>(data) + size);
        },
        &result.png, static_cast<int>(w), static_cast<int>(h), 4, rgba.data(), static_cast<int>(w) * 4);
    if (result.png.empty()) {
        result.error = "Could not encode the PNG";
        return result;
    }
    result.ok = true;
    result.width = w;
    result.height = h;
    return result;
}

} // namespace aether::mcp
