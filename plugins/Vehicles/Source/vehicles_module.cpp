// The Vehicles plugin's runtime module: registers its components, so scenes can name it, when the plugin is enabled.
#include "aether/vehicles/vehicle.h"
#include "aether/plugin/plugin.h"

namespace {

class VehiclesModule : public aether::plugin::IModule {
public:
    void Startup(const aether::plugin::ModuleContext&) override {
        using namespace aether;
        vehicles::RegisterVehicleComponents();
    }
};

} // namespace

AETHER_MODULE(Vehicles, VehiclesModule);
