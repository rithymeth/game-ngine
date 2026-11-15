#include "aether/checkpoints/checkpoint.h"

#include "aether/ecs/world.h"

namespace aether::chk {

void RegisterCheckpointComponents() { (void)GetComponentId<Checkpoint>(); }

} // namespace aether::chk
