// The Crafting plugin's runtime module. The crafting kit has no components of its own to register, so this module has nothing to start; the
// plugin exists to name its dependency (Inventory) and to give the kit its place in the plugin set.
#include "aether/plugin/plugin.h"

namespace {

class CraftingModule : public aether::plugin::IModule {
public:
    void Startup(const aether::plugin::ModuleContext&) override {
        // The crafting kit registers no components: the plugin names its dependency (Inventory) and carries the kit's place in the plugin set.
    }
};

} // namespace

AETHER_MODULE(Crafting, CraftingModule);
