#pragma once

#include "aether/achievements/achievement_system.h"
#include "aether/kit/kit.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace aether::ach {

struct AchievementAssetDiagnostic {
    std::string path;
    std::string message;
    bool content_read_failure = false;
};

// Loads and validates achievement definitions. The host supplies asset paths and content access; diagnostics keep package read failures apart
// from invalid achievements.
std::vector<AchievementAssetDiagnostic> LoadAchievementAssets(
    const std::vector<std::string>& paths,
    std::function<bool(const std::string&, std::vector<u8>&, std::string*)> read_content,
    AchievementLibrary& library);

// This kit's description for the player's kit registry: its name (no dependencies), the achievements it loads and the system it creates over the
// game's save bag (the host's "save.bag" service, and "achievements.library").
std::unique_ptr<kit::IKit> MakeAchievementsKit(AchievementLibrary* library = nullptr);

} // namespace aether::ach
