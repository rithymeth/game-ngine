// The Force Fields plugin's runtime module: registers its component, so scenes can name it, when the plugin is enabled.
#include "aether/forcefields/force_field.h"
#include "aether/plugin/plugin.h"

namespace {

class ForceFieldsModule : public aether::plugin::IModule {
public:
    void Startup(const aether::plugin::ModuleContext&) override {
        using namespace aether;
        ff::RegisterForceFieldComponents();
    }
};

} // namespace

AETHER_MODULE(ForceFields, ForceFieldsModule);
