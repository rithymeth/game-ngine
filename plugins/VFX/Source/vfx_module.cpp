// The VFX plugin's runtime module: registers its components, so scenes can
// name them, when the plugin is enabled.
#include "aether/vfx/particle_system.h"
#include "aether/plugin/plugin.h"

namespace {

class VFXModule : public aether::plugin::IModule {
public:
    void Startup(const aether::plugin::ModuleContext&) override {
        using namespace aether;
        RegisterParticleComponents();
    }
};

} // namespace

AETHER_MODULE(VFX, VFXModule);
