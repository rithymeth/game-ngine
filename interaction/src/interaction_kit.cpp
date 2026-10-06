#include "aether/interaction/interaction_kit.h"

#include "aether/interaction/interaction_system.h"

namespace aether::interact {

namespace {

class InteractionKit : public kit::IKit {
public:
    const char* Name() const override { return "Interaction"; }
    std::vector<std::string> Deps() const override { return {"Gameplay"}; }
    void RegisterComponents() override { RegisterInteractionComponents(); }
    std::vector<kit::KitStage> Stages() const override {
        // Runs with the frame's Update, after effects have run, like the other gameplay kit systems.
        return {{"Player.Interaction", {"Player.Update", "Player.Sequencer", "Player.Effects"}}};
    }
};

} // namespace

std::unique_ptr<kit::IKit> MakeInteractionKit() { return std::make_unique<InteractionKit>(); }

} // namespace aether::interact
