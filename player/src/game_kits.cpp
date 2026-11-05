#include "aether/player/game.h"
#include "game_runtime.h"

#include "aether/core/log.h"
#include <cstring>
#include "aether/gameplay/gameplay_kit.h"
#if AETHER_KIT_INVENTORY
#include "aether/inventory/inventory_system.h"
#endif
#if AETHER_KIT_INTERACTION
#include "aether/interaction/interaction_system.h"
#endif
#if AETHER_KIT_QUESTS
#include "aether/quests/quest_system.h"
#endif
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

void Game::CreateKitRuntime() {
    kit::KitRuntimeContext context;
    context.world = world_.get();
    context.find_service = [this](const char* name) -> void* {
        if (std::strcmp(name, "game.effects") == 0) return runtime_->effects.get();
        if (std::strcmp(name, "game.abilities") == 0) return runtime_->abilities.get();
#if AETHER_KIT_INVENTORY
        if (std::strcmp(name, "inventory.items") == 0) return &items_;
#endif
#if AETHER_KIT_QUESTS
        if (std::strcmp(name, "quests.library") == 0) return &quests_;
#endif
        return nullptr;
    };
    runtime_->kit_runtimes = kits_.CreateRuntime(context);
}

void Game::AddInventoryStage() {
#if AETHER_KIT_INVENTORY
    inv::InventorySystem* inventory_system = runtime_ ? runtime_->kit_runtimes.Get<inv::InventorySystem>("Inventory", "InventorySystem") : nullptr;
    SystemDesc inventory = inv::MakeInventorySystem(inventory_system, [this](const inv::ItemEvent& e) {
        if (!runtime_) return;
        using Kind = inv::ItemEvent::Kind;
        // Quest objectives consume inventory events in the Quests stage, so Inventory has no direct dependency on it.
        if (kits_.Has("Quests")) runtime_->kit_events.Publish(e);
#if AETHER_GAME_SCRIPTING
        if (runtime_->scripts) {
            const f64 count = static_cast<f64>(e.count);
            switch (e.kind) {
            case Kind::Added: runtime_->scripts->SendEvent(e.entity, "OnItemAdded", {e.item, count}); break;
            case Kind::Removed: runtime_->scripts->SendEvent(e.entity, "OnItemRemoved", {e.item, count}); break;
            case Kind::Equipped: runtime_->scripts->SendEvent(e.entity, "OnItemEquipped", {e.item, e.slot}); break;
            case Kind::Unequipped: runtime_->scripts->SendEvent(e.entity, "OnItemUnequipped", {e.item, e.slot}); break;
            case Kind::Used: runtime_->scripts->SendEvent(e.entity, "OnItemUsed", {e.item}); break;
            }
        }
#endif
        if (!runtime_->blueprints) return;
        switch (e.kind) {
        case Kind::Added: {
            const bp::VmValue args[] = {e.item, e.count};
            runtime_->blueprints->VM().Dispatch(e.entity, inv::InventorySystem::kAddedEvent, args);
            break;
        }
        case Kind::Removed: {
            const bp::VmValue args[] = {e.item, e.count};
            runtime_->blueprints->VM().Dispatch(e.entity, inv::InventorySystem::kRemovedEvent, args);
            break;
        }
        case Kind::Equipped:
        case Kind::Unequipped: {
            const bp::VmValue args[] = {e.item, e.slot};
            const char* event = e.kind == Kind::Equipped ? inv::InventorySystem::kEquippedEvent : inv::InventorySystem::kUnequippedEvent;
            runtime_->blueprints->VM().Dispatch(e.entity, event, args);
            break;
        }
        case Kind::Used: {
            const bp::VmValue args[] = {e.item};
            runtime_->blueprints->VM().Dispatch(e.entity, inv::InventorySystem::kUsedEvent, args);
            break;
        }
        }
    });
    ApplyKitStage(inventory, "Player.Inventory");
    runtime_->stages[inventory.name] = std::move(inventory);
#endif
}

void Game::AddInteractionStage() {
#if AETHER_KIT_INTERACTION
    interact::InteractionSystem* interaction_system = runtime_ ? runtime_->kit_runtimes.Get<interact::InteractionSystem>("Interaction", "InteractionSystem") : nullptr;
    SystemDesc interaction = interact::MakeInteractionSystem(interaction_system, [this](const interact::InteractionEvent& e) {
        if (!runtime_) return;
        const bool succeeded = e.kind == interact::InteractionEvent::Kind::Interacted;
#if AETHER_GAME_SCRIPTING
        if (runtime_->scripts) {
            if (succeeded) runtime_->scripts->SendEvent(e.target, "OnInteract", {script::EntityRef{e.interactor}});
            else runtime_->scripts->SendEvent(e.interactor, "OnInteractFailed", {script::EntityRef{e.target}, std::string(interact::ReasonName(e.reason))});
        }
#endif
        if (!runtime_->blueprints) return;
        if (succeeded) {
            const bp::VmValue args[] = {e.interactor};
            runtime_->blueprints->VM().Dispatch(e.target, interact::InteractionSystem::kInteractEvent, args);
        } else {
            const bp::VmValue args[] = {e.target, std::string(interact::ReasonName(e.reason))};
            runtime_->blueprints->VM().Dispatch(e.interactor, interact::InteractionSystem::kFailedEvent, args);
        }
    });
    ApplyKitStage(interaction, "Player.Interaction");
    runtime_->stages[interaction.name] = std::move(interaction);
#endif
}

void Game::AddQuestsStage() {
#if AETHER_KIT_QUESTS
    quest::QuestSystem* quest_system = runtime_ ? runtime_->kit_runtimes.Get<quest::QuestSystem>("Quests", "QuestSystem") : nullptr;
    SystemDesc quests = quest::MakeQuestsSystem(quest_system, [this](quest::QuestSystem& system) {
        if (!runtime_) return;
#if AETHER_KIT_INVENTORY
        for (const inv::ItemEvent& item : runtime_->kit_events.Drain<inv::ItemEvent>()) {
            if (item.kind == inv::ItemEvent::Kind::Added || item.kind == inv::ItemEvent::Kind::Removed) {
                system.Notify(item.entity, quest::Objective::Kind::Count, item.item,
                              item.kind == inv::ItemEvent::Kind::Added ? item.count : -item.count);
            }
        }
#endif
    }, [this](const quest::QuestEvent& e) {
        if (!runtime_) return;
        using Kind = quest::QuestEvent::Kind;
#if AETHER_KIT_INVENTORY
        inv::InventorySystem* inventory = runtime_->kit_runtimes.Get<inv::InventorySystem>("Inventory", "InventorySystem");
        if (e.kind == Kind::Completed && inventory) {
            for (const quest::RewardItem& reward : e.reward_items) inventory->Add(e.entity, reward.item, reward.count);
        }
#endif
#if AETHER_GAME_SCRIPTING
        if (runtime_->scripts) {
            const f64 progress = static_cast<f64>(e.progress), required = static_cast<f64>(e.required);
            switch (e.kind) {
            case Kind::Started: runtime_->scripts->SendEvent(e.entity, "OnQuestStarted", {e.quest}); break;
            case Kind::ObjectiveProgress: runtime_->scripts->SendEvent(e.entity, "OnQuestProgress", {e.quest, e.objective, progress, required}); break;
            case Kind::ObjectiveCompleted: runtime_->scripts->SendEvent(e.entity, "OnQuestObjectiveCompleted", {e.quest, e.objective}); break;
            case Kind::Completed: runtime_->scripts->SendEvent(e.entity, "OnQuestCompleted", {e.quest}); break;
            case Kind::Failed: runtime_->scripts->SendEvent(e.entity, "OnQuestFailed", {e.quest}); break;
            case Kind::Abandoned: runtime_->scripts->SendEvent(e.entity, "OnQuestAbandoned", {e.quest}); break;
            }
        }
#endif
        if (!runtime_->blueprints) return;
        switch (e.kind) {
        case Kind::Started: {
            const bp::VmValue args[] = {e.quest};
            runtime_->blueprints->VM().Dispatch(e.entity, "Event.OnQuestStarted", args);
            break;
        }
        case Kind::ObjectiveProgress: {
            const bp::VmValue args[] = {e.quest, e.objective, e.progress, e.required};
            runtime_->blueprints->VM().Dispatch(e.entity, "Event.OnQuestProgress", args);
            break;
        }
        case Kind::Completed: {
            const bp::VmValue args[] = {e.quest};
            runtime_->blueprints->VM().Dispatch(e.entity, "Event.OnQuestCompleted", args);
            break;
        }
        case Kind::Failed: {
            const bp::VmValue args[] = {e.quest};
            runtime_->blueprints->VM().Dispatch(e.entity, "Event.OnQuestFailed", args);
            break;
        }
        case Kind::ObjectiveCompleted:
        case Kind::Abandoned: break;
        }
    });
    ApplyKitStage(quests, "Player.Quests");
    for (const char* other : {"Inventory", "Interaction"}) {
        if (kits_.Has(other)) quests.after.push_back(std::string("Player.") + other);
    }
    runtime_->stages[quests.name] = std::move(quests);
#endif
}

} // namespace aether::player
