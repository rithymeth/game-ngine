#pragma once

#include "aether/crafting/recipe_def.h"
#include "aether/ecs/entity.h"
#include "aether/inventory/inventory_system.h"

#include <string>
#include <vector>

namespace aether::craft {

enum class Result : u8 { Ok, DeadEntity, UnknownRecipe, BadCount, MissingInputs, NoRoom };
// "ok", "dead_entity" (no entity was given), "unknown_recipe", "bad_count", "missing_inputs" (a dead entity has nothing, so it ends here), "no_room"
const char* ResultName(Result result);

// Crafts: takes a recipe's inputs out of an entity's inventory and puts its outputs in. All or nothing: if anything is missing, or the
// outputs do not fit (slots or weight), the inventory is left as it was. Deterministic and event-driven (no Update), on the inventory system
// it is given; the item events it causes (Removed, Added) are the inventory's own, so quests that count items hear a craft. One caveat of the
// all-or-nothing rule: a craft that has to be undone because the outputs did not fit also queues the undoing events (Added for the inputs,
// Removed for the outputs); they cancel out.
class CraftingSystem {
public:
    static constexpr i32 kMaxTimes = 1000;

    CraftingSystem(inv::InventorySystem& inventory, const RecipeLibrary& recipes);
    ~CraftingSystem();
    CraftingSystem(const CraftingSystem&) = delete;
    CraftingSystem& operator=(const CraftingSystem&) = delete;

    static CraftingSystem* Active();
    void MakeActive();

    // Whether the entity holds the inputs of `recipe`, `times` over (it does not look at room for the outputs: Craft says NoRoom).
    bool CanCraft(Entity owner, const std::string& recipe, i32 times = 1) const;
    // What the entity lacks: each input short of what `times` crafts need, with how many are missing. Empty when it has everything (or the
    // recipe is unknown).
    std::vector<Ingredient> Missing(Entity owner, const std::string& recipe, i32 times = 1) const;
    // Crafts `times` over (1 to kMaxTimes).
    Result Craft(Entity owner, const std::string& recipe, i32 times = 1);

    const RecipeLibrary& Recipes() const { return recipes_; }

private:
    inv::InventorySystem& inventory_;
    const RecipeLibrary& recipes_;
};

} // namespace aether::craft
