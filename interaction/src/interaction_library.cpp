#include "aether/interaction/interaction_library.h"

#include "aether/interaction/interaction_system.h"

namespace aether::interact {

Entity Interaction::FindInteractable(const Entity& interactor, const Vec3& from, const Vec3& forward) {
    const InteractionSystem* s = InteractionSystem::Active();
    return s ? s->Focus(interactor, from, forward).target : Entity{};
}
std::string Interaction::GetInteractionPrompt(const Entity& target) {
    const InteractionSystem* s = InteractionSystem::Active();
    return s ? s->Prompt(target) : std::string();
}
std::string Interaction::GetPromptFor(const Entity& interactor, const Entity& target) {
    const InteractionSystem* s = InteractionSystem::Active();
    return s && s->Check(interactor, target) == Reason::None ? s->Prompt(target) : std::string();
}
bool Interaction::CanInteract(const Entity& interactor, const Entity& target) {
    const InteractionSystem* s = InteractionSystem::Active();
    return s && s->Check(interactor, target) == Reason::None;
}
bool Interaction::Interact(const Entity& interactor, const Entity& target) {
    InteractionSystem* s = InteractionSystem::Active();
    return s && s->Interact(interactor, target) == Reason::None;
}
bool Interaction::SetInteractable(const Entity& target, bool enabled) {
    InteractionSystem* s = InteractionSystem::Active();
    return s && s->SetEnabled(target, enabled);
}
bool Interaction::ResetInteractable(const Entity& target) {
    InteractionSystem* s = InteractionSystem::Active();
    return s && s->Reset(target);
}

} // namespace aether::interact
