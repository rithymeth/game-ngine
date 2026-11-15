// The Streaming plugin's runtime module: registers its components, so scenes can
// name them, when the plugin is enabled.
#include "aether/streaming/partition.h"
#include "aether/plugin/plugin.h"

namespace {

class StreamingModule : public aether::plugin::IModule {
public:
    void Startup(const aether::plugin::ModuleContext&) override {
        using namespace aether;
        RegisterStreamingComponents();
    }
};

} // namespace

AETHER_MODULE(Streaming, StreamingModule);
