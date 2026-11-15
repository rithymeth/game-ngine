// The UI plugin's runtime module: registers its components, so scenes can
// name them, when the plugin is enabled.
#include "aether/ui/widget_system.h"
#include "aether/ui/world_ui.h"
#include "aether/plugin/plugin.h"

namespace {

class UIModule : public aether::plugin::IModule {
public:
    void Startup(const aether::plugin::ModuleContext&) override {
        using namespace aether;
        ui::RegisterWidgetComponents();
        ui::RegisterWorldWidgetComponents();
    }
};

} // namespace

AETHER_MODULE(UI, UIModule);
