#include "aether/forcefields/force_field.h"

#include "aether/ecs/world.h"

namespace aether::ff {

void RegisterForceFieldComponents() { (void)GetComponentId<ForceField>(); }

} // namespace aether::ff
