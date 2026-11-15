#include "aether/dialogue/dialogue_kit.h"

#include <utility>

namespace aether::dlg {

namespace {

class DialogueKit : public kit::IKit {
public:
    explicit DialogueKit(DialogueLibrary* dialogues) : dialogues_(dialogues) {}
    const char* Name() const override { return "Dialogue"; }
    std::vector<std::string> Deps() const override { return {}; }
    void InstallScriptApi(void* context) override {
        auto* api = static_cast<kit::KitScriptApiContext*>(context);
        if (api && api->install) api->install(Name());
    }
    void LoadAssets(const kit::KitAssetContext& context) override {
        if (!dialogues_) return;
        std::vector<std::string> paths;
        for (const kit::KitAsset& asset : context.assets) if (asset.importer == "DialogueDefinition") paths.push_back(asset.path);
        for (const DialogueAssetDiagnostic& diagnostic : LoadDialogueAssets(paths, context.read_content, *dialogues_)) {
            if (context.report) context.report({diagnostic.path, diagnostic.message, diagnostic.content_read_failure});
        }
    }
    std::unique_ptr<kit::IKitRuntime> CreateRuntime(const kit::KitRuntimeContext& context) override {
        auto* dialogues = context.find_service ? static_cast<DialogueLibrary*>(context.find_service("dialogue.library")) : nullptr;
        if (!context.world || !dialogues) return {};
        class Runtime final : public kit::IKitRuntime {
        public:
            Runtime(World& world, const DialogueLibrary& dialogues) : system(world, dialogues) {}
            void* Find(const char* name) override { return std::string(name) == "DialogueSystem" ? &system : nullptr; }
            DialogueSystem system;
        };
        return std::make_unique<Runtime>(*context.world, *dialogues);
    }
private:
    DialogueLibrary* dialogues_;
};

} // namespace

std::vector<DialogueAssetDiagnostic> LoadDialogueAssets(
    const std::vector<std::string>& paths,
    std::function<bool(const std::string&, std::vector<u8>&, std::string*)> read_content,
    DialogueLibrary& dialogues) {
    std::vector<DialogueAssetDiagnostic> diagnostics;
    for (const std::string& path : paths) {
        std::vector<u8> bytes;
        std::string error;
        if (!read_content || !read_content(path, bytes, &error)) {
            diagnostics.push_back({path, std::move(error), true});
            continue;
        }
        DialogueDef dialogue;
        const std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        if (!DialogueFromJson(text, dialogue, &error) || !dialogues.Register(std::move(dialogue), &error)) {
            diagnostics.push_back({path, std::move(error), false});
        }
    }
    return diagnostics;
}

std::unique_ptr<kit::IKit> MakeDialogueKit(DialogueLibrary* dialogues) { return std::make_unique<DialogueKit>(dialogues); }

} // namespace aether::dlg
