// The Ropes plugin's runtime module: registers its components, so scenes can name it, when the plugin is enabled.
#include "aether/ropes/rope.h"
#include "aether/plugin/plugin.h"

namespace {

class RopesModule : public aether::plugin::IModule {
public:
    void Startup(const aether::plugin::ModuleContext&) override {
        using namespace aether;
        ropes::RegisterRopeComponents();
    }
};

} // namespace

AETHER_MODULE(Ropes, RopesModule);
