#include "aether/forcefields/forcefields_kit.h"

#include "aether/forcefields/force_field_system.h"

#include <string>

namespace aether::ff {

namespace {

class ForceFieldsKit : public kit::IKit {
public:
    const char* Name() const override { return "ForceFields"; }
    std::vector<std::string> Deps() const override { return {}; }
    void RegisterComponents() override { RegisterForceFieldComponents(); }
    void InstallScriptApi(void* context) override {
        auto* api = static_cast<kit::KitScriptApiContext*>(context);
        if (api && api->install) api->install(Name());
    }
};

} // namespace

std::unique_ptr<kit::IKit> MakeForceFieldsKit() { return std::make_unique<ForceFieldsKit>(); }

} // namespace aether::ff
