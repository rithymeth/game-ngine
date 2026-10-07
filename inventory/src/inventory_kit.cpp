#include "aether/inventory/inventory_kit.h"

#include "aether/inventory/inventory_system.h"

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

std::unique_ptr<kit::IKit> MakeInventoryKit() { return std::make_unique<InventoryKit>(); }

} // namespace aether::inv
