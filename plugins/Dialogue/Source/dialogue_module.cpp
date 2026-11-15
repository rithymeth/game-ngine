// The Dialogue plugin's runtime module. The dialogue kit has no components of its own to register, so this module has nothing to start; the
// plugin gives the kit its place in the plugin set.
#include "aether/plugin/plugin.h"

namespace {

class DialogueModule : public aether::plugin::IModule {
public:
    void Startup(const aether::plugin::ModuleContext&) override {
        // The dialogue kit registers no components.
    }
};

} // namespace

AETHER_MODULE(Dialogue, DialogueModule);
