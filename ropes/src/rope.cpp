#include "aether/ropes/rope.h"

#include "aether/ecs/world.h"

namespace aether::ropes {

void RegisterRopeComponents() {
    (void)GetComponentId<Rope>();
    (void)GetComponentId<RopeSegment>();
}

} // namespace aether::ropes
