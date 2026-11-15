#include "aether/achievements/achievements_kit.h"

#include <utility>

namespace aether::ach {

namespace {

class AchievementsKit : public kit::IKit {
public:
    explicit AchievementsKit(AchievementLibrary* library) : library_(library) {}
    const char* Name() const override { return "Achievements"; }
    std::vector<std::string> Deps() const override { return {}; }
    void InstallScriptApi(void* context) override {
        auto* api = static_cast<kit::KitScriptApiContext*>(context);
        if (api && api->install) api->install(Name());
    }
    void LoadAssets(const kit::KitAssetContext& context) override {
        if (!library_) return;
        std::vector<std::string> paths;
        for (const kit::KitAsset& asset : context.assets) if (asset.importer == "AchievementDefinition") paths.push_back(asset.path);
        for (const AchievementAssetDiagnostic& diagnostic : LoadAchievementAssets(paths, context.read_content, *library_)) {
            if (context.report) context.report({diagnostic.path, diagnostic.message, diagnostic.content_read_failure});
        }
    }
    std::unique_ptr<kit::IKitRuntime> CreateRuntime(const kit::KitRuntimeContext& context) override {
        auto* library = context.find_service ? static_cast<AchievementLibrary*>(context.find_service("achievements.library")) : nullptr;
        auto* bag = context.find_service ? static_cast<save::SaveBag*>(context.find_service("save.bag")) : nullptr;
        if (!library || !bag) return {};
        class Runtime final : public kit::IKitRuntime {
        public:
            Runtime(save::SaveBag& bag, const AchievementLibrary& library) : system(bag, library) {}
            void* Find(const char* name) override { return std::string(name) == "AchievementSystem" ? &system : nullptr; }
            AchievementSystem system;
        };
        return std::make_unique<Runtime>(*bag, *library);
    }
private:
    AchievementLibrary* library_;
};

} // namespace

std::vector<AchievementAssetDiagnostic> LoadAchievementAssets(
    const std::vector<std::string>& paths,
    std::function<bool(const std::string&, std::vector<u8>&, std::string*)> read_content,
    AchievementLibrary& library) {
    std::vector<AchievementAssetDiagnostic> diagnostics;
    for (const std::string& path : paths) {
        std::vector<u8> bytes;
        std::string error;
        if (!read_content || !read_content(path, bytes, &error)) {
            diagnostics.push_back({path, std::move(error), true});
            continue;
        }
        AchievementDef achievement;
        const std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        if (!AchievementFromJson(text, achievement, &error) || !library.Register(std::move(achievement), &error)) {
            diagnostics.push_back({path, std::move(error), false});
        }
    }
    return diagnostics;
}

std::unique_ptr<kit::IKit> MakeAchievementsKit(AchievementLibrary* library) { return std::make_unique<AchievementsKit>(library); }

} // namespace aether::ach
