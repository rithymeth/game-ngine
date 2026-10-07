#pragma once

#include "aether/kit/kit.h"
#include "aether/gameplay/ability_system.h"
#include "aether/gameplay/attribute_system.h"
#include "aether/gameplay/effect_system.h"
#include "aether/scene/scheduler.h"

#include <memory>

namespace aether::gas {

// The gameplay module as a kit (Phase 37 step 4): tags, attributes, effects and abilities. The other gameplay
// kits depend on it.
std::unique_ptr<kit::IKit> MakeGameplayKit();

SystemDesc MakeEffectSystem(EffectSystem* system, std::function<void(const EffectEvent&)> on_event);
SystemDesc MakeAbilitySystem(AbilitySystem* system, std::function<void(const AbilityEvent&)> on_event);
SystemDesc MakeAttributeSystem(AttributeSystem* system, std::function<void(const AttributeEvent&)> on_event);

} // namespace aether::gas
