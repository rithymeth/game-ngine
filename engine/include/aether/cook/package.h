#pragma once

#include "aether/cook/cooker.h"

#include <filesystem>
#include <string>
#include <vector>

// Packaging (Phase 25 step 5): a cook staged with the player, as a folder
// that runs on its own:
//   <output>/<Game>[.exe]   the player (aether_player), named for the project
//   <output>/*.dll          the player's DLLs, on Windows
//   <output>/Paks/<pak>.apak and CookManifest.json
//   <output>/PackageManifest.json  file sizes and CRC-32 checksums
// The editor's Build and Package window runs this; so can tools.

namespace aether::cook {

struct PackageOptions {
    CookOptions cook;                         // its output_dir is replaced by <output_dir>/Paks
    std::filesystem::path output_dir;
    std::filesystem::path player_executable;  // empty: FindPlayerExecutable()
};

struct PackageReport {
    bool ok = false;
    std::string error;
    CookReport cook;
    std::filesystem::path game_executable;    // the staged player
    std::filesystem::path paks_dir;
    std::filesystem::path manifest_file;
    std::vector<std::filesystem::path> copied; // the player and its DLLs, as staged
};

PackageReport Package(const PackageOptions& options);

// Checks every file listed in PackageManifest.json against its size and CRC-32.
// This detects damage or accidental replacement; it is not a signature.
bool VerifyPackageManifest(const std::filesystem::path& directory, std::vector<std::string>& problems);

// aether_player beside the running executable (a packaged editor), else the
// one this engine was built with; empty if neither exists.
std::filesystem::path FindPlayerExecutable();

// The project's name made into a file name (letters, digits, '-', '_';
// "Game" if nothing is left), with ".exe" on Windows.
std::string GameExecutableName(const std::string& project_name);

} // namespace aether::cook
