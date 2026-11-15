#include "aether/checkpoints/checkpoints_kit.h"

#include <utility>

namespace aether::chk {

namespace {

class CheckpointsKit : public kit::IKit {
public:
    const char* Name() const override { return "Checkpoints"; }
    std::vector<std::string> Deps() const override { return {}; }
    void RegisterComponents() override { RegisterCheckpointComponents(); }
    void InstallScriptApi(void* context) override {
        auto* api = static_cast<kit::KitScriptApiContext*>(context);
        if (api && api->install) api->install(Name());
    }
    std::vector<kit::KitStage> Stages() const override {
        // After the frame's Update has moved the player, before the scripts that hear OnCheckpoint run.
        return {{"Player.Checkpoints", {"Player.Update"}}};
    }
    std::unique_ptr<kit::IKitRuntime> CreateRuntime(const kit::KitRuntimeContext& context) override {
        auto* bag = context.find_service ? static_cast<save::SaveBag*>(context.find_service("save.bag")) : nullptr;
        auto* scene = context.find_service ? static_cast<std::function<std::string()>*>(context.find_service("checkpoints.scene")) : nullptr;
        if (!context.world || !bag) return {};
        class Runtime final : public kit::IKitRuntime {
        public:
            Runtime(World& world, save::SaveBag& bag, std::function<std::string()> scene) : system(world, bag, std::move(scene)) {}
            void* Find(const char* name) override { return std::string(name) == "CheckpointSystem" ? &system : nullptr; }
            CheckpointSystem system;
        };
        return std::make_unique<Runtime>(*context.world, *bag, scene ? *scene : std::function<std::string()>{});
    }
};

} // namespace

SystemDesc MakeCheckpointsSystem(CheckpointSystem* system, std::function<void(const CheckpointEvent&)> on_event) {
    SystemDesc desc;
    desc.name = "Player.Checkpoints";
    desc.phase = SystemPhase::Update;
    desc.after = {"Player.Update"};
    desc.main_thread_only = true;
    desc.run = [system, on_event = std::move(on_event)](World&, const FrameContext&) {
        if (!system) return;
        system->Update();
        const std::vector<CheckpointEvent> events = system->Events();
        system->ClearEvents();
        if (on_event) for (const CheckpointEvent& event : events) on_event(event);
    };
    return desc;
}

std::unique_ptr<kit::IKit> MakeCheckpointsKit() { return std::make_unique<CheckpointsKit>(); }

} // namespace aether::chk
