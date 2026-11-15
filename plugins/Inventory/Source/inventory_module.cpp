// The Inventory plugin's runtime module: registers its components, so scenes can
// name them, when the plugin is enabled.
#include "aether/inventory/inventory_system.h"
#include "aether/plugin/plugin.h"

namespace {

class InventoryModule : public aether::plugin::IModule {
public:
    void Startup(const aether::plugin::ModuleContext&) override {
        using namespace aether;
        inv::RegisterInventoryComponents();
    }
};

} // namespace

AETHER_MODULE(Inventory, InventoryModule);
