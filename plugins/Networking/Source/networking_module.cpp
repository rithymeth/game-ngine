// The Networking plugin's runtime module (Phase 26 step 1): registers its
// components, so scenes can name them, when the plugin is enabled.
#include "aether/net/prediction.h"
#include "aether/net/replication.h"
#include "aether/plugin/plugin.h"

namespace {

class NetworkingModule : public aether::plugin::IModule {
public:
    void Startup(const aether::plugin::ModuleContext&) override {
        using namespace aether;
        (void)GetComponentId<net::NetIdentity>();
        (void)GetComponentId<net::NetMovement>();
    }
};

} // namespace

AETHER_MODULE(Networking, NetworkingModule);
