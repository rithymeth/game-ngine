// aether_player: a cooked game, without the editor (Phase 25 step 4,
// docs/design/PHASE_SPECS.md §25.4). It mounts the game's .apak archives,
// loads the startup scene and runs it on the fixed-timestep frame loop, in
// a window drawn through the RHI (D3D12 on Windows, Vulkan elsewhere), or
// headless.
//
//   aether_player [--pak <file|dir>]... [--scene <path>] [--frames <n>]
//                 [--headless] [--backend d3d12|vulkan] [--size <w>x<h>]
//                 [--quality <preset>] [--key <64 hex digits>]
//                 [--press <Key>]... [--report]
//
// Encrypted archives (§25.6) open with --key, or AETHER_PAK_KEY in the
// environment, or the key a game's player was built with
// (-DAETHER_GAME_PAK_KEY=<hex>).
//
// The window's title, size and vsync, and the quality preset, come from the
// project's settings (§25.5) unless given here.
//
// With no --pak it mounts every .apak in Paks/ beside the executable (then
// in the working directory). The manifest's configuration sets the log
// level: Debug logs everything, Development logs info and a stats line
// every second, Shipping only warnings and errors.

#include "aether/player/game.h"

#include "aether/core/log.h"
#include "aether/gfx/rhi/device.h"
#include "aether/platform/window.h"
#include "aether/scene/components.h"
#include "aether/scene/gameplay.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using namespace aether;
using namespace aether::player;
namespace rhi = aether::gfx::rhi;
namespace stdfs = std::filesystem;

namespace {

struct Options {
    std::vector<std::string> paks;
    std::string scene;
    i64 frames = -1;
    bool headless = false;
    std::string backend;
    u32 width = 0, height = 0; // 0: the project's
    std::string quality;
    std::vector<std::string> keys;
    std::vector<std::string> pressed; // keys held for the whole run (headless runs, smoke tests)
    bool report = false;              // print where the Player-tagged entity ended up
};

void Usage() {
    std::fprintf(stderr,
                 "usage: aether_player [--pak <file|dir>]... [--scene <path>] [--frames <n>] [--headless]\n"
                 "                     [--backend d3d12|vulkan] [--size <w>x<h>] [--quality <preset>]\n"
                 "                     [--key <64 hex digits>] [--press <Key>]... [--report]\n");
}

bool ParseArgs(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const bool has_value = i + 1 < argc;
        if (a == "--pak" && has_value) {
            o.paks.push_back(argv[++i]);
        } else if (a == "--scene" && has_value) {
            o.scene = argv[++i];
        } else if (a == "--frames" && has_value) {
            o.frames = std::atoll(argv[++i]);
        } else if (a == "--headless") {
            o.headless = true;
        } else if (a == "--press" && has_value) {
            o.pressed.push_back(argv[++i]);
        } else if (a == "--report") {
            o.report = true;
        } else if (a == "--key" && has_value) {
            o.keys.push_back(argv[++i]);
        } else if (a == "--quality" && has_value) {
            o.quality = argv[++i];
        } else if (a == "--backend" && has_value) {
            o.backend = argv[++i];
        } else if (a == "--size" && has_value) {
            unsigned w = 0, h = 0;
            if (std::sscanf(argv[++i], "%ux%u", &w, &h) != 2 || w < 64 || h < 64) return false;
            o.width = w, o.height = h;
        } else {
            return false;
        }
    }
    return true;
}

// --pak arguments (a folder means every .apak in it), else Paks/ beside the
// executable, else Paks/ in the working directory.
std::vector<std::string> PaksToMount(const Options& o, const char* argv0) {
    std::vector<std::string> paks;
    for (const std::string& p : o.paks) {
        std::error_code ec;
        if (stdfs::is_directory(p, ec)) {
            for (std::string& f : GamePackage::FindPaks(p)) paks.push_back(std::move(f));
        } else {
            paks.push_back(p);
        }
    }
    if (!o.paks.empty()) return paks;
    std::error_code ec;
    const stdfs::path exe_dir = stdfs::absolute(argv0, ec).parent_path();
    paks = GamePackage::FindPaks(exe_dir / "Paks");
    if (paks.empty()) paks = GamePackage::FindPaks("Paks");
    return paks;
}

} // namespace

int main(int argc, char** argv) {
    Options options;
    if (!ParseArgs(argc, argv, options)) {
        Usage();
        return 2;
    }
    if (std::getenv("AETHER_PLAYER_HEADLESS")) options.headless = true;

    GamePackage package;
    std::vector<std::string> keys = options.keys;
    if (const char* env = std::getenv("AETHER_PAK_KEY")) keys.push_back(env);
#ifdef AETHER_GAME_PAK_KEY
    keys.push_back(AETHER_GAME_PAK_KEY);
#endif
    for (const std::string& hex : keys) {
        pak::PakKey key;
        if (!pak::PakKey::FromHex(hex, key)) {
            std::fprintf(stderr, "aether_player: a key must be 64 hex digits\n");
            return 2;
        }
        package.AddKey(key);
    }
    const std::vector<std::string> paks = PaksToMount(options, argv[0]);
    if (paks.empty()) {
        std::fprintf(stderr, "aether_player: no .apak to mount (pass --pak, or put them in Paks/)\n");
        return 1;
    }
    std::string error;
    for (usize i = 0; i < paks.size(); ++i) {
        if (!package.Mount(paks[i], static_cast<int>(i), &error)) {
            std::fprintf(stderr, "aether_player: %s\n", error.c_str());
            return 1;
        }
    }
    if (!package.LoadManifest(&error)) {
        std::fprintf(stderr, "aether_player: %s\n", error.c_str());
        return 1;
    }
    const GameManifest& manifest = package.Manifest();
    const cook::BuildConfiguration config = manifest.configuration;
    Logger::Instance().SetMinLevel(config == cook::BuildConfiguration::Debug         ? LogLevel::Trace
                                   : config == cook::BuildConfiguration::Development ? LogLevel::Info
                                                                                    : LogLevel::Warn);
    AETHER_LOG_INFO("Player", "%s (%s), %zu archive(s), %zu assets", manifest.project.c_str(),
                    cook::ConfigurationName(config), paks.size(), manifest.assets.size());
    for (const std::string& dlc : package.Dlcs()) AETHER_LOG_INFO("Player", "DLC: %s", dlc.c_str());

    const QualityPreset* quality = ChooseQuality(manifest, options.quality);
    if (!options.quality.empty() && (!quality || quality->name != options.quality)) {
        AETHER_LOG_WARN("Player", "No quality preset '%s'", options.quality.c_str());
    }
    if (quality) {
        AETHER_LOG_INFO("Player", "Quality: %s (resolution x%.2f, %u shadow cascades at %u, MSAA x%u)", quality->name.c_str(),
                        quality->resolution_scale, quality->shadow_cascades, quality->shadow_resolution,
                        quality->msaa_samples);
    }

    Game game(package);
    const bool loaded = options.scene.empty() ? game.LoadStartupScene(&error) : game.LoadScene(options.scene, &error);
    if (!loaded) {
        std::fprintf(stderr, "aether_player: %s\n", error.c_str());
        return 1;
    }

    try {
        std::unique_ptr<Window> window;
        std::unique_ptr<rhi::IDevice> device;
        std::unique_ptr<rhi::ISwapChain> swap_chain;
        std::unique_ptr<rhi::ICommandList> cmd;
        bool resized = false;
        if (!options.headless) {
            WindowDesc desc;
            desc.title = !manifest.window_title.empty() ? manifest.window_title
                         : manifest.project.empty()    ? "Aether"
                                                       : manifest.project;
            if (config != cook::BuildConfiguration::Shipping) {
                desc.title += std::string(" (") + cook::ConfigurationName(config) + ")";
            }
            desc.width = options.width ? options.width : manifest.window_width;
            desc.height = options.height ? options.height : manifest.window_height;
            window = std::make_unique<Window>(desc);
            if (!window->IsValid()) {
                AETHER_LOG_WARN("Player", "No window could be opened; running headless");
                window.reset();
            }
        }
        if (window) {
#if defined(_WIN32)
            rhi::Backend backend = options.backend == "vulkan" ? rhi::Backend::Vulkan : rhi::Backend::D3D12;
            void* native = window->NativeHandle();
#else
            rhi::Backend backend = rhi::Backend::Vulkan;
            void* native = window->PlatformWindow();
#endif
            device = rhi::CreateDevice(backend, config == cook::BuildConfiguration::Debug);
            if (device) {
                swap_chain = device->CreateSwapChain(native, window->Width(), window->Height(), 2);
                cmd = device->CreateCommandList();
                window->on_resize = [&](u32, u32) { resized = true; };
            } else {
                AETHER_LOG_WARN("Player", "No graphics device; running without drawing");
            }
        }

        for (const std::string& name : options.pressed) {
            const input::Key key = input::KeyFromName(name);
            if (key == input::Key::None) {
                std::fprintf(stderr, "aether_player: no key named '%s'\n", name.c_str());
                return 2;
            }
            game.Input().SetButton(key, true);
        }
        game.BeginPlay();
        u64 fence = 0;
        auto last = std::chrono::steady_clock::now();
        auto last_stats = last;
        for (i64 frame = 0; options.frames < 0 || frame < options.frames; ++frame) {
            if (window) {
                if (!window->PumpMessages()) break;
                ApplyWindowEvents(window->TakeEvents(), game.Input());
                if (window->IsMinimized()) continue;
            }
            const auto now = std::chrono::steady_clock::now();
            // Headless runs step at the fixed rate, so a run is reproducible.
            const f32 dt = window ? (std::min)(std::chrono::duration<f32>(now - last).count(), 0.25f)
                                  : 1.0f / manifest.fixed_timestep_hz;
            last = now;
            game.Tick(dt);

            if (swap_chain) {
                device->WaitForFence(fence);
                if (resized) {
                    swap_chain->Resize(window->Width(), window->Height());
                    resized = false;
                }
                swap_chain->AcquireNextImage();
                cmd->Reset();
                cmd->BeginRenderPass(*swap_chain, {0.32f, 0.45f, 0.62f, 1.0f});
                cmd->EndRenderPass();
                cmd->Close();
                fence = device->Submit(*cmd, swap_chain.get());
                swap_chain->Present(manifest.vsync);
            }
            if (config != cook::BuildConfiguration::Shipping && now - last_stats >= std::chrono::seconds(1)) {
                last_stats = now;
                const GameStats s = game.Stats();
                AETHER_LOG_INFO("Player", "frame %llu, %.1f s, %llu fixed steps, %zu entities, %zu bodies, %zu scripts, %zu Blueprints",
                                static_cast<unsigned long long>(s.frames), s.time,
                                static_cast<unsigned long long>(s.fixed_steps), s.entities, s.physics_bodies,
                                s.script_instances, s.blueprint_instances);
            }
        }
        if (device) device->WaitForFence(fence);
        if (options.report) {
            for (const Entity e : FindEntitiesWithTag(game.GetWorld(), "Player")) {
                if (const Transform* t = game.GetWorld().GetComponent<Transform>(e)) {
                    std::printf("Player at (%.3f, %.3f, %.3f)\n", t->position.x, t->position.y, t->position.z);
                }
            }
        }
        game.EndPlay();
        const GameStats s = game.Stats();
        AETHER_LOG_INFO("Player", "Ran %llu frames (%.2f s of game time, %llu fixed steps)",
                        static_cast<unsigned long long>(s.frames), s.time,
                        static_cast<unsigned long long>(s.fixed_steps));
        for (const std::string& e : game.ScriptErrors()) AETHER_LOG_ERROR("Player", "script: %s", e.c_str());
        for (const std::string& e : game.BlueprintErrors()) AETHER_LOG_ERROR("Player", "blueprint: %s", e.c_str());
        if (s.script_errors + s.blueprint_errors > 0) return 3; // the game ran with errors
    } catch (const std::exception& e) {
        std::fprintf(stderr, "aether_player: %s\n", e.what());
        return 1;
    }
    return 0;
}
