// The Sprite2D plugin's runtime module: registers its components, so scenes can
// name them, when the plugin is enabled.
#include "aether/sprite2d/components.h"
#include "aether/sprite2d/physics2d.h"
#include "aether/sprite2d/platformer.h"
#include "aether/sprite2d/lights2d.h"
#include "aether/plugin/plugin.h"

namespace {

class Sprite2DModule : public aether::plugin::IModule {
public:
    void Startup(const aether::plugin::ModuleContext&) override {
        using namespace aether;
        sprite2d::RegisterSprite2DComponents();
        sprite2d::RegisterPhysics2DComponents();
        sprite2d::RegisterPlatformerComponents();
        sprite2d::RegisterLight2DComponents();
    }
};

} // namespace

AETHER_MODULE(Sprite2D, Sprite2DModule);
