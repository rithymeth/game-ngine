#include "aether/gameplay/ability_library.h"

#include "aether/gameplay/gameplay_kit.h"

namespace aether::gas {

bool Abilities::GrantAbility(const Entity& owner, const std::string& ability) {
    AbilitySystem* s = GameplayLibraryScope::Abilities();
    return s && s->Grant(owner, ability);
}
bool Abilities::RevokeAbility(const Entity& owner, const std::string& ability) {
    AbilitySystem* s = GameplayLibraryScope::Abilities();
    return s && s->Revoke(owner, ability);
}
i32 Abilities::TryActivateAbility(const Entity& owner, const std::string& ability) {
    AbilitySystem* s = GameplayLibraryScope::Abilities();
    return s ? static_cast<i32>(s->TryActivate(owner, ability).handle) : 0;
}
bool Abilities::CanActivateAbility(const Entity& owner, const std::string& ability) {
    const AbilitySystem* s = GameplayLibraryScope::Abilities();
    return s && s->CanActivate(owner, ability) == FailReason::None;
}
bool Abilities::CommitAbility(i32 handle) {
    AbilitySystem* s = GameplayLibraryScope::Abilities();
    return s && handle > 0 && s->Commit(static_cast<AbilityHandle>(handle));
}
bool Abilities::EndAbility(i32 handle) {
    AbilitySystem* s = GameplayLibraryScope::Abilities();
    return s && handle > 0 && s->End(static_cast<AbilityHandle>(handle));
}
bool Abilities::CancelAbility(i32 handle) {
    AbilitySystem* s = GameplayLibraryScope::Abilities();
    return s && handle > 0 && s->Cancel(static_cast<AbilityHandle>(handle));
}
bool Abilities::IsAbilityActive(const Entity& owner, const std::string& ability) {
    const AbilitySystem* s = GameplayLibraryScope::Abilities();
    return s && s->IsActive(owner, ability);
}
bool Abilities::IsAbilityGranted(const Entity& owner, const std::string& ability) {
    const AbilitySystem* s = GameplayLibraryScope::Abilities();
    return s && s->IsGranted(owner, ability);
}

} // namespace aether::gas
