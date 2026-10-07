#include "aether/player/game.h"

#include "aether/core/log.h"
#include "aether/gameplay/gameplay_kit.h"
#if AETHER_KIT_INVENTORY
#include "aether/inventory/inventory_kit.h"
#endif
#if AETHER_KIT_INTERACTION
#include "aether/interaction/interaction_kit.h"
#endif
#if AETHER_KIT_QUESTS
#include "aether/quests/quests_kit.h"
#endif

namespace aether::player {

void Game::BuildKits() {
    // Optional kit selection is kept out of the player runtime implementation. Each kit declares its own
    // dependencies and stages; the registry resolves the final install order.
    kits_.Add(gas::MakeGameplayKit(&effects_, &abilities_));
#if AETHER_KIT_INVENTORY
    kits_.Add(inv::MakeInventoryKit(&items_, &effects_));
#endif
#if AETHER_KIT_INTERACTION
    kits_.Add(interact::MakeInteractionKit());
#endif
#if AETHER_KIT_QUESTS
    kits_.Add(quest::MakeQuestsKit(&quests_, &effects_));
#endif
    if (!kits_.Resolve()) {
        for (const std::string& error : kits_.Errors()) AETHER_LOG_ERROR("Player", "Kits: %s", error.c_str());
    }
}

void Game::ApplyKitStage(SystemDesc& system, const std::string& name) const {
    for (const kit::KitStage& stage : kits_.Stages()) {
        if (stage.name != name) continue;
        system.name = stage.name;
        system.after = stage.after;
        return;
    }
    AETHER_LOG_ERROR("Player", "Kits: no stage named %s", name.c_str());
}

void Game::LoadKitAssets() {
    if (kit_assets_loaded_) return;
    kit_assets_loaded_ = true;
    kit::KitAssetContext context;
    for (const GameManifest::Asset& asset : package_.Manifest().assets) {
        context.assets.push_back({asset.importer, asset.path});
    }
    context.read_content = [this](const std::string& path, std::vector<u8>& bytes, std::string* error) {
        return package_.ReadContent(path, bytes, error);
    };
    context.report = [this](const kit::KitAssetDiagnostic& diagnostic) {
        if (diagnostic.content_read_failure) {
            warnings_.push_back(diagnostic.message);
            return;
        }
        const std::string message = diagnostic.path.empty() ? diagnostic.message : diagnostic.path + ": " + diagnostic.message;
        effect_warnings_.push_back(message);
        AETHER_LOG_WARN("Player", "%s", message.c_str());
    };
    kits_.LoadAssets(context);
}

} // namespace aether::player
