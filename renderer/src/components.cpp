#include "aether/renderer/components.h"

#include "aether/ecs/component.h"
#include "aether/renderer/material_instance.h"

namespace aether {

void RegisterRenderComponents() {
    (void)GetComponentId<DirectionalLight>();
    (void)GetComponentId<PointLight>();
    (void)GetComponentId<SpotLight>();
    (void)GetComponentId<SkyLight>();
    (void)GetComponentId<PostProcessVolume>();
    (void)GetComponentId<MaterialParameters>();
}

} // namespace aether
