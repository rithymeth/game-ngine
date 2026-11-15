// The Checkpoints plugin's runtime module: registers its component, so scenes can name it, when the plugin is enabled.
#include "aether/checkpoints/checkpoint.h"
#include "aether/plugin/plugin.h"

namespace {

class CheckpointsModule : public aether::plugin::IModule {
public:
    void Startup(const aether::plugin::ModuleContext&) override {
        using namespace aether;
        chk::RegisterCheckpointComponents();
    }
};

} // namespace

AETHER_MODULE(Checkpoints, CheckpointsModule);
