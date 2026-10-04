#pragma once

#include "aether/core/base.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace aether::gfx::rhi {
class IDevice;
}

namespace aether {

// A platform plugin (Phase 24, docs/design/PHASE_SPECS.md §24.5): how a
// platform the engine doesn't build in - Android, a console - plugs itself
// in without the engine knowing about it. The plugin's code lives outside
// the engine (platforms/<name>/, or a private repository for consoles),
// is built with aether_platform_plugin() from cmake/AetherPlatform.cmake,
// and registers itself at startup with AETHER_PLATFORM_PLUGIN. Every hook
// is optional.
struct PlatformPlugin {
    std::string name;            // "android", a console's name, ...
    std::string description;
    // Process-wide setup and teardown, in registration order (shutdown in
    // reverse): the platform's SDK, its file system mounts, its threads.
    std::function<void()> startup;
    std::function<void()> shutdown;
    // Where saves and settings go on this platform; empty for the default.
    std::function<std::string()> user_data_dir;
    // An RHI device on a graphics API the engine doesn't build in.
    std::function<std::unique_ptr<gfx::rhi::IDevice>(bool enable_debug_layer)> create_device;
};

class PlatformPlugins {
public:
    static PlatformPlugins& Get();

    // Adds a plugin; false (and nothing added) when one has the same name.
    bool Register(PlatformPlugin plugin);
    const PlatformPlugin* Find(const std::string& name) const;
    const std::vector<PlatformPlugin>& All() const { return plugins_; }

    // Runs every plugin's startup, then (on Shutdown) every shutdown in
    // reverse. Each runs once however often these are called.
    void Startup();
    void Shutdown();
    bool IsStarted() const { return started_; }

    // The first plugin's user data directory, or `fallback`.
    std::string UserDataDir(const std::string& fallback) const;
    // A device from the named plugin's factory; null when there's no such
    // plugin, or it has no factory.
    std::unique_ptr<gfx::rhi::IDevice> CreateDevice(const std::string& name, bool enable_debug_layer) const;

    // Tests only.
    void ClearForTesting();

private:
    std::vector<PlatformPlugin> plugins_;
    bool started_ = false;
};

namespace detail {
struct PlatformPluginRegistrar {
    explicit PlatformPluginRegistrar(PlatformPlugin (*make)()) { PlatformPlugins::Get().Register(make()); }
};
} // namespace detail

// In one .cpp of the plugin:
//   AETHER_PLATFORM_PLUGIN(android) {
//       PlatformPlugin p;
//       p.name = "android";
//       p.startup = [] { ... };
//       return p;
//   }
#define AETHER_PLATFORM_PLUGIN(id)                                                                   \
    static ::aether::PlatformPlugin AetherMakePlatformPlugin_##id();                                 \
    static const ::aether::detail::PlatformPluginRegistrar aether_platform_plugin_registrar_##id(    \
        &AetherMakePlatformPlugin_##id);                                                             \
    static ::aether::PlatformPlugin AetherMakePlatformPlugin_##id()

} // namespace aether
