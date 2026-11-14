#include "workspace/layout_presets.h"

#include "test_framework.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

// Stage 1 of docs/design/EDITOR_PARITY_AUDIT.md: Window > Layout. The store is plain files, so it runs here without
// a window or a graphics device: the built-in names come first, saved layouts follow, and a name never becomes a path
// that leaves the folder.

using namespace aether::editor;
namespace stdfs = std::filesystem;

#define CHECK(...) AETHER_CHECK((__VA_ARGS__))

namespace {

stdfs::path Scratch(const char* name) {
    const stdfs::path dir = stdfs::temp_directory_path() / name;
    std::error_code ignored;
    stdfs::remove_all(dir, ignored);
    stdfs::create_directories(dir);
    return dir;
}

} // namespace

// The built-ins are the four arrangements the shell draws, in menu order, and a file named after one is not listed
// twice.
AETHER_TEST(LayoutPresets_BuiltinsComeFirstThenSavedLayouts) {
    const stdfs::path dir = Scratch("aether_layout_presets_names");
    LayoutPresets presets(dir);
    CHECK(presets.Directory() == dir);
    CHECK(LayoutPresets::BuiltinNames().size() == 4);
    CHECK(LayoutPresets::BuiltinNames().front() == "Default");
    CHECK(LayoutPresets::IsBuiltin("Default") && LayoutPresets::IsBuiltin("Scripting"));
    CHECK(!LayoutPresets::IsBuiltin("Mine"));
    CHECK(presets.Names() == LayoutPresets::BuiltinNames()); // nothing saved yet
    CHECK(presets.SavedNames().empty());

    CHECK(presets.Save("zeta", "[Window][x]\n"));
    CHECK(presets.Save("Alpha", "[Window][y]\n"));
    CHECK(presets.Save("Default", "[Window][z]\n")); // saving under a built-in name is allowed
    const std::vector<std::string> saved = presets.SavedNames();
    CHECK(saved.size() == 2); // Default is a built-in, so it is not repeated
    CHECK(saved[0] == "Alpha" && saved[1] == "zeta");
    const std::vector<std::string> names = presets.Names();
    CHECK(names.size() == 6);
    CHECK(names[0] == "Default" && names[1] == "Level design" && names[2] == "Scripting" && names[3] == "Animation");
    CHECK(names[4] == "Alpha" && names[5] == "zeta");
    CHECK(presets.Load("Default").has_value()); // a file of that name still wins when the name is chosen
}

// A preset is written whole or not at all, and reading one back gives exactly what was saved.
AETHER_TEST(LayoutPresets_RoundTripsAPreset) {
    const stdfs::path dir = Scratch("aether_layout_presets_round_trip");
    LayoutPresets presets(dir / "nested" / "layouts"); // saving makes the folder
    const std::string ini = "[Window][Editors]\nPos=0,0\nSize=800,600\n\n[Docking][Data]\nDockSpace ID=0x1\n";
    std::string error;
    CHECK(presets.Save("My layout", ini, &error) && error.empty());
    CHECK(stdfs::exists(presets.Path("My layout")));
    CHECK(!stdfs::exists(stdfs::path(presets.Path("My layout")).concat(".tmp")));
    const std::optional<std::string> back = presets.Load("My layout");
    CHECK(back.has_value() && *back == ini);
    CHECK(!presets.Load("Not there").has_value());

    std::ofstream(presets.Path("Blank")) << ""; // an empty file is no layout
    CHECK(!presets.Load("Blank").has_value());

    CHECK(!presets.Save("", ini, &error) && !error.empty());       // no name
    CHECK(!presets.Save("  ...  ", ini, &error) && !error.empty()); // nothing a file could be called
    CHECK(!presets.Save("Empty", "", &error) && !error.empty());    // nothing to save
}

// A name is turned into a file name that stays inside the preset folder, on every platform the editor builds for.
AETHER_TEST(LayoutPresets_SanitizesNamesIntoFiles) {
    const stdfs::path dir = Scratch("aether_layout_presets_names_files");
    LayoutPresets presets(dir);
    CHECK(presets.Path("My layout") == dir / "My layout.ini");
    CHECK(presets.Path("a/b:c*?") == dir / "a_b_c__.ini"); // separators and punctuation become '_'
    CHECK(presets.Path("../escape") == dir / "_escape.ini"); // leading dots go, so no parent folder
    CHECK(presets.Path("...").empty());                       // nothing left to name it
    CHECK(presets.Path("  ").empty());
    const std::string long_name(200, 'x');
    const std::string stem = presets.Path(long_name).stem().string();
    CHECK(stem.size() == 64); // a sane file name, not a 200-character one
    CHECK(presets.Path("Save.") == dir / "Save.ini"); // a trailing dot would be dropped by Windows
}
