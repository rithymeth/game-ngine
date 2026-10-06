#include "aether/inventory/inventory_kit.h"

#include "aether/inventory/inventory_system.h"

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

std::unique_ptr<kit::IKit> MakeInventoryKit() { return std::make_unique<InventoryKit>(); }

} // namespace aether::inv
