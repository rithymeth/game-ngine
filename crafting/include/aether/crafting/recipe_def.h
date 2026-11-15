#pragma once

#include "aether/core/base.h"
#include "aether/inventory/item_def.h"
#include "aether/reflection/reflection.h"

#include <map>
#include <string>
#include <vector>

namespace aether::craft {

// One line of a recipe: an item and how many.
struct Ingredient {
    std::string item;
    i32 count = 1;
    bool operator==(const Ingredient&) const = default;
};

// A recipe definition (`.arecipe`): what crafting takes and what it makes. Where it can be done (a workbench, a menu) is the game's
// business; the recipe only says what changes in an inventory.
struct RecipeDef {
    static constexpr i32 kMaxCount = 100000; // per line, so count x times always fits an i32

    std::string name;
    std::string title; // shown text (empty: the name)
    std::vector<Ingredient> inputs;  // taken
    std::vector<Ingredient> outputs; // given

    // Empty string if valid, else a named error ("recipe.name_empty", ...).
    std::string Validate() const;
    bool operator==(const RecipeDef&) const = default;
};

reflect::Json RecipeToJson(const RecipeDef& recipe);
// False (with `error`, "recipe.<code>: message") for bad JSON or an invalid recipe.
bool RecipeFromJson(const std::string& text, RecipeDef& out, std::string* error = nullptr);

// Recipes by name: filled from cooked `.arecipe` assets by the player, or by code.
class RecipeLibrary {
public:
    bool Register(RecipeDef recipe, std::string* error = nullptr);
    const RecipeDef* Find(const std::string& name) const;
    void Clear() { recipes_.clear(); }
    usize Size() const { return recipes_.size(); }
    std::vector<std::string> Names() const; // sorted

    // Every item a recipe names must be an item ("recipe.unknown_item"): the first problem of one recipe, or empty.
    static std::string CheckItems(const RecipeDef& recipe, const inv::ItemLibrary& items);
    // The same over the whole library: one message per problem; empty when all is well.
    std::vector<std::string> Check(const inv::ItemLibrary& items) const;

private:
    std::map<std::string, RecipeDef> recipes_;
};

} // namespace aether::craft
