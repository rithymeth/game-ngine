#include "aether/gameplay/ability_system.h"

#include "aether/ecs/component.h"
#include "aether/gameplay/tag_container.h"

#include <algorithm>

namespace aether::gas {

namespace {
AbilitySystem* g_active = nullptr;
constexpr f32 kEps = 1e-5f;

TagContainer TagsOf(const GameplayAbility& def) {
    TagContainer c;
    for (const GameplayTag& t : def.tags) c.Add(t);
    return c;
}
} // namespace

AbilitySystem::AbilitySystem(World& world, AttributeSystem& attributes, EffectSystem& effects, const AbilityLibrary& abilities, const EffectLibrary& effect_library)
    : world_(world), attributes_(attributes), effects_(effects), abilities_(abilities), effect_library_(effect_library) {
    RegisterGameplayComponents();
    MakeActive();
}

AbilitySystem::~AbilitySystem() {
    if (g_active == this) g_active = nullptr;
}

AbilitySystem* AbilitySystem::Active() { return g_active; }
void AbilitySystem::MakeActive() { g_active = this; }

AbilityContainer* AbilitySystem::ContainerOf(Entity entity) const {
    if (entity.IsNull() || !world_.IsAlive(entity) || !world_.HasComponent<AbilityContainer>(entity)) return nullptr;
    return world_.GetComponent<AbilityContainer>(entity);
}

std::vector<Entity> AbilitySystem::Targets() const {
    std::vector<Entity> out;
    const ComponentId id = GetComponentId<AbilityContainer>();
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

bool AbilitySystem::IsGranted(Entity owner, const std::string& ability) const {
    const AbilityContainer* c = ContainerOf(owner);
    return c && std::binary_search(c->granted.begin(), c->granted.end(), ability);
}

bool AbilitySystem::IsActive(Entity owner, const std::string& ability) const {
    const AbilityContainer* c = ContainerOf(owner);
    return c && std::any_of(c->active.begin(), c->active.end(), [&](const ActiveAbility& a) { return a.ability == ability; });
}

i32 AbilitySystem::ActiveCount(Entity owner) const {
    const AbilityContainer* c = ContainerOf(owner);
    return c ? static_cast<i32>(c->active.size()) : 0;
}

bool AbilitySystem::Grant(Entity owner, const std::string& ability) {
    if (owner.IsNull() || !world_.IsAlive(owner) || abilities_.Find(ability) == nullptr) return false;
    if (!world_.HasComponent<AbilityContainer>(owner)) world_.AddComponent<AbilityContainer>(owner, AbilityContainer{});
    AbilityContainer* c = ContainerOf(owner);
    const auto it = std::lower_bound(c->granted.begin(), c->granted.end(), ability);
    if (it == c->granted.end() || *it != ability) c->granted.insert(it, ability);
    return true;
}

bool AbilitySystem::Revoke(Entity owner, const std::string& ability) {
    AbilityContainer* c = ContainerOf(owner);
    if (c == nullptr) return false;
    const auto it = std::lower_bound(c->granted.begin(), c->granted.end(), ability);
    if (it == c->granted.end() || *it != ability) return false;
    c->granted.erase(it);
    std::vector<AbilityHandle> running;
    for (const ActiveAbility& a : c->active) {
        if (a.ability == ability) running.push_back(a.handle);
    }
    for (const AbilityHandle h : running) Finish(h, true, false);
    return true;
}

bool AbilitySystem::Affordable(Entity owner, const GameplayAbility& def) const {
    if (def.cost.empty()) return true;
    const GameplayEffect* cost = effect_library_.Find(def.cost);
    if (cost == nullptr) return false;
    for (const GameplayEffect::Modifier& m : cost->modifiers) {
        if (!attributes_.Has(owner, m.attribute)) return false;
        // Spending is an Add below zero: it must leave the base within the attribute's range.
        if (m.op == GameplayEffect::Op::Add && m.magnitude < 0.0f && attributes_.GetBase(owner, m.attribute) + m.magnitude < attributes_.GetMin(owner, m.attribute) - kEps) return false;
    }
    return true;
}

FailReason AbilitySystem::CanActivate(Entity owner, const std::string& name) const {
    if (owner.IsNull() || !world_.IsAlive(owner)) return FailReason::DeadOwner;
    const GameplayAbility* def = abilities_.Find(name);
    if (def == nullptr) return FailReason::UnknownAbility;
    if (!IsGranted(owner, name)) return FailReason::NotGranted;
    if (IsActive(owner, name)) return FailReason::AlreadyActive;
    const TagContainer none;
    const TagContainer* tags = world_.HasComponent<TagContainer>(owner) ? world_.GetComponent<TagContainer>(owner) : &none;
    if (!def->activation_required.IsEmpty() && !def->activation_required.Matches(*tags)) return FailReason::MissingRequired;
    if (!def->activation_blocked.IsEmpty() && def->activation_blocked.Matches(*tags)) return FailReason::Blocked;
    if (!def->cooldown.empty()) {
        if (const GameplayEffect* cd = effect_library_.Find(def->cooldown)) {
            for (const GameplayTag& g : cd->granted_tags) {
                if (tags->HasTag(g)) return FailReason::Cooldown;
            }
        }
    }
    const AbilityContainer* c = ContainerOf(owner);
    const TagContainer mine = TagsOf(*def);
    for (const ActiveAbility& a : c->active) {
        const GameplayAbility* other = abilities_.Find(a.ability);
        if (other != nullptr && !other->block_abilities_with_tags.IsEmpty() && other->block_abilities_with_tags.Matches(mine)) return FailReason::BlockedByAbility;
    }
    if (!Affordable(owner, *def)) return FailReason::CannotAfford;
    return FailReason::None;
}

void AbilitySystem::Fail(Entity owner, const std::string& ability, FailReason reason) {
    AbilityEvent e;
    e.entity = owner;
    e.ability = ability;
    e.kind = AbilityEvent::Kind::Failed;
    e.reason = reason;
    events_.push_back(std::move(e));
}

AbilitySystem::ActivateResult AbilitySystem::TryActivate(Entity owner, const std::string& name) {
    const FailReason reason = CanActivate(owner, name);
    if (reason != FailReason::None) {
        Fail(owner, name, reason);
        return {0, reason};
    }
    const GameplayAbility& def = *abilities_.Find(name);
    // Abilities this one cancels go first, so their tags are gone before ours arrive.
    if (!def.cancel_abilities_with_tags.IsEmpty()) {
        std::vector<AbilityHandle> cancel;
        for (const ActiveAbility& a : ContainerOf(owner)->active) {
            const GameplayAbility* other = abilities_.Find(a.ability);
            if (other != nullptr && def.cancel_abilities_with_tags.Matches(TagsOf(*other))) cancel.push_back(a.handle);
        }
        for (const AbilityHandle h : cancel) Finish(h, true, false);
    }
    ActiveAbility active;
    active.handle = next_handle_++;
    active.ability = name;
    active.seq = next_seq_++;
    ContainerOf(owner)->active.push_back(active);
    handles_[active.handle] = owner;
    if (!def.activation_owned_tags.empty()) {
        if (!world_.HasComponent<TagContainer>(owner)) world_.AddComponent<TagContainer>(owner, TagContainer{});
        TagContainer* t = world_.GetComponent<TagContainer>(owner);
        for (const GameplayTag& g : def.activation_owned_tags) t->Add(g);
    }
    if (def.commit_on_activate && !Commit(active.handle)) {
        Finish(active.handle, true, false);
        return {0, FailReason::CannotAfford};
    }
    AbilityEvent e;
    e.entity = owner;
    e.ability = name;
    e.handle = active.handle;
    e.kind = AbilityEvent::Kind::Activated;
    events_.push_back(std::move(e));
    return {active.handle, FailReason::None};
}

bool AbilitySystem::Commit(AbilityHandle handle) {
    const auto it = handles_.find(handle);
    if (it == handles_.end()) return false;
    const Entity owner = it->second;
    AbilityContainer* c = ContainerOf(owner);
    if (c == nullptr) return false;
    for (ActiveAbility& a : c->active) {
        if (a.handle != handle) continue;
        if (a.committed) return true;
        const GameplayAbility* def = abilities_.Find(a.ability);
        if (def == nullptr) return false;
        if (!Affordable(owner, *def) || (!def->cost.empty() && effects_.Apply(owner, def->cost, owner).status == EffectSystem::Status::Rejected)) {
            Fail(owner, a.ability, FailReason::CannotAfford);
            return false;
        }
        if (!def->cooldown.empty()) effects_.Apply(owner, def->cooldown, owner);
        // Applying effects can add components to the owner, which moves this container: find it again.
        if (AbilityContainer* again = ContainerOf(owner)) {
            for (ActiveAbility& x : again->active) {
                if (x.handle == handle) x.committed = true;
            }
        }
        return true;
    }
    return false;
}

bool AbilitySystem::Finish(AbilityHandle handle, bool cancelled, bool timed_out) {
    const auto it = handles_.find(handle);
    if (it == handles_.end()) return false;
    const Entity owner = it->second;
    handles_.erase(it);
    AbilityContainer* c = ContainerOf(owner);
    if (c == nullptr) return false;
    for (usize i = 0; i < c->active.size(); ++i) {
        if (c->active[i].handle != handle) continue;
        const std::string name = c->active[i].ability;
        c->active.erase(c->active.begin() + static_cast<std::ptrdiff_t>(i));
        if (const GameplayAbility* def = abilities_.Find(name); def != nullptr && world_.HasComponent<TagContainer>(owner)) {
            TagContainer* t = world_.GetComponent<TagContainer>(owner);
            for (const GameplayTag& g : def->activation_owned_tags) t->Remove(g);
        }
        AbilityEvent e;
        e.entity = owner;
        e.ability = name;
        e.handle = handle;
        e.kind = cancelled ? AbilityEvent::Kind::Cancelled : AbilityEvent::Kind::Ended;
        e.timed_out = timed_out;
        events_.push_back(std::move(e));
        return true;
    }
    return false;
}

bool AbilitySystem::End(AbilityHandle handle) { return Finish(handle, false, false); }
bool AbilitySystem::Cancel(AbilityHandle handle) { return Finish(handle, true, false); }

i32 AbilitySystem::CancelByTag(Entity owner, const GameplayTag& tag) {
    const AbilityContainer* c = ContainerOf(owner);
    if (c == nullptr || !tag.IsValid()) return 0;
    std::vector<AbilityHandle> cancel;
    for (const ActiveAbility& a : c->active) {
        const GameplayAbility* def = abilities_.Find(a.ability);
        if (def != nullptr && std::any_of(def->tags.begin(), def->tags.end(), [&](const GameplayTag& t) { return t.Matches(tag); })) cancel.push_back(a.handle);
    }
    for (const AbilityHandle h : cancel) Finish(h, true, false);
    return static_cast<i32>(cancel.size());
}

void AbilitySystem::Update(f32 dt) {
    if (!(dt > 0.0f)) return;
    for (const Entity owner : Targets()) {
        AbilityContainer* c = ContainerOf(owner);
        if (c == nullptr) continue;
        std::vector<AbilityHandle> expired;
        for (ActiveAbility& a : c->active) {
            a.elapsed += dt;
            const GameplayAbility* def = abilities_.Find(a.ability);
            if (def != nullptr && def->max_duration > 0.0f && a.elapsed >= def->max_duration - kEps) expired.push_back(a.handle);
        }
        for (const AbilityHandle h : expired) Finish(h, false, true);
    }
}

void AbilitySystem::Rebuild() {
    handles_.clear();
    AbilityHandle max_handle = 0;
    u32 max_seq = 0;
    for (const Entity owner : Targets()) {
        for (const ActiveAbility& a : ContainerOf(owner)->active) {
            handles_[a.handle] = owner;
            max_handle = std::max(max_handle, a.handle);
            max_seq = std::max(max_seq, a.seq);
        }
    }
    next_handle_ = std::max(next_handle_, max_handle + 1);
    next_seq_ = std::max(next_seq_, max_seq + 1);
}

} // namespace aether::gas
