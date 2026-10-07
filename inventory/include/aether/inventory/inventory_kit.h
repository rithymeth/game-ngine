#pragma once

#include "aether/kit/kit.h"
#include "aether/inventory/inventory_system.h"
#include "aether/scene/scheduler.h"

#include <memory>
#include <functional>
#include <string>
#include <vector>

namespace aether::inv {

// Builds the inventory update system. Event delivery stays with the host so this
// kit does not depend on scripting, Blueprints, or other optional kits.
SystemDesc MakeInventorySystem(InventorySystem* system, std::function<void(const ItemEvent&)> on_event);

struct ItemAssetDiagnostic {
    std::string path;
    std::string message;
    bool content_read_failure = false;
};

// Loads and validates item definitions against the shared gameplay effect library.
// The host supplies asset paths and content access; diagnostics preserve the distinction
// between package read failures and invalid item definitions.
std::vector<ItemAssetDiagnostic> LoadItemAssets(
    const std::vector<std::string>& paths,
    std::function<bool(const std::string&, std::vector<u8>&, std::string*)> read_content,
    const gas::EffectLibrary& effects, ItemLibrary& items);

// This kit's description for the player's kit registry (Phase 37 step 4): its name, what it needs, the
// components it registers and the scheduler stage it adds.
std::unique_ptr<kit::IKit> MakeInventoryKit(ItemLibrary* items = nullptr, const gas::EffectLibrary* effects = nullptr);

} // namespace aether::inv
