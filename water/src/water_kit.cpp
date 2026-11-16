#include "aether/water/water_kit.h"

#include "aether/water/water.h"

#include <string>

namespace aether::water {

namespace {

class WaterKit : public kit::IKit {
public:
    const char* Name() const override { return "Water"; }
    std::vector<std::string> Deps() const override { return {}; }
    void RegisterComponents() override { RegisterWaterComponents(); }
    void InstallScriptApi(void* context) override {
        auto* api = static_cast<kit::KitScriptApiContext*>(context);
        if (api && api->install) api->install(Name());
    }
};

} // namespace

std::unique_ptr<kit::IKit> MakeWaterKit() { return std::make_unique<WaterKit>(); }

} // namespace aether::water
