#pragma once

#include "aether/ecs/world.h"
#include "aether/gameplay/ability_system.h"
#include "aether/gameplay/attribute_set.h"
#include "aether/gameplay/effect_system.h"
#include "aether/gameplay/tag_container.h"

#include <string>
#include <vector>

namespace aether::editor {

// The rows the Gameplay Debugger shows (Phase 30 step 6, §30.7).
struct DebugAttributeRow {
    std::string name;
    f32 base = 0.0f, current = 0.0f, min = 0.0f, max = 0.0f;
    f32 add = 0.0f, mul = 1.0f;
    bool has_override = false;
    f32 override_value = 0.0f;
    bool Modified() const { return add != 0.0f || mul != 1.0f || has_override; }
};
struct DebugEffectRow {
    u32 handle = 0;
    std::string effect;
    bool known = false; // the library has its definition
    std::string policy; // "timed", "infinite" or "?" when unknown
    f32 remaining = 0.0f;
    i32 stacks = 1;
    f32 period_timer = 0.0f;
    Entity source;
};
struct DebugAbilityRow {
    std::string name;
    bool active = false;
    u32 handle = 0;
    f32 elapsed = 0.0f;
    bool committed = false;
};
struct DebugTagRow {
    std::string tag;
    i32 count = 0;
};
struct DebugEntityRows {
    Entity entity;
    std::string label; // "Entity 12"
    std::vector<DebugAttributeRow> attributes;
    std::vector<DebugEffectRow> effects;
    std::vector<DebugAbilityRow> abilities;
    std::vector<DebugTagRow> tags;
    bool Empty() const { return attributes.empty() && effects.empty() && abilities.empty() && tags.empty(); }
};

// What the Gameplay Debugger panel shows and does, without any ImGui: for each
// entity with gameplay data (or just the selected one), its attributes with
// their modifiers, active effects, granted and running abilities and tags,
// narrowed by a filter, in a deterministic order (entity index, then names;
// effects and abilities by handle). The actions go through the gameplay systems it is
// given (so events fire and modifiers follow), and edit the components
// directly without them. The document refers to a world it doesn't
// own: the host points it at the live world and clears it when that goes.
class GameplayDebuggerDocument {
public:
    void SetWorld(World* world) { world_ = world; }
    World* GetWorld() const { return world_; }
    // The libraries give effects and abilities their kind; optional.
    void SetLibraries(const gas::EffectLibrary* effects, const gas::AbilityLibrary* abilities) {
        effects_ = effects;
        abilities_ = abilities;
    }
    // The running systems the actions go through (so events fire and modifiers follow);
    // null ones make the action edit the components directly.
    void SetSystems(gas::AttributeSystem* attributes, gas::EffectSystem* effects, gas::AbilitySystem* abilities) {
        attribute_system_ = attributes;
        effect_system_ = effects;
        ability_system_ = abilities;
    }
    // Only this entity's rows (a null entity shows every entity with gameplay data).
    void Select(Entity entity) { selected_ = entity; }
    Entity Selected() const { return selected_; }
    // Case-insensitive substring; an entity with no matching row is left out.
    void SetFilter(std::string filter) { filter_ = std::move(filter); }
    const std::string& Filter() const { return filter_; }

    // Re-reads the world (never changes it). Rows past `kMaxEntities` are dropped.
    static constexpr usize kMaxEntities = 500;
    void Rebuild();
    const std::vector<DebugEntityRows>& Entities() const { return entities_; }
    usize TotalEntities() const { return total_; } // before the cap and the filter

    // Each returns what happened ("" never): a message the panel shows.
    std::string SetBase(Entity entity, const std::string& attribute, f32 value);
    std::string RemoveEffect(Entity entity, u32 handle);
    std::string CancelAbility(Entity entity, u32 handle);
    std::string AddTag(Entity entity, const std::string& tag);
    std::string RemoveTag(Entity entity, const std::string& tag);

private:
    bool Alive(Entity entity) const { return world_ != nullptr && world_->IsAlive(entity); }

    World* world_ = nullptr;
    const gas::EffectLibrary* effects_ = nullptr;
    const gas::AbilityLibrary* abilities_ = nullptr;
    gas::AttributeSystem* attribute_system_ = nullptr;
    gas::EffectSystem* effect_system_ = nullptr;
    gas::AbilitySystem* ability_system_ = nullptr;
    Entity selected_;
    std::string filter_;
    std::vector<DebugEntityRows> entities_;
    usize total_ = 0;
};

} // namespace aether::editor
