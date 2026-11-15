#include "aether/spawner/spawner.h"

#include "aether/ecs/world.h"

namespace aether::spawn {

void RegisterSpawnerComponents() { (void)GetComponentId<Spawner>(); }

} // namespace aether::spawn
