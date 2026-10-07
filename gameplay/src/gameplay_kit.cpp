#include "aether/gameplay/gameplay_kit.h"

#include "aether/gameplay/attribute_system.h"

#include <utility>

namespace aether::gas {

namespace {

class GameplayKit : public kit::IKit {
public:
    const char* Name() const override { return "Gameplay"; }
    void RegisterComponents() override { RegisterGameplayComponents(); }
    std::vector<kit::KitStage> Stages() const override {
        return {
            {"Player.Attributes", {"Player.Update", "Player.Sequencer", "Player.Effects"}},
            {"Player.Abilities", {"Player.Update", "Player.Sequencer", "Player.Effects"}},
            // Keep Effects as the gameplay kit's dependency barrier: downstream kits
            // must wait for effects, but can still run before attribute notifications.
            {"Player.Effects", {"Player.Update", "Player.Sequencer"}},
        };
    }
};

} // namespace

std::unique_ptr<kit::IKit> MakeGameplayKit() { return std::make_unique<GameplayKit>(); }

SystemDesc MakeEffectSystem(EffectSystem* system, std::function<void(const EffectEvent&)> on_event) {
    SystemDesc desc;
    desc.name = "Player.Effects";
    desc.phase = SystemPhase::Update;
    desc.after = {"Player.Update", "Player.Sequencer"};
    desc.main_thread_only = true;
    desc.run = [system, on_event = std::move(on_event)](World&, const FrameContext& frame) {
        if (!system) return;
        system->Update(frame.dt);
        const std::vector<EffectEvent> events = system->Events();
        system->ClearEvents();
        if (on_event) for (const EffectEvent& event : events) on_event(event);
    };
    return desc;
}

SystemDesc MakeAbilitySystem(AbilitySystem* system, std::function<void(const AbilityEvent&)> on_event) {
    SystemDesc desc;
    desc.name = "Player.Abilities";
    desc.phase = SystemPhase::Update;
    desc.after = {"Player.Update", "Player.Sequencer", "Player.Effects"};
    desc.main_thread_only = true;
    desc.run = [system, on_event = std::move(on_event)](World&, const FrameContext& frame) {
        if (!system) return;
        system->Update(frame.dt);
        const std::vector<AbilityEvent> events = system->Events();
        system->ClearEvents();
        if (on_event) for (const AbilityEvent& event : events) on_event(event);
    };
    return desc;
}

SystemDesc MakeAttributeSystem(AttributeSystem* system, std::function<void(const AttributeEvent&)> on_event) {
    SystemDesc desc;
    desc.name = "Player.Attributes";
    desc.phase = SystemPhase::Update;
    desc.after = {"Player.Update", "Player.Sequencer", "Player.Effects"};
    desc.main_thread_only = true;
    desc.run = [system, on_event = std::move(on_event)](World&, const FrameContext&) {
        if (!system) return;
        const std::vector<AttributeEvent> events = system->Events();
        system->ClearEvents();
        if (on_event) for (const AttributeEvent& event : events) on_event(event);
    };
    return desc;
}

} // namespace aether::gas
