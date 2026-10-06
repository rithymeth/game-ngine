#pragma once

#include <filesystem>
#include <string>

namespace aether::player {

// Where a game keeps what belongs to the player (Phase 28 step 6, §28.7):
// `<root>/Saves` for save slots and `<root>/Config` for settings.
struct UserPaths {
    std::filesystem::path root;
    std::filesystem::path saves;
    std::filesystem::path settings;
};

// The folder name a project gets: letters, digits, '-' and '_' kept, anything
// else (separators, '.', spaces) becomes '_', so a project name can't climb
// out of the user folder. An empty result is "Game".
std::string SanitizeProjectFolder(const std::string& project_name);

// Precedence: `override_root` (the --user-dir option), then the AETHER_USER_DIR
// environment variable, then the OS's per-user data folder (%APPDATA% on
// Windows, ~/Library/Application Support on macOS, $XDG_DATA_HOME or
// ~/.local/share elsewhere) plus the sanitized project name. With none of them
// available, a folder under the system temp directory.
UserPaths ResolveUserPaths(const std::string& project_name, const std::filesystem::path& override_root = {});

} // namespace aether::player
