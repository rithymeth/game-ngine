#include "aether/fire/fire_kit.h"

#include "aether/fire/fire.h"

#include <string>

namespace aether::fire {

namespace {

class FireKit : public kit::IKit {
public:
    const char* Name() const override { return "Fire"; }
    std::vector<std::string> Deps() const override { return {}; }
    void RegisterComponents() override { RegisterFireComponents(); }
    void InstallScriptApi(void* context) override {
        auto* api = static_cast<kit::KitScriptApiContext*>(context);
        if (api && api->install) api->install(Name());
    }
    std::vector<kit::KitStage> Stages() const override {
        // With the frame's Update, before the scripts that react to it run (and so before the particles are advanced).
        return {{"Player.Fire", {"Player.Update"}}};
    }
    std::unique_ptr<kit::IKitRuntime> CreateRuntime(const kit::KitRuntimeContext& context) override {
        if (!context.world) return {};
        class Runtime final : public kit::IKitRuntime {
        public:
            explicit Runtime(World& world) : system(world) {}
            void* Find(const char* name) override { return std::string(name) == "FireSystem" ? &system : nullptr; }
            FireSystem system;
        };
        return std::make_unique<Runtime>(*context.world);
    }
};

} // namespace

SystemDesc MakeFireSystem(FireSystem* system) {
    SystemDesc desc;
    desc.name = "Player.Fire";
    desc.phase = SystemPhase::Update;
    desc.after = {"Player.Update"};
    desc.main_thread_only = true;
    desc.run = [system](World&, const FrameContext& frame) {
        if (system) system->Advance(frame.dt);
    };
    return desc;
}

std::unique_ptr<kit::IKit> MakeFireKit() { return std::make_unique<FireKit>(); }

} // namespace aether::fire
