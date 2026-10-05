#include "aether/interaction/interaction_system.h"

#include "aether/ecs/component.h"
#include "aether/gameplay/tag_container.h"
#include "aether/interaction/interaction_library.h" // registers the Blueprint library wherever the system is linked
#include "aether/loc/localize.h"
#include "aether/scene/components.h"

#include <algorithm>

namespace aether::interact {

namespace {
InteractionSystem* g_active = nullptr;
constexpr f32 kEps = 1e-5f;
// What counts as "in front": within about 72 degrees of where the user is looking.
constexpr f32 kCone = 0.3f;
} // namespace

const char* ReasonName(Reason reason) {
    switch (reason) {
    case Reason::None: return "none";
    case Reason::DeadEntity: return "dead_entity";
    case Reason::NotInteractable: return "not_interactable";
    case Reason::Disabled: return "disabled";
    case Reason::OutOfRange: return "out_of_range";
    case Reason::MissingTag: return "missing_tag";
    case Reason::BlockedTag: return "blocked_tag";
    case Reason::Cooldown: return "cooldown";
    case Reason::Used: return "used";
    case Reason::ActionFailed: return "action_failed";
    }
    return "none";
}

void RegisterInteractionComponents() {
    gas::RegisterGameplayComponents();
    (void)GetComponentId<Interactable>();
}

InteractionSystem::InteractionSystem(World& world, gas::EffectSystem* effects, gas::AbilitySystem* abilities)
    : world_(world), effects_(effects), abilities_(abilities) {
    RegisterInteractionComponents();
    MakeActive();
}

InteractionSystem::~InteractionSystem() {
    if (g_active == this) g_active = nullptr;
}

InteractionSystem* InteractionSystem::Active() { return g_active; }
void InteractionSystem::MakeActive() { g_active = this; }

std::vector<Entity> InteractionSystem::Targets() const {
    std::vector<Entity> out;
    const ComponentId id = GetComponentId<Interactable>();
    world_.ForEachArchetype([&](const Archetype& a) {
        if (!a.Mask().test(id)) return;
        for (usize c = 0; c < a.ChunkCount(); ++c) {
            const Entity* chunk = a.EntityArray(c);
            out.insert(out.end(), chunk, chunk + a.ChunkEntityCount(c));
        }
    });
    std::sort(out.begin(), out.end(), [](Entity x, Entity y) { return x.index != y.index ? x.index < y.index : x.generation < y.generation; });
    return out;
}

Reason InteractionSystem::CheckAt(Entity interactor, Entity target, const Vec3* from) const {
    if (interactor.IsNull() || target.IsNull() || !world_.IsAlive(interactor) || !world_.IsAlive(target)) return Reason::DeadEntity;
    const Interactable* i = world_.HasComponent<Interactable>(target) ? world_.GetComponent<Interactable>(target) : nullptr;
    if (i == nullptr) return Reason::NotInteractable;
    if (!i->enabled) return Reason::Disabled;
    if (i->one_shot && i->used) return Reason::Used;
    if (i->remaining > kEps) return Reason::Cooldown;
    if (i->range > 0.0f && from != nullptr && world_.HasComponent<Transform>(target)) {
        if ((world_.GetComponent<Transform>(target)->position - *from).Length() > i->range + kEps) return Reason::OutOfRange;
    }
    if (!i->required_tags.empty() || !i->blocked_tags.empty()) {
        const gas::TagContainer none;
        const gas::TagContainer* tags = world_.HasComponent<gas::TagContainer>(interactor) ? world_.GetComponent<gas::TagContainer>(interactor) : &none;
        for (const std::string& t : i->required_tags) {
            if (!tags->HasTag(gas::GameplayTag::Make(t))) return Reason::MissingTag;
        }
        for (const std::string& t : i->blocked_tags) {
            if (tags->HasTag(gas::GameplayTag::Make(t))) return Reason::BlockedTag;
        }
    }
    return Reason::None;
}

Reason InteractionSystem::Check(Entity interactor, Entity target, const Vec3& from) const { return CheckAt(interactor, target, &from); }

Reason InteractionSystem::Check(Entity interactor, Entity target) const {
    if (!interactor.IsNull() && world_.IsAlive(interactor) && world_.HasComponent<Transform>(interactor)) {
        const Vec3 from = world_.GetComponent<Transform>(interactor)->position;
        return CheckAt(interactor, target, &from);
    }
    return CheckAt(interactor, target, nullptr); // no position: the range isn't checked
}

std::string InteractionSystem::Prompt(Entity target) const {
    if (target.IsNull() || !world_.IsAlive(target) || !world_.HasComponent<Interactable>(target)) return {};
    const Interactable* i = world_.GetComponent<Interactable>(target);
    return i->prompt_key.empty() ? i->prompt : loc::Localize::GetText(i->prompt_key, i->prompt);
}

FocusResult InteractionSystem::Focus(Entity interactor, const Vec3& from, const Vec3& forward) const {
    FocusResult best;
    if (interactor.IsNull() || !world_.IsAlive(interactor)) return best;
    const bool look = forward.LengthSq() > kEps;
    const Vec3 dir = look ? forward.Normalized() : Vec3();
    f32 best_distance = 0.0f;
    for (const Entity target : Targets()) {
        if (target == interactor || CheckAt(interactor, target, &from) != Reason::None) continue;
        if (!world_.HasComponent<Transform>(target)) continue;
        const Vec3 to = world_.GetComponent<Transform>(target)->position - from;
        const f32 d = to.Length();
        if (look && d > kEps && to.Normalized().Dot(dir) < kCone) continue;
        if (best.target.IsNull() || d < best_distance - kEps) {
            best.target = target;
            best_distance = d;
        }
    }
    if (!best.target.IsNull()) {
        best.distance = best_distance;
        best.prompt = Prompt(best.target);
    }
    return best;
}

Reason InteractionSystem::Interact(Entity interactor, Entity target) {
    const auto fail = [&](Reason r) {
        events_.push_back({target, interactor, InteractionEvent::Kind::Failed, r});
        return r;
    };
    const Reason reason = Check(interactor, target);
    if (reason != Reason::None) return fail(reason);
    // Copy what is needed first: applying effects can add components, which moves the target's data.
    const Interactable snapshot = *world_.GetComponent<Interactable>(target);
    if (!snapshot.ability.empty()) {
        if (abilities_ == nullptr || abilities_->TryActivate(interactor, snapshot.ability).handle == 0) return fail(Reason::ActionFailed);
    }
    if (!snapshot.effect.empty() && effects_ != nullptr) {
        if (effects_->Apply(interactor, snapshot.effect, target).status == gas::EffectSystem::Status::Rejected) return fail(Reason::ActionFailed);
    }
    Interactable* i = world_.GetComponent<Interactable>(target);
    if (snapshot.one_shot) i->used = true;
    if (snapshot.cooldown > 0.0f) i->remaining = snapshot.cooldown;
    events_.push_back({target, interactor, InteractionEvent::Kind::Interacted, Reason::None});
    return Reason::None;
}

bool InteractionSystem::SetEnabled(Entity target, bool enabled) {
    if (target.IsNull() || !world_.IsAlive(target) || !world_.HasComponent<Interactable>(target)) return false;
    world_.GetComponent<Interactable>(target)->enabled = enabled;
    return true;
}

bool InteractionSystem::Reset(Entity target) {
    if (target.IsNull() || !world_.IsAlive(target) || !world_.HasComponent<Interactable>(target)) return false;
    Interactable* i = world_.GetComponent<Interactable>(target);
    i->used = false;
    i->remaining = 0.0f;
    return true;
}

void InteractionSystem::Update(f32 dt) {
    if (!(dt > 0.0f)) return;
    for (const Entity target : Targets()) {
        Interactable* i = world_.GetComponent<Interactable>(target);
        if (i != nullptr && i->remaining > 0.0f) i->remaining = std::max(0.0f, i->remaining - dt);
    }
}

} // namespace aether::interact
