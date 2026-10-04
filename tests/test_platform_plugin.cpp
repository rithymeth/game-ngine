#include "aether/gfx/rhi/device.h"
#include "aether/platform/platform_plugin.h"
#include "test_framework.h"

#include <string>
#include <vector>

// Phase 24 step 6: platform plugins - registration, startup and shutdown
// order, user data directories, device factories, and (when the build
// lists platforms/example in AETHER_PLATFORM_PLUGINS) a real plugin
// registering itself from its own library.

using namespace aether;

#define CHECK(...) AETHER_CHECK((__VA_ARGS__))

#if defined(AETHER_HAS_EXAMPLE_PLATFORM)
int AetherExamplePlatformStartCount();
#endif

AETHER_TEST(PlatformPlugin_RegistryOrderAndHooks) {
    PlatformPlugins plugins;
    std::vector<std::string> calls;
    const auto make = [&](const char* name, const char* dir) {
        PlatformPlugin p;
        p.name = name;
        p.startup = [&calls, name] { calls.push_back(std::string("start ") + name); };
        p.shutdown = [&calls, name] { calls.push_back(std::string("stop ") + name); };
        if (dir) p.user_data_dir = [dir] { return std::string(dir); };
        return p;
    };
    CHECK(plugins.Register(make("first", "")));
    CHECK(plugins.Register(make("second", "saves/second")));
    CHECK(!plugins.Register(make("first", nullptr)) && !plugins.Register(PlatformPlugin{}));
    CHECK(plugins.All().size() == 2 && plugins.Find("second") && !plugins.Find("third"));

    // An empty directory falls through to the next plugin, then the fallback.
    CHECK(plugins.UserDataDir("default") == "saves/second");
    PlatformPlugins empty;
    CHECK(empty.UserDataDir("default") == "default");

    plugins.Startup();
    plugins.Startup(); // once only
    CHECK(plugins.IsStarted());
    CHECK(plugins.Register(make("late", nullptr))); // started as it registers
    plugins.Shutdown();
    plugins.Shutdown();
    CHECK(calls == (std::vector<std::string>{"start first", "start second", "start late", "stop late", "stop second",
                                             "stop first"}));

    // Device factories: none here, and an unknown plugin has none either.
    CHECK(plugins.CreateDevice("first", false) == nullptr && plugins.CreateDevice("nope", false) == nullptr);
    int asked = 0;
    PlatformPlugin gpu;
    gpu.name = "gpu";
    gpu.create_device = [&asked](bool) -> std::unique_ptr<gfx::rhi::IDevice> {
        ++asked;
        return nullptr;
    };
    CHECK(plugins.Register(std::move(gpu)));
    CHECK(plugins.CreateDevice("gpu", true) == nullptr && asked == 1);
    plugins.ClearForTesting();
    CHECK(plugins.All().empty() && !plugins.IsStarted());
}

#if defined(AETHER_HAS_EXAMPLE_PLATFORM)
AETHER_TEST(PlatformPlugin_ExampleRegistersItself) {
    PlatformPlugins& plugins = PlatformPlugins::Get();
    const PlatformPlugin* example = plugins.Find("example");
    CHECK(example != nullptr && example->startup && example->user_data_dir);
    CHECK(plugins.UserDataDir("default") == "example-saves");
    const int before = AetherExamplePlatformStartCount();
    plugins.Startup();
    CHECK(AetherExamplePlatformStartCount() == before + 1);
    plugins.Shutdown();
}
#endif
