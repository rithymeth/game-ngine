// The Renderer plugin's runtime module: registers its components, so scenes can
// name them, when the plugin is enabled.
#include "aether/renderer/components.h"
#include "aether/plugin/plugin.h"

namespace {

class RendererModule : public aether::plugin::IModule {
public:
    void Startup(const aether::plugin::ModuleContext&) override {
        using namespace aether;
        RegisterRenderComponents();
    }
};

} // namespace

AETHER_MODULE(Renderer, RendererModule);
