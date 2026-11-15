// The Quests plugin's runtime module: registers its components, so scenes can
// name them, when the plugin is enabled.
#include "aether/quests/quest_system.h"
#include "aether/plugin/plugin.h"

namespace {

class QuestsModule : public aether::plugin::IModule {
public:
    void Startup(const aether::plugin::ModuleContext&) override {
        using namespace aether;
        quest::RegisterQuestComponents();
    }
};

} // namespace

AETHER_MODULE(Quests, QuestsModule);
