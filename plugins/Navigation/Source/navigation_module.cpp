// The Navigation plugin's runtime module (Phase 26 step 1): registers its
// components, so scenes can name them, when the plugin is enabled.
#include "aether/nav/components.h"
#include "aether/nav/crowd.h"
#include "aether/plugin/plugin.h"

namespace {

class NavigationModule : public aether::plugin::IModule {
public:
    void Startup(const aether::plugin::ModuleContext&) override {
        using namespace aether;
        RegisterNavComponents();
        RegisterNavAgentComponents();
    }
};

} // namespace

AETHER_MODULE(Navigation, NavigationModule);
