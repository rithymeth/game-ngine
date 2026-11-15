// The Loot plugin's runtime module. The loot kit has no components of its own to register, so this module has nothing to start; the plugin exists to name
// its dependency (Inventory) and to give the kit its place in the plugin set.
#include "aether/plugin/plugin.h"

namespace {

class LootModule : public aether::plugin::IModule {
public:
    void Startup(const aether::plugin::ModuleContext&) override {
        // The loot kit registers no components: the plugin names its dependency (Inventory) and carries the kit's place in the plugin set.
    }
};

} // namespace

AETHER_MODULE(Loot, LootModule);
