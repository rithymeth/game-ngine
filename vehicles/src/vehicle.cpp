#include "aether/vehicles/vehicle.h"

#include "aether/ecs/world.h"

namespace aether::vehicles {

void RegisterVehicleComponents() {
    (void)GetComponentId<Vehicle>();
    (void)GetComponentId<VehicleInput>();
    (void)GetComponentId<VehicleState>();
}

} // namespace aether::vehicles
