// The Spawner plugin's runtime module: registers its component, so scenes can name it, when the plugin is enabled.
#include "aether/plugin/plugin.h"
#include "aether/spawner/spawner.h"

namespace {

class SpawnerModule : public aether::plugin::IModule {
public:
    void Startup(const aether::plugin::ModuleContext&) override {
        using namespace aether;
        spawn::RegisterSpawnerComponents();
    }
};

} // namespace

AETHER_MODULE(Spawner, SpawnerModule);
