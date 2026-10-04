// A template platform plugin. A real one (Android, a console) would bring
// up its SDK in startup, mount its file systems, report where saves go, and
// hand out an RHI device for its graphics API.
#include "aether/platform/platform_plugin.h"

#include <atomic>

namespace {
std::atomic<int> g_started{0};
}

// For the tests: how many times startup has run.
int AetherExamplePlatformStartCount() {
    return g_started.load();
}

AETHER_PLATFORM_PLUGIN(example) {
    aether::PlatformPlugin plugin;
    plugin.name = "example";
    plugin.description = "Template platform plugin (platforms/example)";
    plugin.startup = [] { ++g_started; };
    plugin.user_data_dir = [] { return std::string("example-saves"); };
    return plugin;
}
