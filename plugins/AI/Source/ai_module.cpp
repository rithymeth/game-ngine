// The AI plugin's runtime module (Phase 26 step 1): registers its
// components, so scenes can name them, when the plugin is enabled.
#include "aether/ai/bt_world.h"
#include "aether/ai/perception.h"
#include "aether/plugin/plugin.h"

namespace {

class AIModule : public aether::plugin::IModule {
public:
    void Startup(const aether::plugin::ModuleContext&) override {
        using namespace aether;
        RegisterBehaviorTreeComponents();
        RegisterPerceptionComponents();
    }
};

} // namespace

AETHER_MODULE(AI, AIModule);
