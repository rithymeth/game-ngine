#include "aether/spawner/spawner_kit.h"

#include <utility>

namespace aether::spawn {

namespace {

class SpawnerKit : public kit::IKit {
public:
    const char* Name() const override { return "Spawner"; }
    std::vector<std::string> Deps() const override { return {}; }
    void RegisterComponents() override { RegisterSpawnerComponents(); }
    void InstallScriptApi(void* context) override {
        auto* api = static_cast<kit::KitScriptApiContext*>(context);
        if (api && api->install) api->install(Name());
    }
    std::vector<kit::KitStage> Stages() const override {
        // With the frame's Update, before the scripts that hear OnSpawned run (the scripting stage waits for every kit's).
        return {{"Player.Spawner", {"Player.Update"}}};
    }
    std::unique_ptr<kit::IKitRuntime> CreateRuntime(const kit::KitRuntimeContext& context) override {
        auto* spawn = context.find_service ? static_cast<SpawnFunction*>(context.find_service("spawner.spawn")) : nullptr;
        if (!context.world || !spawn) return {};
        class Runtime final : public kit::IKitRuntime {
        public:
            Runtime(World& world, SpawnFunction spawn) : system(world, std::move(spawn)) {}
            void* Find(const char* name) override { return std::string(name) == "SpawnerSystem" ? &system : nullptr; }
            SpawnerSystem system;
        };
        return std::make_unique<Runtime>(*context.world, *spawn);
    }
};

} // namespace

SystemDesc MakeSpawnerSystem(SpawnerSystem* system, std::function<void(const SpawnEvent&)> on_event) {
    SystemDesc desc;
    desc.name = "Player.Spawner";
    desc.phase = SystemPhase::Update;
    desc.after = {"Player.Update"};
    desc.main_thread_only = true;
    desc.run = [system, on_event = std::move(on_event)](World&, const FrameContext& frame) {
        if (!system) return;
        system->Update(frame.dt);
        const std::vector<SpawnEvent> events = system->Events();
        system->ClearEvents();
        if (on_event) for (const SpawnEvent& event : events) on_event(event);
    };
    return desc;
}

std::unique_ptr<kit::IKit> MakeSpawnerKit() { return std::make_unique<SpawnerKit>(); }

} // namespace aether::spawn
