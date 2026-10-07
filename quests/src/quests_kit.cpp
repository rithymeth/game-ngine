#include "aether/quests/quests_kit.h"

#include "aether/quests/quest_system.h"
#include "aether/quests/quest_def.h"

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

std::vector<QuestAssetDiagnostic> LoadQuestAssets(
    const std::vector<std::string>& paths,
    std::function<bool(const std::string&, std::vector<u8>&, std::string*)> read_content,
    const gas::EffectLibrary& effects, QuestLibrary& quests) {
    std::vector<QuestAssetDiagnostic> diagnostics;
    for (const std::string& path : paths) {
        std::vector<u8> bytes;
        std::string error;
        if (!read_content || !read_content(path, bytes, &error)) {
            diagnostics.push_back({path, std::move(error), true});
            continue;
        }
        QuestDef quest;
        const std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        if (!QuestFromJson(text, quest, &error) || !quests.Register(std::move(quest), &error)) {
            diagnostics.push_back({path, std::move(error), false});
        }
    }
    for (std::string& problem : quests.Check(&effects)) {
        diagnostics.push_back({{}, std::move(problem), false});
    }
    return diagnostics;
}

std::unique_ptr<kit::IKit> MakeQuestsKit() { return std::make_unique<QuestsKit>(); }

} // namespace aether::quest
