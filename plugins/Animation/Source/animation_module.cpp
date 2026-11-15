// The Animation plugin's runtime module: registers its components, so scenes can
// name them, when the plugin is enabled.
#include "aether/animation/animator.h"
#include "aether/plugin/plugin.h"

namespace {

class AnimationModule : public aether::plugin::IModule {
public:
    void Startup(const aether::plugin::ModuleContext&) override {
        using namespace aether;
        RegisterAnimationComponents();
    }
};

} // namespace

AETHER_MODULE(Animation, AnimationModule);
