#include "aether/crafting/crafting_kit.h"

#include <utility>

namespace aether::craft {

namespace {

class CraftingKit : public kit::IKit {
public:
    CraftingKit(RecipeLibrary* recipes, const inv::ItemLibrary* items) : recipes_(recipes), items_(items) {}
    const char* Name() const override { return "Crafting"; }
    std::vector<std::string> Deps() const override { return {"Inventory"}; }
    void InstallScriptApi(void* context) override {
        auto* api = static_cast<kit::KitScriptApiContext*>(context);
        if (api && api->install) api->install(Name());
    }
    void LoadAssets(const kit::KitAssetContext& context) override {
        if (!recipes_ || !items_) return;
        std::vector<std::string> paths;
        for (const kit::KitAsset& asset : context.assets) if (asset.importer == "RecipeDefinition") paths.push_back(asset.path);
        for (const RecipeAssetDiagnostic& diagnostic : LoadRecipeAssets(paths, context.read_content, *items_, *recipes_)) {
            if (context.report) context.report({diagnostic.path, diagnostic.message, diagnostic.content_read_failure});
        }
    }
    std::unique_ptr<kit::IKitRuntime> CreateRuntime(const kit::KitRuntimeContext& context) override {
        auto* recipes = context.find_service ? static_cast<RecipeLibrary*>(context.find_service("crafting.recipes")) : nullptr;
        // The Inventory kit's runtime is created first (dependencies come first) and made its system the active one.
        inv::InventorySystem* inventory = inv::InventorySystem::Active();
        if (!context.world || !recipes || !inventory) return {};
        class Runtime final : public kit::IKitRuntime {
        public:
            Runtime(inv::InventorySystem& inventory, const RecipeLibrary& recipes) : system(inventory, recipes) {}
            void* Find(const char* name) override { return std::string(name) == "CraftingSystem" ? &system : nullptr; }
            CraftingSystem system;
        };
        return std::make_unique<Runtime>(*inventory, *recipes);
    }
private:
    RecipeLibrary* recipes_;
    const inv::ItemLibrary* items_;
};

} // namespace

std::vector<RecipeAssetDiagnostic> LoadRecipeAssets(
    const std::vector<std::string>& paths,
    std::function<bool(const std::string&, std::vector<u8>&, std::string*)> read_content,
    const inv::ItemLibrary& items, RecipeLibrary& recipes) {
    std::vector<RecipeAssetDiagnostic> diagnostics;
    for (const std::string& path : paths) {
        std::vector<u8> bytes;
        std::string error;
        if (!read_content || !read_content(path, bytes, &error)) {
            diagnostics.push_back({path, std::move(error), true});
            continue;
        }
        RecipeDef recipe;
        const std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        if (!RecipeFromJson(text, recipe, &error) ||
            !(error = RecipeLibrary::CheckItems(recipe, items)).empty() ||
            !recipes.Register(std::move(recipe), &error)) {
            diagnostics.push_back({path, std::move(error), false});
        }
    }
    return diagnostics;
}

std::unique_ptr<kit::IKit> MakeCraftingKit(RecipeLibrary* recipes, const inv::ItemLibrary* items) {
    return std::make_unique<CraftingKit>(recipes, items);
}

} // namespace aether::craft
