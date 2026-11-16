#include "aether/water/water.h"

#include "aether/ecs/world.h"

namespace aether::water {

void RegisterWaterComponents() {
    (void)GetComponentId<WaterSurface>();
    (void)GetComponentId<Floater>();
    (void)GetComponentId<Boat>();
}

} // namespace aether::water
