// The Interaction plugin's runtime module: registers its components, so scenes can
// name them, when the plugin is enabled.
#include "aether/interaction/interaction_system.h"
#include "aether/plugin/plugin.h"

namespace {

class InteractionModule : public aether::plugin::IModule {
public:
    void Startup(const aether::plugin::ModuleContext&) override {
        using namespace aether;
        interact::RegisterInteractionComponents();
    }
};

} // namespace

AETHER_MODULE(Interaction, InteractionModule);
