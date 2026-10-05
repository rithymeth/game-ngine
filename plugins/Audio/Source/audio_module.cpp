// The Audio plugin's runtime module (Phase 26 step 1): registers its
// components, so scenes can name them, when the plugin is enabled.
#include "aether/audio/audio_system.h"
#include "aether/plugin/plugin.h"

namespace {

class AudioModule : public aether::plugin::IModule {
public:
    void Startup(const aether::plugin::ModuleContext&) override {
        using namespace aether;
        RegisterAudioComponents();
    }
};

} // namespace

AETHER_MODULE(Audio, AudioModule);
