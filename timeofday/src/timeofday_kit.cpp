#include "aether/timeofday/timeofday_kit.h"

#include <utility>

namespace aether::tod {

namespace {

class TimeOfDayKit : public kit::IKit {
public:
    const char* Name() const override { return "TimeOfDay"; }
    std::vector<std::string> Deps() const override { return {}; }
    void InstallScriptApi(void* context) override {
        auto* api = static_cast<kit::KitScriptApiContext*>(context);
        if (api && api->install) api->install(Name());
    }
    std::vector<kit::KitStage> Stages() const override {
        // With the frame's Update, before the scripts that read the time run.
        return {{"Player.TimeOfDay", {"Player.Update"}}};
    }
};

} // namespace

SystemDesc MakeTimeOfDaySystem(WorldClock* clock) {
    SystemDesc desc;
    desc.name = "Player.TimeOfDay";
    desc.phase = SystemPhase::Update;
    desc.after = {"Player.Update"};
    desc.main_thread_only = true;
    desc.run = [clock](World&, const FrameContext& frame) {
        if (clock) clock->Advance(frame.dt);
    };
    return desc;
}

std::unique_ptr<kit::IKit> MakeTimeOfDayKit() { return std::make_unique<TimeOfDayKit>(); }

} // namespace aether::tod
