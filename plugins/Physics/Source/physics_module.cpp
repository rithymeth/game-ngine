// The Physics plugin's runtime module (Phase 26 step 1): registers its
// components, so scenes can name them, when the plugin is enabled.
#include "aether/physics/components.h"
#include "aether/plugin/plugin.h"

namespace {

class PhysicsModule : public aether::plugin::IModule {
public:
    void Startup(const aether::plugin::ModuleContext&) override {
        using namespace aether;
        (void)GetComponentId<RigidBody>();
        (void)GetComponentId<BoxCollider>();
        (void)GetComponentId<SphereCollider>();
        (void)GetComponentId<CapsuleCollider>();
        (void)GetComponentId<ConvexCollider>();
        (void)GetComponentId<MeshCollider>();
        RegisterPhysicsComponentSerializers();
    }
};

} // namespace

AETHER_MODULE(Physics, PhysicsModule);
