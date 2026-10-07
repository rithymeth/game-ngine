#include "aether/inventory/inventory_kit.h"

#include "aether/inventory/inventory_system.h"
#include "aether/inventory/item_def.h"

#include <utility>

namespace aether::inv {

namespace {

class InventoryKit : public kit::IKit {
public:
    const char* Name() const override { return "Inventory"; }
    std::vector<std::string> Deps() const override { return {"Gameplay"}; }
    void RegisterComponents() override { RegisterInventoryComponents(); }
    std::vector<kit::KitStage> Stages() const override {
        // Runs with the frame's Update, after effects have run, like the other gameplay kit systems.
        return {{"Player.Inventory", {"Player.Update", "Player.Sequencer", "Player.Effects"}}};
    }
};

} // namespace

SystemDesc MakeInventorySystem(InventorySystem* system, std::function<void(const ItemEvent&)> on_event) {
    SystemDesc desc;
    desc.name = "Player.Inventory";
    desc.phase = SystemPhase::Update;
    desc.after = {"Player.Update", "Player.Sequencer", "Player.Effects"};
    desc.main_thread_only = true;
    desc.run = [system, on_event = std::move(on_event)](World&, const FrameContext&) {
        if (!system) return;
        const std::vector<ItemEvent> events = system->Events();
        system->ClearEvents();
        if (on_event) for (const ItemEvent& event : events) on_event(event);
    };
    return desc;
}

std::vector<ItemAssetDiagnostic> LoadItemAssets(
    const std::vector<std::string>& paths,
    std::function<bool(const std::string&, std::vector<u8>&, std::string*)> read_content,
    const gas::EffectLibrary& effects, ItemLibrary& items) {
    std::vector<ItemAssetDiagnostic> diagnostics;
    for (const std::string& path : paths) {
        std::vector<u8> bytes;
        std::string error;
        if (!read_content || !read_content(path, bytes, &error)) {
            diagnostics.push_back({path, std::move(error), true});
            continue;
        }
        ItemDef item;
        const std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        if (!ItemFromJson(text, item, &error) ||
            !(error = ItemLibrary::CheckEffects(item, effects)).empty() ||
            !items.Register(std::move(item), &error)) {
            diagnostics.push_back({path, std::move(error), false});
        }
    }
    return diagnostics;
}

std::unique_ptr<kit::IKit> MakeInventoryKit() { return std::make_unique<InventoryKit>(); }

} // namespace aether::inv
