// The Destruction plugin's runtime module: registers its components, so scenes can name it, when the plugin is enabled.
#include "aether/destruction/destructible.h"
#include "aether/plugin/plugin.h"

namespace {

class DestructionModule : public aether::plugin::IModule {
public:
    void Startup(const aether::plugin::ModuleContext&) override {
        using namespace aether;
        destruction::RegisterDestructionComponents();
    }
};

} // namespace

AETHER_MODULE(Destruction, DestructionModule);
