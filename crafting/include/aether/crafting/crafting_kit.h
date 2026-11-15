#pragma once

#include "aether/crafting/crafting_system.h"
#include "aether/kit/kit.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace aether::craft {

struct RecipeAssetDiagnostic {
    std::string path;
    std::string message;
    bool content_read_failure = false;
};

// Loads and validates recipe definitions against the item library (which the Inventory kit has filled first). The host supplies asset paths
// and content access; diagnostics keep package read failures apart from invalid recipes.
std::vector<RecipeAssetDiagnostic> LoadRecipeAssets(
    const std::vector<std::string>& paths,
    std::function<bool(const std::string&, std::vector<u8>&, std::string*)> read_content,
    const inv::ItemLibrary& items, RecipeLibrary& recipes);

// This kit's description for the player's kit registry: its name, what it needs (Inventory), the recipes it loads and the system it creates.
// It adds no scheduler stage: crafting happens when a script (or Blueprint) asks.
std::unique_ptr<kit::IKit> MakeCraftingKit(RecipeLibrary* recipes = nullptr, const inv::ItemLibrary* items = nullptr);

} // namespace aether::craft
