#include "aether/vehicles/vehicles_kit.h"

#include "aether/vehicles/vehicle.h"

#include <string>

namespace aether::vehicles {

namespace {

class VehiclesKit : public kit::IKit {
public:
    const char* Name() const override { return "Vehicles"; }
    std::vector<std::string> Deps() const override { return {}; }
    void RegisterComponents() override { RegisterVehicleComponents(); }
    void InstallScriptApi(void* context) override {
        auto* api = static_cast<kit::KitScriptApiContext*>(context);
        if (api && api->install) api->install(Name());
    }
};

} // namespace

std::unique_ptr<kit::IKit> MakeVehiclesKit() { return std::make_unique<VehiclesKit>(); }

} // namespace aether::vehicles
