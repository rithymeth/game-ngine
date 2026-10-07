#include "aether/gameplay/gameplay_kit.h"

#include "aether/gameplay/attribute_system.h"

#include <utility>

namespace aether::gas {

namespace {

class GameplayKit : public kit::IKit {
public:
    GameplayKit(EffectLibrary* effects, AbilityLibrary* abilities) : effects_(effects), abilities_(abilities) {}
    const char* Name() const override { return "Gameplay"; }
    void RegisterComponents() override { RegisterGameplayComponents(); }
    void InstallScriptApi(void* context) override {
        auto* api = static_cast<kit::KitScriptApiContext*>(context);
        if (api && api->install) api->install(Name());
    }
    void LoadAssets(const kit::KitAssetContext& context) override {
        if (effects_) {
            std::vector<std::string> paths;
            for (const kit::KitAsset& asset : context.assets) if (asset.importer == "GameplayEffect") paths.push_back(asset.path);
            for (const GameplayAssetDiagnostic& diagnostic : LoadEffectAssets(paths, context.read_content, *effects_)) {
                if (context.report) context.report({diagnostic.path, diagnostic.message, diagnostic.content_read_failure});
            }
        }
        if (abilities_ && effects_) {
            std::vector<std::string> paths;
            for (const kit::KitAsset& asset : context.assets) if (asset.importer == "GameplayAbility") paths.push_back(asset.path);
            for (const GameplayAssetDiagnostic& diagnostic : LoadAbilityAssets(paths, context.read_content, *effects_, *abilities_)) {
                if (context.report) context.report({diagnostic.path, diagnostic.message, diagnostic.content_read_failure});
            }
        }
    }
    std::vector<kit::KitStage> Stages() const override {
        return {
            {"Player.Attributes", {"Player.Update", "Player.Sequencer", "Player.Effects"}},
            {"Player.Abilities", {"Player.Update", "Player.Sequencer", "Player.Effects"}},
            // Keep Effects as the gameplay kit's dependency barrier: downstream kits
            // must wait for effects, but can still run before attribute notifications.
            {"Player.Effects", {"Player.Update", "Player.Sequencer"}},
        };
    }
private:
    EffectLibrary* effects_;
    AbilityLibrary* abilities_;
};

} // namespace

std::unique_ptr<kit::IKit> MakeGameplayKit(EffectLibrary* effects, AbilityLibrary* abilities) {
    return std::make_unique<GameplayKit>(effects, abilities);
}

SystemDesc MakeEffectSystem(EffectSystem* system, std::function<void(const EffectEvent&)> on_event) {
    SystemDesc desc;
    desc.name = "Player.Effects";
    desc.phase = SystemPhase::Update;
    desc.after = {"Player.Update", "Player.Sequencer"};
    desc.main_thread_only = true;
    desc.run = [system, on_event = std::move(on_event)](World&, const FrameContext& frame) {
        if (!system) return;
        system->Update(frame.dt);
        const std::vector<EffectEvent> events = system->Events();
        system->ClearEvents();
        if (on_event) for (const EffectEvent& event : events) on_event(event);
    };
    return desc;
}

SystemDesc MakeAbilitySystem(AbilitySystem* system, std::function<void(const AbilityEvent&)> on_event) {
    SystemDesc desc;
    desc.name = "Player.Abilities";
    desc.phase = SystemPhase::Update;
    desc.after = {"Player.Update", "Player.Sequencer", "Player.Effects"};
    desc.main_thread_only = true;
    desc.run = [system, on_event = std::move(on_event)](World&, const FrameContext& frame) {
        if (!system) return;
        system->Update(frame.dt);
        const std::vector<AbilityEvent> events = system->Events();
        system->ClearEvents();
        if (on_event) for (const AbilityEvent& event : events) on_event(event);
    };
    return desc;
}

SystemDesc MakeAttributeSystem(AttributeSystem* system, std::function<void(const AttributeEvent&)> on_event) {
    SystemDesc desc;
    desc.name = "Player.Attributes";
    desc.phase = SystemPhase::Update;
    desc.after = {"Player.Update", "Player.Sequencer", "Player.Effects"};
    desc.main_thread_only = true;
    desc.run = [system, on_event = std::move(on_event)](World&, const FrameContext&) {
        if (!system) return;
        const std::vector<AttributeEvent> events = system->Events();
        system->ClearEvents();
        if (on_event) for (const AttributeEvent& event : events) on_event(event);
    };
    return desc;
}

std::vector<GameplayAssetDiagnostic> LoadEffectAssets(
    const std::vector<std::string>& paths,
    std::function<bool(const std::string&, std::vector<u8>&, std::string*)> read_content,
    EffectLibrary& effects) {
    std::vector<GameplayAssetDiagnostic> diagnostics;
    for (const std::string& path : paths) {
        std::vector<u8> bytes;
        std::string error;
        if (!read_content || !read_content(path, bytes, &error)) {
            diagnostics.push_back({path, std::move(error), true});
            continue;
        }
        GameplayEffect effect;
        const std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        if (!EffectFromJson(text, effect, &error) || !effects.Register(std::move(effect), &error)) {
            diagnostics.push_back({path, std::move(error), false});
        }
    }
    return diagnostics;
}

std::vector<GameplayAssetDiagnostic> LoadAbilityAssets(
    const std::vector<std::string>& paths,
    std::function<bool(const std::string&, std::vector<u8>&, std::string*)> read_content,
    const EffectLibrary& effects, AbilityLibrary& abilities) {
    std::vector<GameplayAssetDiagnostic> diagnostics;
    for (const std::string& path : paths) {
        std::vector<u8> bytes;
        std::string error;
        if (!read_content || !read_content(path, bytes, &error)) {
            diagnostics.push_back({path, std::move(error), true});
            continue;
        }
        GameplayAbility ability;
        const std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        if (!AbilityFromJson(text, ability, &error) ||
            !(error = AbilityLibrary::CheckEffects(ability, effects)).empty() ||
            !abilities.Register(std::move(ability), &error)) {
            diagnostics.push_back({path, std::move(error), false});
        }
    }
    return diagnostics;
}

} // namespace aether::gas
