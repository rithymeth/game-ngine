#include "gameplay/gameplay_debugger_document.h"

#include "aether/ecs/component.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace aether::editor {

using namespace gas;

namespace {

std::string Lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool Has(const std::string& text, const std::string& needle_lower) { return Lower(text).find(needle_lower) != std::string::npos; }

bool EntityLess(Entity a, Entity b) { return a.index != b.index ? a.index < b.index : a.generation < b.generation; }

} // namespace

void GameplayDebuggerDocument::Rebuild() {
    entities_.clear();
    total_ = 0;
    if (world_ == nullptr) return;
    std::vector<Entity> found;
    const auto collect = [&](ComponentId id) {
        world_->ForEachArchetype([&](const Archetype& a) {
            if (!a.Mask().test(id)) return;
            for (usize c = 0; c < a.ChunkCount(); ++c) {
                const Entity* chunk = a.EntityArray(c);
                found.insert(found.end(), chunk, chunk + a.ChunkEntityCount(c));
            }
        });
    };
    collect(GetComponentId<AttributeSet>());
    collect(GetComponentId<EffectContainer>());
    collect(GetComponentId<AbilityContainer>());
    collect(GetComponentId<TagContainer>());
    std::sort(found.begin(), found.end(), EntityLess);
    found.erase(std::unique(found.begin(), found.end()), found.end());
    if (!selected_.IsNull()) {
        found.erase(std::remove_if(found.begin(), found.end(), [&](Entity e) { return e != selected_; }), found.end());
    }
    total_ = found.size();
    const std::string filter = Lower(filter_);
    for (const Entity e : found) {
        if (entities_.size() >= kMaxEntities) break;
        DebugEntityRows rows;
        rows.entity = e;
        rows.label = "Entity " + std::to_string(e.index);
        const bool entity_matches = !filter.empty() && Has(rows.label, filter);
        const auto keep = [&](const std::string& name) { return filter.empty() || entity_matches || Has(name, filter); };
        if (const AttributeSet* set = world_->GetComponent<AttributeSet>(e)) {
            for (const Attribute& a : set->attributes) {
                if (!keep(a.name)) continue;
                rows.attributes.push_back({a.name, a.base, a.current, a.min, a.max, a.add, a.mul, a.has_override, a.override_value});
            }
        }
        if (const EffectContainer* c = world_->GetComponent<EffectContainer>(e)) {
            std::vector<ActiveEffect> sorted = c->active;
            std::sort(sorted.begin(), sorted.end(), [](const ActiveEffect& x, const ActiveEffect& y) { return x.handle < y.handle; });
            for (const ActiveEffect& a : sorted) {
                if (!keep(a.effect)) continue;
                DebugEffectRow row;
                row.handle = a.handle;
                row.effect = a.effect;
                const GameplayEffect* def = effects_ ? effects_->Find(a.effect) : nullptr;
                row.known = def != nullptr;
                row.policy = def == nullptr ? "?" : def->duration_policy == GameplayEffect::Duration::Infinite ? "infinite" : "timed";
                row.remaining = a.remaining;
                row.stacks = a.stacks;
                row.period_timer = a.period_timer;
                row.source = a.source;
                rows.effects.push_back(std::move(row));
            }
        }
        if (const AbilityContainer* c = world_->GetComponent<AbilityContainer>(e)) {
            for (const std::string& name : c->granted) {
                const ActiveAbility* running = nullptr;
                for (const ActiveAbility& a : c->active) {
                    if (a.ability == name) running = &a;
                }
                if (!keep(name)) continue;
                DebugAbilityRow row;
                row.name = name;
                if (running != nullptr) {
                    row.active = true;
                    row.handle = running->handle;
                    row.elapsed = running->elapsed;
                    row.committed = running->committed;
                }
                rows.abilities.push_back(std::move(row));
            }
        }
        if (const TagContainer* t = world_->GetComponent<TagContainer>(e)) {
            for (const TagCount& tc : t->tags) {
                if (keep(tc.tag)) rows.tags.push_back({tc.tag, tc.count});
            }
        }
        if (!rows.Empty()) entities_.push_back(std::move(rows));
    }
}

std::string GameplayDebuggerDocument::SetBase(Entity entity, const std::string& attribute, f32 value) {
    if (!Alive(entity)) return "No such entity.";
    if (std::isnan(value)) return "That isn't a number.";
    if (AttributeSystem* sys = attribute_system_) {
        if (!sys->Has(entity, attribute)) return "The entity has no attribute '" + attribute + "'.";
        sys->SetBase(entity, attribute, value);
        return "Set " + attribute + " to " + std::to_string(sys->GetBase(entity, attribute));
    }
    AttributeSet* set = world_->GetComponent<AttributeSet>(entity);
    if (set == nullptr || !set->Has(attribute)) return "The entity has no attribute '" + attribute + "'.";
    set->SetBase(attribute, value);
    return "Set " + attribute + " to " + std::to_string(set->Find(attribute)->base);
}

std::string GameplayDebuggerDocument::RemoveEffect(Entity entity, u32 handle) {
    if (!Alive(entity)) return "No such entity.";
    if (EffectSystem* sys = effect_system_) {
        return sys->Remove(handle) ? "Removed the effect." : "No such effect.";
    }
    EffectContainer* c = world_->GetComponent<EffectContainer>(entity);
    if (c == nullptr) return "No such effect.";
    const auto it = std::find_if(c->active.begin(), c->active.end(), [&](const ActiveEffect& a) { return a.handle == handle; });
    if (it == c->active.end()) return "No such effect.";
    c->active.erase(it); // no system to release its tags and modifiers: the next run rebuilds them
    return "Removed the effect (no running system: its modifiers go on the next play).";
}

std::string GameplayDebuggerDocument::CancelAbility(Entity entity, u32 handle) {
    if (!Alive(entity)) return "No such entity.";
    if (AbilitySystem* sys = ability_system_) {
        return sys->Cancel(handle) ? "Cancelled the ability." : "No such running ability.";
    }
    AbilityContainer* c = world_->GetComponent<AbilityContainer>(entity);
    if (c == nullptr) return "No such running ability.";
    const auto it = std::find_if(c->active.begin(), c->active.end(), [&](const ActiveAbility& a) { return a.handle == handle; });
    if (it == c->active.end()) return "No such running ability.";
    c->active.erase(it);
    return "Cancelled the ability (no running system: its tags stay until they are removed).";
}

std::string GameplayDebuggerDocument::AddTag(Entity entity, const std::string& tag) {
    if (!Alive(entity)) return "No such entity.";
    const GameplayTag t = GameplayTag::Make(tag);
    if (!t.IsValid()) return "'" + tag + "' isn't a valid tag name.";
    if (!world_->HasComponent<TagContainer>(entity)) world_->AddComponent<TagContainer>(entity, TagContainer{});
    world_->GetComponent<TagContainer>(entity)->Add(t);
    return "Added " + tag + ".";
}

std::string GameplayDebuggerDocument::RemoveTag(Entity entity, const std::string& tag) {
    if (!Alive(entity)) return "No such entity.";
    TagContainer* c = world_->GetComponent<TagContainer>(entity);
    const GameplayTag t = GameplayTag::Make(tag);
    if (c == nullptr || !t.IsValid() || !c->Remove(t)) return "The entity doesn't have the tag '" + tag + "'.";
    return "Removed one " + tag + ".";
}

} // namespace aether::editor
