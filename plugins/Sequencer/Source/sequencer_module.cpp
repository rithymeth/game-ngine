// The Sequencer plugin's runtime module: registers its components, so scenes can
// name them, when the plugin is enabled.
#include "aether/sequencer/sequence_system.h"
#include "aether/plugin/plugin.h"

namespace {

class SequencerModule : public aether::plugin::IModule {
public:
    void Startup(const aether::plugin::ModuleContext&) override {
        using namespace aether;
        RegisterSequenceComponents();
    }
};

} // namespace

AETHER_MODULE(Sequencer, SequencerModule);
