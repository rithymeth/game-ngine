#include "aether/weather/weather_kit.h"

#include <utility>

namespace aether::weather {

namespace {

class WeatherKit : public kit::IKit {
public:
    const char* Name() const override { return "Weather"; }
    std::vector<std::string> Deps() const override { return {}; }
    void InstallScriptApi(void* context) override {
        auto* api = static_cast<kit::KitScriptApiContext*>(context);
        if (api && api->install) api->install(Name());
    }
    std::vector<kit::KitStage> Stages() const override {
        // With the frame's Update, before the scripts that read the weather run.
        return {{"Player.Weather", {"Player.Update"}}};
    }
};

} // namespace

SystemDesc MakeWeatherSystem(WeatherSystem* weather) {
    SystemDesc desc;
    desc.name = "Player.Weather";
    desc.phase = SystemPhase::Update;
    desc.after = {"Player.Update"};
    desc.main_thread_only = true;
    desc.run = [weather](World&, const FrameContext& frame) {
        if (weather) weather->Advance(frame.dt);
    };
    return desc;
}

std::unique_ptr<kit::IKit> MakeWeatherKit() { return std::make_unique<WeatherKit>(); }

} // namespace aether::weather
