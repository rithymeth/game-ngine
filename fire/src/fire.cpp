#include "aether/fire/fire.h"

#include "aether/ecs/world.h"

namespace aether::fire {

void RegisterFireComponents() { (void)GetComponentId<Flammable>(); }

} // namespace aether::fire
