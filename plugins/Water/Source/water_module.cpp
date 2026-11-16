// The Water plugin's runtime module: registers its components, so scenes can name it, when the plugin is enabled.
#include "aether/water/water.h"
#include "aether/plugin/plugin.h"

namespace {

class WaterModule : public aether::plugin::IModule {
public:
    void Startup(const aether::plugin::ModuleContext&) override {
        using namespace aether;
        water::RegisterWaterComponents();
    }
};

} // namespace

AETHER_MODULE(Water, WaterModule);
