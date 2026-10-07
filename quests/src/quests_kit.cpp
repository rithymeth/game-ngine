#include "aether/quests/quests_kit.h"

#include "aether/quests/quest_system.h"

#include <utility>

namespace aether::quest {

namespace {

class QuestsKit : public kit::IKit {
public:
    const char* Name() const override { return "Quests"; }
    std::vector<std::string> Deps() const override { return {"Gameplay"}; }
    void RegisterComponents() override { RegisterQuestComponents(); }
    std::vector<kit::KitStage> Stages() const override {
        // Runs with the frame's Update, after effects have run, like the other gameplay kit systems.
        return {{"Player.Quests", {"Player.Update", "Player.Sequencer", "Player.Effects"}}};
    }
};

} // namespace

SystemDesc MakeQuestsSystem(QuestSystem* system, std::function<void(QuestSystem&)> before_drain,
                            std::function<void(const QuestEvent&)> on_event) {
    SystemDesc desc;
    desc.name = "Player.Quests";
    desc.phase = SystemPhase::Update;
    desc.after = {"Player.Update", "Player.Sequencer", "Player.Effects"};
    desc.main_thread_only = true;
    desc.run = [system, before_drain = std::move(before_drain), on_event = std::move(on_event)](World&, const FrameContext&) {
        if (!system) return;
        if (before_drain) before_drain(*system);
        const std::vector<QuestEvent> events = system->Events();
        system->ClearEvents();
        if (on_event) for (const QuestEvent& event : events) on_event(event);
    };
    return desc;
}

std::unique_ptr<kit::IKit> MakeQuestsKit() { return std::make_unique<QuestsKit>(); }

} // namespace aether::quest
