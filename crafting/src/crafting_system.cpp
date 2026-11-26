#include "aether/crafting/crafting_system.h"

namespace aether::craft {

namespace {
CraftingSystem* g_active = nullptr;
} // namespace

const char* ResultName(Result result) {
    switch (result) {
        case Result::Ok: return "ok";
        case Result::DeadEntity: return "dead_entity";
        case Result::UnknownRecipe: return "unknown_recipe";
        case Result::BadCount: return "bad_count";
        case Result::MissingInputs: return "missing_inputs";
        case Result::NoRoom: return "no_room";
    }
    return "unknown";
}

CraftingSystem::CraftingSystem(inv::InventorySystem& inventory, const RecipeLibrary& recipes) : inventory_(inventory), recipes_(recipes) { MakeActive(); }
CraftingSystem::~CraftingSystem() {
    if (g_active == this) g_active = nullptr;
}
CraftingSystem* CraftingSystem::Active() { return g_active; }
void CraftingSystem::MakeActive() { g_active = this; }

std::vector<Ingredient> CraftingSystem::Missing(Entity owner, const std::string& recipe, i32 times) const {
    std::vector<Ingredient> lacking;
    const RecipeDef* def = recipes_.Find(recipe);
    if (def == nullptr || times < 1 || times > kMaxTimes) return lacking;
    for (const Ingredient& input : def->inputs) {
        const i32 need = input.count * times;
        const i32 have = inventory_.Count(owner, input.item);
        if (have < need) lacking.push_back({input.item, need - have});
    }
    return lacking;
}

bool CraftingSystem::CanCraft(Entity owner, const std::string& recipe, i32 times) const {
    const RecipeDef* def = recipes_.Find(recipe);
    return def != nullptr && times >= 1 && times <= kMaxTimes && Missing(owner, recipe, times).empty();
}

Result CraftingSystem::Craft(Entity owner, const std::string& recipe, i32 times) {
    if (owner.IsNull()) return Result::DeadEntity;
    const RecipeDef* def = recipes_.Find(recipe);
    if (def == nullptr) return Result::UnknownRecipe;
    if (times < 1 || times > kMaxTimes) return Result::BadCount;
    if (!Missing(owner, recipe, times).empty()) return Result::MissingInputs;

    // Ingredients can already exceed today's weight limit, and adding them
    // back can fail or rearrange stacks. Snapshot the exact inventory instead.
    inv::Inventory* current = inventory_.InventoryOf(owner);
    if (!current) return Result::MissingInputs;
    inv::Inventory before = *current;
    const usize event_count = inventory_.events_.size();
    const auto rollback = [&](Result reason) {
        *current = std::move(before);
        inventory_.events_.resize(event_count);
        return reason;
    };

    // Remove/Add only edit this existing component and queue events; they do
    // not move the component or dispatch callbacks.
    for (const Ingredient& input : def->inputs) {
        if (!inventory_.Remove(owner, input.item, input.count * times))
            return rollback(Result::MissingInputs);
    }
    for (const Ingredient& output : def->outputs) {
        const i32 count = output.count * times;
        if (inventory_.Add(owner, output.item, count) < count)
            return rollback(Result::NoRoom);
    }
    return Result::Ok;
}

} // namespace aether::craft
