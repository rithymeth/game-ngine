#include "test_framework.h"

#include "aether/platform/filesystem.h"
#include "aether/save/save_bag.h"
#include "aether/save/save_system.h"
#include "aether/save/settings.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>

// Row A04: files written by this format version must keep loading. These two are frozen snapshots (see
// tests/fixtures/formats/README.md): a settings file and a save slot.

using namespace aether;
using namespace aether::save;
namespace stdfs = std::filesystem;

namespace {

stdfs::path Fixtures() { return stdfs::path(AETHER_REPO_ASSETS_DIR).parent_path() / "tests/fixtures/formats"; }

// A private copy, so a load that rewrites or backs up a file cannot touch the repository's.
stdfs::path Copy(const std::string& test, const std::string& file) {
    const stdfs::path dir = stdfs::temp_directory_path() / "aether_format_fixture_tests" / test;
    stdfs::remove_all(dir);
    stdfs::create_directories(dir);
    stdfs::copy_file(Fixtures() / file, dir / file);
    return dir;
}

std::string ReadAll(const stdfs::path& file) {
    std::string text;
    AETHER_CHECK(fs::ReadFileText(file.string(), text));
    return text;
}

void Replace(const stdfs::path& file, const std::string& from, const std::string& to) {
    std::string text = ReadAll(file);
    const usize at = text.find(from);
    AETHER_CHECK(at != std::string::npos);
    if (at == std::string::npos) return;
    text.replace(at, from.size(), to);
    std::ofstream(file, std::ios::binary | std::ios::trunc) << text;
}

bool Near(f32 a, f32 b) { return std::fabs(a - b) < 1e-4f; }

} // namespace

AETHER_TEST(FormatFixtures_ASettingsFileWrittenByThisVersionStillLoads) {
    const stdfs::path dir = Copy("Settings", "settings.asettings");
    SettingsStore<> store(dir);
    const SaveResult r = store.Load();
    AETHER_CHECK(r.ok);
    AETHER_CHECK(r.warnings.empty()); // every value was already in range: nothing was fixed up
    const GameSettings& s = store.Get();
    AETHER_CHECK(s.quality == "Medium" && s.width == 1920 && s.height == 1080);
    AETHER_CHECK(s.fullscreen && !s.vsync && !s.subtitles);
    AETHER_CHECK(Near(s.master, 0.8f) && Near(s.music, 0.5f) && Near(s.sfx, 0.75f) && Near(s.voice, 1.0f));
    AETHER_CHECK(s.language == "fr");
}

AETHER_TEST(FormatFixtures_ASaveSlotWrittenByThisVersionStillLoads) {
    const stdfs::path dir = Copy("Slot", "slot1.asav");
    SaveSystem saves(dir);
    SaveBag bag;
    const SaveResult r = saves.Load("slot1", bag);
    AETHER_CHECK(r.ok);
    AETHER_CHECK(bag.GetInt("level") == 7);
    AETHER_CHECK(bag.GetString("checkpoint") == "bridge");
    AETHER_CHECK(bag.GetBool("met_the_guide"));
    AETHER_CHECK(Near(bag.GetFloat("health"), 62.5f));
    const std::vector<SlotInfo> slots = saves.ListSlots();
    AETHER_CHECK(slots.size() == 1 && slots[0].valid && slots[0].type == "SaveBag" && slots[0].version == 1);
}

AETHER_TEST(FormatFixtures_ADamagedOrNewerFileIsRefusedAndLeftAlone) {
    {   // a changed value no longer matches the checksum
        const stdfs::path dir = Copy("Damaged", "slot1.asav");
        const std::string before = [&] { Replace(dir / "slot1.asav", "\"bridge\"", "\"cheese\""); return ReadAll(dir / "slot1.asav"); }();
        SaveSystem saves(dir);
        SaveBag bag;
        const SaveResult r = saves.Load("slot1", bag);
        AETHER_CHECK(!r.ok);
        AETHER_CHECK(bag.GetString("checkpoint").empty()); // nothing was half-loaded
        AETHER_CHECK(ReadAll(dir / "slot1.asav") == before); // and the file is as it was
    }
    {   // a save from a newer version of the struct is refused, not guessed at
        const stdfs::path dir = Copy("Future", "slot1.asav");
        Replace(dir / "slot1.asav", "\"type_version\": 1", "\"type_version\": 99");
        const std::string before = ReadAll(dir / "slot1.asav");
        SaveSystem saves(dir);
        SaveBag bag;
        const SaveResult r = saves.Load("slot1", bag);
        AETHER_CHECK(!r.ok && r.error == SaveError::FutureVersion);
        AETHER_CHECK(ReadAll(dir / "slot1.asav") == before);
    }
    {   // settings: damaged gives the defaults and says why
        const stdfs::path dir = Copy("SettingsDamaged", "settings.asettings");
        Replace(dir / "settings.asettings", "\"fr\"", "\"xx\"");
        SettingsStore<> store(dir);
        const SaveResult r = store.Load();
        AETHER_CHECK(r.ok); // Get() is usable
        AETHER_CHECK(r.error != SaveError::None && !r.warnings.empty());
        AETHER_CHECK(store.Get().width == GameSettings{}.width && store.Get().language == "en");
    }
}
