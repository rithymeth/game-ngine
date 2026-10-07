#pragma once

#include "aether/kit/kit.h"
#include "aether/gameplay/ability_system.h"
#include "aether/gameplay/attribute_system.h"
#include "aether/gameplay/effect_system.h"
#include "aether/scene/scheduler.h"

#include <memory>
#include <functional>
#include <string>
#include <vector>

namespace aether::gas {

// The gameplay module as a kit (Phase 37 step 4): tags, attributes, effects and abilities. The other gameplay
// kits depend on it.
std::unique_ptr<kit::IKit> MakeGameplayKit(EffectLibrary* effects = nullptr, AbilityLibrary* abilities = nullptr);

SystemDesc MakeEffectSystem(EffectSystem* system, std::function<void(const EffectEvent&)> on_event);
SystemDesc MakeAbilitySystem(AbilitySystem* system, std::function<void(const AbilityEvent&)> on_event);
SystemDesc MakeAttributeSystem(AttributeSystem* system, std::function<void(const AttributeEvent&)> on_event);

struct GameplayAssetDiagnostic {
    std::string path;
    std::string message;
    bool content_read_failure = false;
};

std::vector<GameplayAssetDiagnostic> LoadEffectAssets(
    const std::vector<std::string>& paths,
    std::function<bool(const std::string&, std::vector<u8>&, std::string*)> read_content,
    EffectLibrary& effects);
std::vector<GameplayAssetDiagnostic> LoadAbilityAssets(
    const std::vector<std::string>& paths,
    std::function<bool(const std::string&, std::vector<u8>&, std::string*)> read_content,
    const EffectLibrary& effects, AbilityLibrary& abilities);

} // namespace aether::gas
