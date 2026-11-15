#pragma once

#include "aether/kit/kit.h"
#include "aether/loot/loot_system.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace aether::loot {

struct LootAssetDiagnostic {
    std::string path;
    std::string message;
    bool content_read_failure = false;
};

// Loads and validates loot tables against the item library (which the Inventory kit has filled first). The host supplies asset paths and content access;
// diagnostics keep package read failures apart from invalid tables.
std::vector<LootAssetDiagnostic> LoadLootAssets(
    const std::vector<std::string>& paths,
    std::function<bool(const std::string&, std::vector<u8>&, std::string*)> read_content,
    const inv::ItemLibrary& items, LootLibrary& tables);

// This kit's description for the player's kit registry: its name, what it needs (Inventory), the tables it loads and the system it creates. It adds no
// scheduler stage: loot is given when a script (or Blueprint) asks.
std::unique_ptr<kit::IKit> MakeLootKit(LootLibrary* tables = nullptr, const inv::ItemLibrary* items = nullptr);

} // namespace aether::loot
