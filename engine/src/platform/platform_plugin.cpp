#include "aether/platform/platform_plugin.h"

#include "aether/core/log.h"
#include "aether/gfx/rhi/device.h"

namespace aether {

PlatformPlugins& PlatformPlugins::Get() {
    static PlatformPlugins instance;
    return instance;
}

bool PlatformPlugins::Register(PlatformPlugin plugin) {
    if (plugin.name.empty() || Find(plugin.name)) {
        AETHER_LOG_ERROR("Platform", "Platform plugin '%s' is already registered (or has no name)",
                         plugin.name.c_str());
        return false;
    }
    plugins_.push_back(std::move(plugin));
    if (started_ && plugins_.back().startup) {
        plugins_.back().startup(); // registered late: start it now
    }
    return true;
}

const PlatformPlugin* PlatformPlugins::Find(const std::string& name) const {
    for (const PlatformPlugin& p : plugins_) {
        if (p.name == name) return &p;
    }
    return nullptr;
}

void PlatformPlugins::Startup() {
    if (started_) return;
    started_ = true;
    for (const PlatformPlugin& p : plugins_) {
        AETHER_LOG_INFO("Platform", "Starting platform plugin '%s'", p.name.c_str());
        if (p.startup) p.startup();
    }
}

void PlatformPlugins::Shutdown() {
    if (!started_) return;
    started_ = false;
    for (auto it = plugins_.rbegin(); it != plugins_.rend(); ++it) {
        if (it->shutdown) it->shutdown();
    }
}

std::string PlatformPlugins::UserDataDir(const std::string& fallback) const {
    for (const PlatformPlugin& p : plugins_) {
        if (p.user_data_dir) {
            std::string dir = p.user_data_dir();
            if (!dir.empty()) return dir;
        }
    }
    return fallback;
}

std::unique_ptr<gfx::rhi::IDevice> PlatformPlugins::CreateDevice(const std::string& name,
                                                                  bool enable_debug_layer) const {
    const PlatformPlugin* plugin = Find(name);
    if (!plugin || !plugin->create_device) return nullptr;
    return plugin->create_device(enable_debug_layer);
}

void PlatformPlugins::ClearForTesting() {
    plugins_.clear();
    started_ = false;
}

} // namespace aether
