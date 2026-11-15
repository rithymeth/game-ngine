// The Gameplay plugin's runtime module: registers its components, so scenes can
// name them, when the plugin is enabled.
#include "aether/gameplay/attribute_system.h"
#include "aether/plugin/plugin.h"

namespace {

class GameplayModule : public aether::plugin::IModule {
public:
    void Startup(const aether::plugin::ModuleContext&) override {
        using namespace aether;
        gas::RegisterGameplayComponents();
    }
};

} // namespace

AETHER_MODULE(Gameplay, GameplayModule);
