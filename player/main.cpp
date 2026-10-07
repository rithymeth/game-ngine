// aether_player: a cooked game, without the editor (Phase 25 step 4,
// docs/design/PHASE_SPECS.md §25.4). It mounts the game's .apak archives,
// loads the startup scene and runs it on the fixed-timestep frame loop, in
// a window drawn through the RHI (D3D12 on Windows, Vulkan elsewhere), or
// headless.
//
//   aether_player [--pak <file|dir>]... [--scene <path>] [--frames <n>]
//                 [--headless] [--backend d3d12|vulkan] [--size <w>x<h>]
//                 [--quality <preset>] [--key <64 hex digits>]
//                 [--press <Key>]... [--report] [--user-dir <dir>]
//
// Encrypted archives (§25.6) open with --key, or AETHER_PAK_KEY in the
// environment, or the key a game's player was built with
// (-DAETHER_GAME_PAK_KEY=<hex>).
//
// The window's title, size and vsync, and the quality preset, come from the
// player's settings file (settings.asettings, §28.6) if it has one, else the
// project's settings (§25.5), unless given here. Saves and settings live in
// the player's folder: --user-dir, else AETHER_USER_DIR, else the OS's
// per-user data folder (Saves/ and Config/ inside it). A settings file is
// only written when this run changed something (the old
// Saved/Config/Input.json rebinds are moved into it once).
//
// With no --pak it mounts every .apak in Paks/ beside the executable (then
// in the working directory). The manifest's configuration sets the log
// level: Debug logs everything, Development logs info and a stats line
// every second, Shipping only warnings and errors.

#include "aether/player/bindings_migrate.h"
#include "aether/player/game.h"
#include "aether/player/scene_renderer.h"

#include "aether/core/log.h"
#include "aether/gameplay/attribute_set.h"
#include "aether/gfx/rhi/device.h"
#include "aether/platform/window.h"
#include "aether/reflection/serialize.h"
#include "aether/scene/components.h"
#include "aether/scene/gameplay.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
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
    std::string user_dir; // empty: AETHER_USER_DIR, then the OS's folder
    std::vector<std::string> keys;
    std::vector<std::string> pressed; // keys held for the whole run (headless runs, smoke tests)
    std::string screenshot;
    std::string replay_input;          // recorded input for deterministic package verification
    bool report = false;              // print where the Player-tagged entity ended up
};

void Usage() {
    std::fprintf(stderr,
                 "usage: aether_player [--pak <file|dir>]... [--scene <path>] [--frames <n>] [--headless]\n"
                 "                     [--backend d3d12|vulkan] [--size <w>x<h>] [--quality <preset>]\n"
                 "                     [--key <64 hex digits>] [--press <Key>]... [--screenshot <file.bmp>]\n"
                 "                     [--report] [--user-dir <dir>] [--replay-input <file.json>]\n");
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
        } else if (a == "--screenshot" && has_value) {
            o.screenshot = argv[++i];
        } else if (a == "--report") {
            o.report = true;
        } else if (a == "--replay-input" && has_value) {
            o.replay_input = argv[++i];
        } else if (a == "--key" && has_value) {
            o.keys.push_back(argv[++i]);
        } else if (a == "--user-dir" && has_value) {
            o.user_dir = argv[++i];
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

void WriteU16(std::ostream& stream, u16 value) {
    const char bytes[2] = {static_cast<char>(value & 0xff), static_cast<char>((value >> 8) & 0xff)};
    stream.write(bytes, sizeof(bytes));
}

void WriteU32(std::ostream& stream, u32 value) {
    const char bytes[4] = {static_cast<char>(value & 0xff), static_cast<char>((value >> 8) & 0xff),
                           static_cast<char>((value >> 16) & 0xff), static_cast<char>((value >> 24) & 0xff)};
    stream.write(bytes, sizeof(bytes));
}

bool SaveScreenshotBmp(const std::string& path, u32 width, u32 height, const std::vector<u8>& rgba) {
    if (width == 0 || height == 0 || rgba.size() != static_cast<usize>(width) * height * 4) return false;
    std::ofstream file(path, std::ios::binary);
    if (!file) return false;
    const u32 bytes = width * height * 4;
    file.put('B');
    file.put('M');
    WriteU32(file, 54 + bytes);
    WriteU16(file, 0);
    WriteU16(file, 0);
    WriteU32(file, 54);
    WriteU32(file, 40);
    WriteU32(file, width);
    WriteU32(file, static_cast<u32>(-static_cast<i32>(height)));
    WriteU16(file, 1);
    WriteU16(file, 32);
    WriteU32(file, 0);
    WriteU32(file, bytes);
    WriteU32(file, 2835);
    WriteU32(file, 2835);
    WriteU32(file, 0);
    WriteU32(file, 0);
    for (usize i = 0; i < rgba.size(); i += 4) {
        const char bgra[4] = {static_cast<char>(rgba[i + 2]), static_cast<char>(rgba[i + 1]),
                              static_cast<char>(rgba[i]), static_cast<char>(rgba[i + 3])};
        file.write(bgra, sizeof(bgra));
    }
    return file.good();
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

    // The player's folder, settings and old rebinds, before the scene: the
    // settings decide the window and quality, the rebinds the input contexts.
    Game game(package);
    const UserPaths user_paths = ResolveUserPaths(manifest.project, options.user_dir);
    game.SetUserPaths(user_paths);
    // The window can't change after it opens, so the settings' window values
    // are only recorded here, then used when it is created below.
    struct Chosen {
        u32 width = 0, height = 0;
        bool fullscreen = false, vsync = true;
    } chosen;
    SettingsTargets targets;
    targets.set_window = [&](u32 w, u32 h, bool fullscreen, bool vsync) { chosen = {w, h, fullscreen, vsync}; };
    for (const std::string& w : game.LoadSettings(targets)) AETHER_LOG_WARN("Player", "%s", w.c_str());
    // A settings file counts only if there is one: the defaults (1280x720,
    // High) must not override the project's own.
    std::error_code settings_ec;
    const bool have_settings = stdfs::exists(game.Settings().Path(), settings_ec);
    std::string saved_json = reflect::ToJson(game.Settings().Get()).dump();
    for (const std::string& w : MigrateLegacyBindings(user_paths.root, game.Settings())) AETHER_LOG_WARN("Player", "%s", w.c_str());
    const auto settings_changed = [&] { return reflect::ToJson(game.Settings().Get()).dump() != saved_json; };
    if (settings_changed()) { // the old rebinds were moved in and their file renamed: keep them
        for (const std::string& w : game.Settings().Save().warnings) AETHER_LOG_WARN("Player", "%s", w.c_str());
        saved_json = reflect::ToJson(game.Settings().Get()).dump();
    }

    const std::string wanted_quality = !options.quality.empty() ? options.quality : (have_settings ? game.Settings().Get().quality : std::string());
    const QualityPreset* quality = ChooseQuality(manifest, wanted_quality);
    if (!wanted_quality.empty() && (!quality || quality->name != wanted_quality)) {
        AETHER_LOG_WARN("Player", "No quality preset '%s'", wanted_quality.c_str());
    }
    if (quality) {
        AETHER_LOG_INFO("Player", "Quality: %s (resolution x%.2f, %u shadow cascades at %u, MSAA x%u)", quality->name.c_str(),
                        quality->resolution_scale, quality->shadow_cascades, quality->shadow_resolution,
                        quality->msaa_samples);
    }
    const bool vsync = have_settings ? chosen.vsync : manifest.vsync;
    if (have_settings && chosen.fullscreen) AETHER_LOG_WARN("Player", "Fullscreen is saved in the settings but isn't supported by this window yet");

    const bool loaded = options.scene.empty() ? game.LoadStartupScene(&error) : game.LoadScene(options.scene, &error);
    if (!loaded) {
        std::fprintf(stderr, "aether_player: %s\n", error.c_str());
        return 1;
    }

    try {
        struct ReplayEvent { i64 frame; input::Key key; f32 value; };
        std::vector<ReplayEvent> replay;
        if (!options.replay_input.empty()) {
            std::ifstream stream(options.replay_input);
            const auto json = reflect::Json::parse(stream);
            const i64 frames = json.at("frames").get<i64>();
            if (frames <= 0 || frames > 360000) throw std::runtime_error("Invalid replay frame count");
            if (options.frames >= 0 && options.frames != frames) throw std::runtime_error("Replay frame count differs from --frames");
            options.frames = frames;
            i64 previous = 0;
            for (const auto& event : json.at("events")) {
                const i64 frame = event.at("frame").get<i64>();
                const auto key = input::KeyFromName(event.at("key").get<std::string>());
                const f32 value = event.at("value").get<f32>();
                if (frame < previous || frame >= frames || key == input::Key::None || !std::isfinite(value))
                    throw std::runtime_error("Invalid replay input event");
                replay.push_back({frame, key, value});
                previous = frame;
            }
        }
        usize replay_cursor = 0;
        input::InputState replay_held;
        std::unique_ptr<Window> window;
        std::unique_ptr<rhi::IDevice> device;
        std::unique_ptr<rhi::ISwapChain> swap_chain;
        std::unique_ptr<rhi::ICommandList> cmd;
        std::unique_ptr<SceneRenderer> scene_renderer;
        bool resized = false;
        if (!options.headless) {
            WindowDesc desc;
            desc.title = !manifest.window_title.empty() ? manifest.window_title
                         : manifest.project.empty()    ? "Aether"
                                                       : manifest.project;
            if (config != cook::BuildConfiguration::Shipping) {
                desc.title += std::string(" (") + cook::ConfigurationName(config) + ")";
            }
            desc.width = options.width ? options.width : (have_settings ? chosen.width : manifest.window_width);
            desc.height = options.height ? options.height : (have_settings ? chosen.height : manifest.window_height);
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
                scene_renderer = std::make_unique<SceneRenderer>(*device, *swap_chain, package);
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
        if (window && !game.StartAudioOutput(&error)) AETHER_LOG_WARN("Player", "Audio output: %s", error.c_str());
        game.BeginPlay();
        u64 fence = 0;
        bool screenshot_written = false;
        bool screenshot_failed = false;
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
            const f32 dt = window && options.replay_input.empty() ? (std::min)(std::chrono::duration<f32>(now - last).count(), 0.25f)
                                  : 1.0f / manifest.fixed_timestep_hz;
            last = now;
            if (!options.screenshot.empty()) {
                // Ignore incidental cursor motion during a proof capture so
                // its camera stays at the startup scene's deterministic pose.
                game.Input().EndFrame();
            }
            if (!options.replay_input.empty()) {
                // The trace owns raw input; incidental window input cannot alter verification.
                replay_held.EndFrame();
                while (replay_cursor < replay.size() && replay[replay_cursor].frame == frame) {
                    const auto& event = replay[replay_cursor++];
                    replay_held.SetAxis(event.key, event.value);
                }
                game.Input() = replay_held;
            }
            game.Tick(dt);
            if (game.ExitRequested()) break;

            if (swap_chain) {
                device->WaitForFence(fence);
                if (resized) {
                    swap_chain->Resize(window->Width(), window->Height());
                    resized = false;
                }
                swap_chain->AcquireNextImage();
                cmd->Reset();
                if (scene_renderer) {
                    scene_renderer->Draw(game, *cmd);
                } else {
                    cmd->BeginRenderPass(*swap_chain, {0.015f, 0.025f, 0.045f, 1.0f});
                    cmd->EndRenderPass();
                }
                cmd->Close();
                fence = device->Submit(*cmd, swap_chain.get());
                const bool should_capture = options.frames < 0 || frame + 1 >= options.frames;
                if (!options.screenshot.empty() && !screenshot_written && should_capture) {
                    std::vector<u8> rgba;
                    screenshot_written = true;
                    if (swap_chain->ReadBack(rgba) &&
                        SaveScreenshotBmp(options.screenshot, swap_chain->Width(), swap_chain->Height(), rgba)) {
                        AETHER_LOG_INFO("Player", "Saved rendered frame to %s", options.screenshot.c_str());
                    } else {
                        AETHER_LOG_ERROR("Player", "Couldn't read back or save rendered frame to %s", options.screenshot.c_str());
                        screenshot_failed = true;
                    }
                }
                swap_chain->Present(vsync);
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
                if (const auto* attributes = game.GetWorld().GetComponent<gas::AttributeSet>(e)) {
                    std::printf("Player attributes: %s\n", reflect::Json({{"Health", attributes->Get("Health")},
                        {"MissionStage", attributes->Get("MissionStage")}, {"ArchiveTime", attributes->Get("ArchiveTime")}}).dump().c_str());
                }
            }
        }
        game.EndPlay();
        if (!options.screenshot.empty() && (!screenshot_written || screenshot_failed)) return 4;
        // Settings are written only if the game changed them: a damaged file
        // isn't overwritten by a run that never touched it.
        if (settings_changed()) {
            for (const std::string& w : game.Settings().Save().warnings) AETHER_LOG_WARN("Player", "%s", w.c_str());
        }
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
