#include "aether/gameplay/attribute_system.h"

#include "aether/ecs/component.h"
#include "aether/gameplay/ability_library.h"
#include "aether/gameplay/ability_system.h"
#include "aether/gameplay/attribute_library.h"
#include "aether/gameplay/effect_library.h"
#include "aether/gameplay/tag_library.h"
#include "aether/gameplay/effect_system.h"
#include "aether/gameplay/tag_container.h"

namespace aether::gas {

namespace {
AttributeSystem* g_active = nullptr;
}

// The Blueprint libraries register where their headers are included, and take their
// functions' addresses, so including them here is what links them in and makes them
// available to any program that uses the gameplay systems (the player).
void RegisterGameplayComponents() {
    (void)GetComponentId<AttributeSet>();
    (void)GetComponentId<TagContainer>();
    (void)GetComponentId<EffectContainer>();
    (void)GetComponentId<AbilityContainer>();
}

AttributeSystem::AttributeSystem(World& world) : world_(world) {
    RegisterGameplayComponents();
    MakeActive();
}

AttributeSystem::~AttributeSystem() {
    if (g_active == this) g_active = nullptr;
}

AttributeSystem* AttributeSystem::Active() { return g_active; }
void AttributeSystem::MakeActive() { g_active = this; }

AttributeSet* AttributeSystem::SetOf(Entity entity) const {
    if (entity.IsNull() || !world_.IsAlive(entity) || !world_.HasComponent<AttributeSet>(entity)) return nullptr;
    return world_.GetComponent<AttributeSet>(entity);
}

f32 AttributeSystem::Get(Entity entity, const std::string& name, f32 fallback) const {
    const AttributeSet* s = SetOf(entity);
    return s ? s->Get(name, fallback) : fallback;
}

bool AttributeSystem::Has(Entity entity, const std::string& name) const {
    const AttributeSet* s = SetOf(entity);
    return s && s->Has(name);
}

f32 AttributeSystem::GetBase(Entity entity, const std::string& name, f32 fallback) const {
    const AttributeSet* s = SetOf(entity);
    const Attribute* a = s ? s->Find(name) : nullptr;
    return a ? a->base : fallback;
}

f32 AttributeSystem::GetMin(Entity entity, const std::string& name, f32 fallback) const {
    const AttributeSet* s = SetOf(entity);
    const Attribute* a = s ? s->Find(name) : nullptr;
    return a ? a->min : fallback;
}

f32 AttributeSystem::GetMax(Entity entity, const std::string& name, f32 fallback) const {
    const AttributeSet* s = SetOf(entity);
    const Attribute* a = s ? s->Find(name) : nullptr;
    return a ? a->max : fallback;
}

template <typename Fn>
bool AttributeSystem::Mutate(Entity entity, const std::string& name, Fn&& change) {
    AttributeSet* s = SetOf(entity);
    if (s == nullptr) return false;
    const bool existed = s->Has(name);
    const f32 before = s->Get(name, 0.0f);
    if (!change(*s)) return false;
    const f32 after = s->Get(name, 0.0f);
    // A new attribute counts as a change from nothing (0), so a game that waits for Health to appear hears it.
    if (after != before || !existed) events_.push_back({entity, name, before, after});
    return true;
}

bool AttributeSystem::Define(Entity entity, const std::string& name, f32 base, f32 min, f32 max) {
    if (entity.IsNull() || !world_.IsAlive(entity)) return false;
    if (!world_.HasComponent<AttributeSet>(entity)) world_.AddComponent<AttributeSet>(entity, AttributeSet{});
    return Mutate(entity, name, [&](AttributeSet& s) { return s.Define(name, base, min, max); });
}

bool AttributeSystem::SetBase(Entity entity, const std::string& name, f32 base) {
    return Mutate(entity, name, [&](AttributeSet& s) { return s.SetBase(name, base); });
}

bool AttributeSystem::SetModifiers(Entity entity, const std::string& name, f32 add, f32 mul, bool has_override, f32 override_value) {
    return Mutate(entity, name, [&](AttributeSet& s) {
        Attribute* a = s.Find(name);
        if (a == nullptr) return false;
        a->add = add;
        a->mul = mul;
        a->has_override = has_override;
        a->override_value = override_value;
        AttributeSet::RecomputeCurrent(*a);
        return true;
    });
}

bool AttributeSystem::AddBase(Entity entity, const std::string& name, f32 delta) {
    return Mutate(entity, name, [&](AttributeSet& s) { return s.AddBase(name, delta); });
}

} // namespace aether::gas
