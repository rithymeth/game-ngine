#include "aether/gameplay/effect_library.h"

#include "aether/gameplay/effect_system.h"

namespace aether::gas {

i32 Effects::ApplyEffect(const Entity& target, const std::string& effect, const Entity& source) {
    EffectSystem* s = EffectSystem::Active();
    return s ? static_cast<i32>(s->Apply(target, effect, source).handle) : 0;
}
bool Effects::RemoveEffect(i32 handle) {
    EffectSystem* s = EffectSystem::Active();
    return s && handle > 0 && s->Remove(static_cast<EffectHandle>(handle));
}
i32 Effects::RemoveEffectsByTag(const Entity& target, const std::string& tag) {
    EffectSystem* s = EffectSystem::Active();
    return s ? s->RemoveByTag(target, GameplayTag::Make(tag)) : 0;
}
bool Effects::HasActiveEffect(const Entity& target, const std::string& effect) {
    const EffectSystem* s = EffectSystem::Active();
    return s && s->HasEffect(target, effect);
}
i32 Effects::GetActiveEffectCount(const Entity& target) {
    const EffectSystem* s = EffectSystem::Active();
    return s ? s->ActiveCount(target) : 0;
}

} // namespace aether::gas
