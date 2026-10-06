#include "aether/quests/quests_kit.h"

#include "aether/quests/quest_system.h"

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

std::unique_ptr<kit::IKit> MakeQuestsKit() { return std::make_unique<QuestsKit>(); }

} // namespace aether::quest
