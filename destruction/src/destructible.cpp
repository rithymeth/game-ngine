#include "aether/destruction/destructible.h"

#include "aether/ecs/world.h"

namespace aether::destruction {

void RegisterDestructionComponents() {
    (void)GetComponentId<Destructible>();
    (void)GetComponentId<Debris>();
}

} // namespace aether::destruction
