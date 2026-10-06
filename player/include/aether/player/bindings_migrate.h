#pragma once

#include "aether/save/settings.h"

#include <filesystem>
#include <string>
#include <vector>

namespace aether::player {

// Before Phase 28 a player's rebinds lived in `<saved_dir>/Config/Input.json`
// (input::UserBindingsPath). They are now part of GameSettings, so a game
// that has the old file moves it in, once (§28.6):
//
// - no old file: nothing happens (this is what makes it idempotent);
// - a damaged old file: a warning, the file stays, the settings are untouched;
// - the settings already have rebinds: they win and the old file is only
//   set aside;
// - otherwise the rebinds are copied into the store (Set, so observers hear
//   it; the caller saves) and the old file is renamed `Input.json.migrated`,
//   not deleted.
//
// Returns warnings, one line each.
std::vector<std::string> MigrateLegacyBindings(const std::filesystem::path& saved_dir, save::SettingsStore<save::GameSettings>& store);

} // namespace aether::player
