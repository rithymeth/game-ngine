#include "core/package_runner.h"

#include <cstdio>

int main(int argc, char** argv) {
    if (argc != 3) return 2;

    // Same command layer as the editor's Build and Package window. Both the
    // project and the typed output directory are relative to the editor's CWD.
    using Runner = aether::editor::PackageRunner;
    Runner runner(argv[1]);
    runner.player_path = argv[2];
    runner.output_dir = "Editor Packaged Game";
    const auto user_dir = (std::filesystem::current_path() / "Editor's Player Data" / "Profile One").make_preferred();
    runner.launch_args = "--headless --frames 60 --report --user-dir \"" + user_dir.string() + "\"";
    if (!runner.StartPackage()) return 1;
    runner.WaitForBuild();
    if (runner.LastResult() != Runner::Result::Succeeded) {
        std::fprintf(stderr, "%s\n", runner.Stage().c_str());
        return 1;
    }
    std::filesystem::create_directory("Editor Launch CWD");
    std::filesystem::current_path("Editor Launch CWD");
    std::string error;
    int exit_code = -1;
    if (!runner.Launch(&error) || !runner.Game().Wait(&exit_code) || exit_code != 0) {
        std::fprintf(stderr, "Editor Launch failed: %s (exit %d)\n", error.c_str(), exit_code);
        return 1;
    }
    if (!std::filesystem::is_directory(user_dir / "Crashes")) {
        std::fprintf(stderr, "Editor Launch lost the quoted user directory\n");
        return 1;
    }
    runner.launch_args = "--headless --user-dir \"unterminated";
    if (runner.Launch(&error) || error.find("unclosed double quote") == std::string::npos) {
        std::fprintf(stderr, "Editor Launch did not reject the unclosed quote\n");
        return 1;
    }
    return 0;
}
