// The TimeOfDay plugin's runtime module. The timeofday kit has no components of its own to register, so this module has nothing to start; the
// plugin gives the kit its place in the plugin set.
#include "aether/plugin/plugin.h"

namespace {

class TimeOfDayModule : public aether::plugin::IModule {
public:
    void Startup(const aether::plugin::ModuleContext&) override {
        // The timeofday kit registers no components.
    }
};

} // namespace

AETHER_MODULE(TimeOfDay, TimeOfDayModule);
