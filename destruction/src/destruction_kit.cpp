#include "aether/destruction/destruction_kit.h"

#include "aether/destruction/destructible.h"

#include <string>

namespace aether::destruction {

namespace {

class DestructionKit : public kit::IKit {
public:
    const char* Name() const override { return "Destruction"; }
    std::vector<std::string> Deps() const override { return {}; }
    void RegisterComponents() override { RegisterDestructionComponents(); }
    void InstallScriptApi(void* context) override {
        auto* api = static_cast<kit::KitScriptApiContext*>(context);
        if (api && api->install) api->install(Name());
    }
};

} // namespace

std::unique_ptr<kit::IKit> MakeDestructionKit() { return std::make_unique<DestructionKit>(); }

} // namespace aether::destruction
