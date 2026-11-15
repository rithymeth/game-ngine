// The Blueprint plugin's runtime module: registers its components, so scenes can
// name them, when the plugin is enabled.
#include "aether/blueprint/system.h"
#include "aether/plugin/plugin.h"

namespace {

class BlueprintModule : public aether::plugin::IModule {
public:
    void Startup(const aether::plugin::ModuleContext&) override {
        using namespace aether;
        bp::RegisterBlueprintComponents();
    }
};

} // namespace

AETHER_MODULE(Blueprint, BlueprintModule);
