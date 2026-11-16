// The Fire plugin's runtime module: registers its components, so scenes can name it, when the plugin is enabled.
#include "aether/fire/fire.h"
#include "aether/plugin/plugin.h"

namespace {

class FireModule : public aether::plugin::IModule {
public:
    void Startup(const aether::plugin::ModuleContext&) override {
        using namespace aether;
        fire::RegisterFireComponents();
    }
};

} // namespace

AETHER_MODULE(Fire, FireModule);
