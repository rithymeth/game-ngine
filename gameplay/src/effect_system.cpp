#include "aether/gameplay/effect_system.h"

#include "aether/ecs/component.h"
#include "aether/gameplay/tag_container.h"

#include <algorithm>
#include <cmath>

namespace aether::gas {

namespace {
EffectSystem* g_active = nullptr;
constexpr f32 kEps = 1e-5f;

struct Agg {
    f32 add = 0.0f;
    f32 mul = 1.0f;
    bool has_override = false;
    f32 override_value = 0.0f;
    u32 override_seq = 0;
};
} // namespace

EffectSystem::EffectSystem(World& world, AttributeSystem& attributes, const EffectLibrary& library)
    : world_(world), attributes_(attributes), library_(library) {
    RegisterGameplayComponents();
    MakeActive();
}

EffectSystem::~EffectSystem() {
    if (g_active == this) g_active = nullptr;
}

EffectSystem* EffectSystem::Active() { return g_active; }
void EffectSystem::MakeActive() { g_active = this; }

EffectContainer* EffectSystem::ContainerOf(Entity entity) const {
    if (entity.IsNull() || !world_.IsAlive(entity) || !world_.HasComponent<EffectContainer>(entity)) return nullptr;
    return world_.GetComponent<EffectContainer>(entity);
}

std::vector<Entity> EffectSystem::Targets() const {
    std::vector<Entity> out;
    const ComponentId id = GetComponentId<EffectContainer>();
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

bool EffectSystem::HasEffect(Entity target, const std::string& effect) const {
    const EffectContainer* c = ContainerOf(target);
    if (c == nullptr) return false;
    return std::any_of(c->active.begin(), c->active.end(), [&](const ActiveEffect& e) { return e.effect == effect; });
}

i32 EffectSystem::ActiveCount(Entity target) const {
    const EffectContainer* c = ContainerOf(target);
    return c ? static_cast<i32>(c->active.size()) : 0;
}

void EffectSystem::Recompute(Entity target) {
    const EffectContainer* c = ContainerOf(target);
    if (c == nullptr || !world_.HasComponent<AttributeSet>(target)) return;
    std::map<std::string, Agg> agg;
    for (const ActiveEffect& e : c->active) {
        const GameplayEffect* def = library_.Find(e.effect);
        if (def == nullptr || def->period > 0.0f) continue; // periodic effects change the base instead
        for (const GameplayEffect::Modifier& m : def->modifiers) {
            Agg& a = agg[m.attribute];
            switch (m.op) {
            case GameplayEffect::Op::Add: a.add += m.magnitude * static_cast<f32>(e.stacks); break;
            case GameplayEffect::Op::Multiply: a.mul *= std::pow(m.magnitude, static_cast<f32>(e.stacks)); break;
            case GameplayEffect::Op::Override:
                if (!a.has_override || e.seq >= a.override_seq) a.has_override = true, a.override_value = m.magnitude, a.override_seq = e.seq;
                break;
            }
        }
    }
    // Every attribute is refreshed, so one that lost its last modifier goes back to its base.
    const std::vector<Attribute> attrs = world_.GetComponent<AttributeSet>(target)->attributes;
    for (const Attribute& attr : attrs) {
        const auto it = agg.find(attr.name);
        const Agg a = it == agg.end() ? Agg{} : it->second;
        attributes_.SetModifiers(target, attr.name, a.add, a.mul, a.has_override, a.override_value);
    }
}

void EffectSystem::ApplyInstant(Entity target, const GameplayEffect& effect, i32 stacks) {
    for (const GameplayEffect::Modifier& m : effect.modifiers) {
        const f32 base = attributes_.GetBase(target, m.attribute);
        switch (m.op) {
        case GameplayEffect::Op::Add: attributes_.SetBase(target, m.attribute, base + m.magnitude * static_cast<f32>(stacks)); break;
        case GameplayEffect::Op::Multiply: attributes_.SetBase(target, m.attribute, base * std::pow(m.magnitude, static_cast<f32>(stacks))); break;
        case GameplayEffect::Op::Override: attributes_.SetBase(target, m.attribute, m.magnitude); break;
        }
    }
}

EffectSystem::ApplyResult EffectSystem::Apply(Entity target, const std::string& name, Entity source) {
    if (target.IsNull() || !world_.IsAlive(target)) return {};
    const GameplayEffect* def = library_.Find(name);
    if (def == nullptr) return {};
    for (const GameplayEffect::Modifier& m : def->modifiers) {
        if (!attributes_.Has(target, m.attribute)) return {};
    }
    const TagContainer none;
    const TagContainer* tags = world_.HasComponent<TagContainer>(target) ? world_.GetComponent<TagContainer>(target) : &none;
    if (!def->require.IsEmpty() && !def->require.Matches(*tags)) return {};
    if (!def->blocked.IsEmpty() && def->blocked.Matches(*tags)) return {};

    if (def->duration_policy == GameplayEffect::Duration::Instant) {
        ApplyInstant(target, *def, 1);
        return {Status::Instant, 0};
    }

    EffectContainer* container = ContainerOf(target);
    if (container != nullptr) {
        for (ActiveEffect& e : container->active) {
            if (e.effect != name || (def->per_source && e.source != source)) continue;
            if (def->stacking == GameplayEffect::Stacking::None) return {};
            e.remaining = def->duration;
            if (def->stacking == GameplayEffect::Stacking::StackCount) e.stacks = std::min(e.stacks + 1, def->max_stacks);
            const EffectHandle h = e.handle;
            Recompute(target);
            return {Status::Applied, h};
        }
    } else {
        world_.AddComponent<EffectContainer>(target, EffectContainer{});
        container = ContainerOf(target);
    }

    ActiveEffect active;
    active.handle = next_handle_++;
    active.effect = name;
    active.source = source;
    active.remaining = def->duration;
    active.seq = next_seq_++;
    container->active.push_back(active);
    handles_[active.handle] = target;
    if (!def->granted_tags.empty()) {
        if (!world_.HasComponent<TagContainer>(target)) world_.AddComponent<TagContainer>(target, TagContainer{});
        TagContainer* t = world_.GetComponent<TagContainer>(target);
        for (const GameplayTag& g : def->granted_tags) t->Add(g);
    }
    Recompute(target);
    events_.push_back({target, name, active.handle, true});
    if (def->period > 0.0f && def->execute_on_apply) ApplyInstant(target, *def, 1);
    return {Status::Applied, active.handle};
}

void EffectSystem::RemoveAt(Entity target, usize index) {
    EffectContainer* c = ContainerOf(target);
    if (c == nullptr || index >= c->active.size()) return;
    const ActiveEffect gone = c->active[index];
    c->active.erase(c->active.begin() + static_cast<std::ptrdiff_t>(index));
    handles_.erase(gone.handle);
    if (const GameplayEffect* def = library_.Find(gone.effect); def != nullptr && world_.HasComponent<TagContainer>(target)) {
        TagContainer* t = world_.GetComponent<TagContainer>(target);
        for (const GameplayTag& g : def->granted_tags) t->Remove(g);
    }
    Recompute(target);
    events_.push_back({target, gone.effect, gone.handle, false});
}

bool EffectSystem::Remove(EffectHandle handle) {
    const auto it = handles_.find(handle);
    if (it == handles_.end()) return false;
    const Entity target = it->second;
    EffectContainer* c = ContainerOf(target);
    if (c == nullptr) {
        handles_.erase(it);
        return false;
    }
    for (usize i = 0; i < c->active.size(); ++i) {
        if (c->active[i].handle == handle) {
            RemoveAt(target, i);
            return true;
        }
    }
    handles_.erase(handle);
    return false;
}

i32 EffectSystem::RemoveByTag(Entity target, const GameplayTag& tag) {
    EffectContainer* c = ContainerOf(target);
    if (c == nullptr || !tag.IsValid()) return 0;
    i32 removed = 0;
    for (usize i = 0; i < c->active.size();) {
        const GameplayEffect* def = library_.Find(c->active[i].effect);
        const bool grants = def != nullptr && std::any_of(def->granted_tags.begin(), def->granted_tags.end(), [&](const GameplayTag& g) { return g.Matches(tag); });
        if (grants) {
            RemoveAt(target, i);
            ++removed;
        } else {
            ++i;
        }
    }
    return removed;
}

void EffectSystem::Update(f32 dt) {
    if (!(dt > 0.0f)) return;
    for (const Entity target : Targets()) {
        EffectContainer* c = ContainerOf(target);
        if (c == nullptr) continue;
        std::vector<EffectHandle> done;
        const std::vector<ActiveEffect> snapshot = c->active; // ticking changes attributes, never the list
        for (const ActiveEffect& snap : snapshot) {
            const GameplayEffect* def = library_.Find(snap.effect);
            if (def == nullptr) {
                done.push_back(snap.handle);
                continue;
            }
            ActiveEffect* e = nullptr;
            for (ActiveEffect& a : c->active) {
                if (a.handle == snap.handle) e = &a;
            }
            if (e == nullptr) continue;
            if (def->period > 0.0f) {
                e->period_timer += dt;
                while (e->period_timer >= def->period - kEps) {
                    ApplyInstant(target, *def, e->stacks);
                    e->period_timer -= def->period;
                }
            }
            if (def->duration_policy == GameplayEffect::Duration::Timed) {
                e->remaining -= dt;
                if (e->remaining <= kEps) done.push_back(e->handle);
            }
        }
        // Effects that remove themselves when the target gains certain tags.
        if (world_.HasComponent<TagContainer>(target)) {
            const TagContainer& t = *world_.GetComponent<TagContainer>(target);
            for (const ActiveEffect& a : c->active) {
                const GameplayEffect* def = library_.Find(a.effect);
                if (def != nullptr && !def->remove_on_tags.empty() && t.HasAny(def->remove_on_tags)) done.push_back(a.handle);
            }
        }
        for (const EffectHandle h : done) Remove(h);
    }
}

void EffectSystem::Rebuild() {
    handles_.clear();
    EffectHandle max_handle = 0;
    u32 max_seq = 0;
    for (const Entity target : Targets()) {
        const EffectContainer* c = ContainerOf(target);
        for (const ActiveEffect& e : c->active) {
            handles_[e.handle] = target;
            max_handle = std::max(max_handle, e.handle);
            max_seq = std::max(max_seq, e.seq);
        }
        Recompute(target);
    }
    next_handle_ = std::max(next_handle_, max_handle + 1);
    next_seq_ = std::max(next_seq_, max_seq + 1);
}

} // namespace aether::gas
