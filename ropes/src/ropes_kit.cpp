#include "aether/ropes/ropes_kit.h"

#include "aether/ropes/rope.h"

#include <string>

namespace aether::ropes {

namespace {

class RopesKit : public kit::IKit {
public:
    const char* Name() const override { return "Ropes"; }
    std::vector<std::string> Deps() const override { return {}; }
    void RegisterComponents() override { RegisterRopeComponents(); }
    void InstallScriptApi(void* context) override {
        auto* api = static_cast<kit::KitScriptApiContext*>(context);
        if (api && api->install) api->install(Name());
    }
};

} // namespace

std::unique_ptr<kit::IKit> MakeRopesKit() { return std::make_unique<RopesKit>(); }

} // namespace aether::ropes
