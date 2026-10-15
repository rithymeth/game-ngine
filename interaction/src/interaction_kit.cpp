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
    void InstallScriptApi(void* context) override {
        auto* api = static_cast<kit::KitScriptApiContext*>(context);
        if (api && api->install) api->install(Name());
    }
    std::vector<kit::KitStage> Stages() const override {
        // Runs with the frame's Update, after effects have run, like the other gameplay kit systems.
        return {{"Player.Interaction", {"Player.Update", "Player.Sequencer", "Player.Effects"}}};
    }
    std::unique_ptr<kit::IKitRuntime> CreateRuntime(const kit::KitRuntimeContext& context) override {
        auto* effects = context.find_service ? static_cast<gas::EffectSystem*>(context.find_service("game.effects")) : nullptr;
        auto* abilities = context.find_service ? static_cast<gas::AbilitySystem*>(context.find_service("game.abilities")) : nullptr;
        if (!context.world) return {};
        class Runtime final : public kit::IKitRuntime {
        public:
            Runtime(World& world, gas::EffectSystem* effects, gas::AbilitySystem* abilities)
                : system(world, effects, abilities) {}
            void* Find(const char* name) override { return std::string(name) == "InteractionSystem" ? &system : nullptr; }
            InteractionSystem system;
        };
        return std::make_unique<Runtime>(*context.world, effects, abilities);
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
