#include "aether/loot/loot_kit.h"

#include <utility>

namespace aether::loot {

namespace {

class LootKit : public kit::IKit {
public:
    LootKit(LootLibrary* tables, const inv::ItemLibrary* items) : tables_(tables), items_(items) {}
    const char* Name() const override { return "Loot"; }
    std::vector<std::string> Deps() const override { return {"Inventory"}; }
    void InstallScriptApi(void* context) override {
        auto* api = static_cast<kit::KitScriptApiContext*>(context);
        if (api && api->install) api->install(Name());
    }
    void LoadAssets(const kit::KitAssetContext& context) override {
        if (!tables_ || !items_) return;
        std::vector<std::string> paths;
        for (const kit::KitAsset& asset : context.assets) if (asset.importer == "LootTableDefinition") paths.push_back(asset.path);
        for (const LootAssetDiagnostic& diagnostic : LoadLootAssets(paths, context.read_content, *items_, *tables_)) {
            if (context.report) context.report({diagnostic.path, diagnostic.message, diagnostic.content_read_failure});
        }
    }
    std::unique_ptr<kit::IKitRuntime> CreateRuntime(const kit::KitRuntimeContext& context) override {
        auto* tables = context.find_service ? static_cast<LootLibrary*>(context.find_service("loot.tables")) : nullptr;
        // The Inventory kit's runtime is created first (dependencies come first) and made its system the active one.
        inv::InventorySystem* inventory = inv::InventorySystem::Active();
        if (!context.world || !tables || !inventory) return {};
        class Runtime final : public kit::IKitRuntime {
        public:
            Runtime(inv::InventorySystem& inventory, const LootLibrary& tables) : system(inventory, tables) {}
            void* Find(const char* name) override { return std::string(name) == "LootSystem" ? &system : nullptr; }
            LootSystem system;
        };
        return std::make_unique<Runtime>(*inventory, *tables);
    }
private:
    LootLibrary* tables_;
    const inv::ItemLibrary* items_;
};

} // namespace

std::vector<LootAssetDiagnostic> LoadLootAssets(
    const std::vector<std::string>& paths,
    std::function<bool(const std::string&, std::vector<u8>&, std::string*)> read_content,
    const inv::ItemLibrary& items, LootLibrary& tables) {
    std::vector<LootAssetDiagnostic> diagnostics;
    for (const std::string& path : paths) {
        std::vector<u8> bytes;
        std::string error;
        if (!read_content || !read_content(path, bytes, &error)) {
            diagnostics.push_back({path, std::move(error), true});
            continue;
        }
        LootTableDef table;
        const std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        if (!LootTableFromJson(text, table, &error) ||
            !(error = LootLibrary::CheckItems(table, items)).empty() ||
            !tables.Register(std::move(table), &error)) {
            diagnostics.push_back({path, std::move(error), false});
        }
    }
    return diagnostics;
}

std::unique_ptr<kit::IKit> MakeLootKit(LootLibrary* tables, const inv::ItemLibrary* items) { return std::make_unique<LootKit>(tables, items); }

} // namespace aether::loot
