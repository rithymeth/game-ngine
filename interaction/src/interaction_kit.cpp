#include "aether/interaction/interaction_kit.h"

#include "aether/interaction/interaction_system.h"

#include <utility>

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

SystemDesc MakeInteractionSystem(InteractionSystem* system, std::function<void(const InteractionEvent&)> on_event) {
    SystemDesc desc;
    desc.name = "Player.Interaction";
    desc.phase = SystemPhase::Update;
    desc.after = {"Player.Update", "Player.Sequencer", "Player.Effects"};
    desc.main_thread_only = true;
    desc.run = [system, on_event = std::move(on_event)](World&, const FrameContext& frame) {
        if (!system) return;
        system->Update(frame.dt);
        const std::vector<InteractionEvent> events = system->Events();
        system->ClearEvents();
        if (on_event) for (const InteractionEvent& event : events) on_event(event);
    };
    return desc;
}

std::unique_ptr<kit::IKit> MakeInteractionKit() { return std::make_unique<InteractionKit>(); }

} // namespace aether::interact
