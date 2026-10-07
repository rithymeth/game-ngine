#include "test_framework.h"

#include "aether/platform/filesystem.h"
#include "aether/save/save_system.h"
#include "aether/save/settings.h"

#include <cmath>
#include <filesystem>
#include <fstream>

// Phase 28 step 3 (§28.3): the settings store, GameSettings and its validation.

using namespace aether;
using namespace aether::save;
namespace stdfs = std::filesystem;

#define CHECK(...) AETHER_CHECK((__VA_ARGS__))

namespace settings_test {
// A game's own settings struct, with its own validation found by ADL.
struct Options {
    i32 difficulty = 1;
    bool subtitles = true;
};
void Validate(Options& o, std::vector<std::string>* warnings) {
    if (o.difficulty < 0 || o.difficulty > 3) {
        o.difficulty = 1;
        if (warnings) warnings->push_back("difficulty out of range");
    }
}
} // namespace settings_test

AETHER_REFLECT(settings_test::Options, 1, AETHER_FIELD(difficulty, Field_EditAnywhere), AETHER_FIELD(subtitles, Field_EditAnywhere))

namespace {

stdfs::path Dir(const std::string& name) {
    const stdfs::path dir = stdfs::temp_directory_path() / "aether_settings_tests" / name;
    stdfs::remove_all(dir);
    return dir;
}

std::string ReadAll(const stdfs::path& file) {
    std::string text;
    CHECK(fs::ReadFileText(file.string(), text));
    return text;
}

void WriteAll(const stdfs::path& file, const std::string& text) {
    stdfs::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary | std::ios::trunc) << text;
}

bool Near(f32 a, f32 b, f32 eps = 1e-4f) { return std::fabs(a - b) <= eps; }

} // namespace

AETHER_TEST(Settings_DefaultsWhenTheFileIsMissing) {
    SettingsStore<> store(Dir("Missing"));
    const SaveResult r = store.Load();
    CHECK(r.ok && r.error == SaveError::None && r.warnings.empty());
    CHECK(store.Get().quality == "High" && store.Get().width == 1280 && store.Get().vsync && Near(store.Get().master, 1.0f) && store.Get().language == "en");
    CHECK(!stdfs::exists(store.Path())); // loading doesn't create it
}

AETHER_TEST(Settings_RoundTripAndSeparateFromSaves) {
    const stdfs::path dir = Dir("RoundTrip");
    {
        SettingsStore<> store(dir);
        GameSettings s;
        s.quality = "Low";
        s.width = 1920;
        s.height = 1080;
        s.fullscreen = true;
        s.vsync = false;
        s.master = 0.5f;
        s.music = 0.25f;
        s.language = "fr";
        s.bindings.Set("Gameplay", "Jump", 0, input::Key::Space);
        store.Set(s);
        CHECK(store.Save().ok && store.Path().extension() == ".asettings");
    }
    SettingsStore<> again(dir);
    const SaveResult r = again.Load();
    const GameSettings& g = again.Get();
    CHECK(r.ok && r.warnings.empty() && g.quality == "Low" && g.width == 1920 && g.fullscreen && !g.vsync);
    CHECK(Near(g.master, 0.5f) && Near(g.music, 0.25f) && g.language == "fr");
    CHECK(g.bindings.overrides.size() == 1 && g.bindings.Find("Gameplay", "Jump", 0) != nullptr);
    // The file is a settings file, readable JSON, in its own envelope kind.
    const reflect::Json j = reflect::Json::parse(ReadAll(again.Path()));
    CHECK(j["$type"] == "aether.settings" && j["type"] == "GameSettings" && j["data"]["language"] == "fr");
    // A save slot and a settings file are never mistaken for each other.
    SaveSystem saves(dir);
    CHECK(saves.Save("slot", g).ok);
    stdfs::copy_file(dir / "slot.asav", dir / "fromsave.asettings");
    SettingsStore<> wrong(dir, "fromsave");
    const SaveResult w = wrong.Load();
    CHECK(w.ok && w.error == SaveError::Corrupt && wrong.Get().quality == "High"); // defaults, with the reason
}

AETHER_TEST(Settings_DamagedFileFallsBackToTheBackupThenTheDefaults) {
    const stdfs::path dir = Dir("Damaged");
    SettingsStore<> store(dir);
    GameSettings a;
    a.language = "de";
    store.Set(a);
    CHECK(store.Save().ok);
    GameSettings b = a;
    b.language = "es";
    store.Set(b);
    CHECK(store.Save().ok && stdfs::exists(store.Path().string() + ".bak"));
    // The newest file is damaged: the previous settings load.
    WriteAll(store.Path(), "{ nope");
    SettingsStore<> reload(dir);
    SaveResult r = reload.Load();
    CHECK(r.ok && reload.Get().language == "de" && r.warnings.size() == 1 && r.warnings[0].find("previous") != std::string::npos);
    // Both damaged: the defaults, and the reason; the files aren't overwritten by loading.
    WriteAll(store.Path().string() + ".bak", "also bad");
    SettingsStore<> hopeless(dir);
    r = hopeless.Load();
    CHECK(r.ok && r.error == SaveError::Corrupt && hopeless.Get().language == "en" && !r.warnings.empty());
    CHECK(ReadAll(store.Path()) == "{ nope");
    // Saving then replaces the bad file.
    CHECK(hopeless.Save().ok);
    SettingsStore<> fine(dir);
    CHECK(fine.Load().ok && fine.Get().language == "en");
}

AETHER_TEST(Settings_ValidationClampsWithWarnings) {
    GameSettings s;
    s.master = 3.0f;
    s.music = -1.0f;
    s.sfx = std::nanf("");
    s.width = 5;
    s.height = 100000;
    s.language = "";
    s.quality = std::string(100, 'x');
    std::vector<std::string> warnings;
    Validate(s, &warnings);
    CHECK(warnings.size() == 7);
    CHECK(Near(s.master, 1.0f) && Near(s.music, 0.0f) && Near(s.sfx, 1.0f) && s.width == 320 && s.height == 16384);
    CHECK(s.language == "en" && s.quality == "High");
    warnings.clear();
    Validate(s, &warnings);
    CHECK(warnings.empty()); // valid settings are left alone

    // A hand-edited file is validated on load.
    const stdfs::path dir = Dir("Validate");
    SettingsStore<> store(dir);
    GameSettings bad;
    bad.master = 0.4f;
    store.Set(bad);
    CHECK(store.Save().ok);
    reflect::Json j = reflect::Json::parse(ReadAll(store.Path()));
    j["data"]["master"] = 9.0;
    // (the checksum is the file's guard: a hand edit needs it recomputed, so this one is refused)
    WriteAll(store.Path(), j.dump());
    SettingsStore<> reload(dir);
    CHECK(reload.Load().error == SaveError::Corrupt);
}

AETHER_TEST(Settings_ObserversHearAboutRealChangesOnly) {
    SettingsStore<> store(Dir("Observers"));
    int calls = 0;
    f32 last_before = -1, last_now = -1;
    const u64 id = store.AddObserver([&](const GameSettings& now, const GameSettings& before) {
        ++calls;
        last_now = now.master;
        last_before = before.master;
    });
    GameSettings s = store.Get();
    CHECK(!store.Set(s) && calls == 0); // nothing changed
    s.master = 0.3f;
    CHECK(store.Set(s) && calls == 1 && Near(last_now, 0.3f) && Near(last_before, 1.0f));
    s.master = 7.0f; // validated to 1: a change from 0.3
    CHECK(store.Set(s) && calls == 2 && Near(store.Get().master, 1.0f));
    CHECK(!store.Set(s) && calls == 2); // 7 validates to the same 1
    CHECK(store.ResetToDefaults() == false && calls == 2); // already the defaults
    s.language = "ja";
    store.Set(s);
    CHECK(calls == 3 && store.ResetToDefaults() && calls == 4 && store.Get().language == "en");
    store.RemoveObserver(id);
    s.master = 0.1f;
    store.Set(s);
    CHECK(calls == 4);
    // A Set from inside an observer takes effect without notifying again.
    SettingsStore<> chain(Dir("Chain"));
    int chain_calls = 0;
    chain.AddObserver([&](const GameSettings& now, const GameSettings&) {
        ++chain_calls;
        if (now.sfx != 0.5f) {
            GameSettings next = now;
            next.sfx = 0.5f;
            chain.Set(next);
        }
    });
    GameSettings c;
    c.music = 0.2f;
    chain.Set(c);
    CHECK(chain_calls == 1 && Near(chain.Get().sfx, 0.5f));
}

AETHER_TEST(Settings_AnyReflectedStructCanBeSettings) {
    const stdfs::path dir = Dir("Custom");
    SettingsStore<settings_test::Options> store(dir, "options");
    CHECK(store.Load().ok && store.Get().difficulty == 1);
    settings_test::Options o;
    o.difficulty = 9; // the struct's own validation brings it back
    o.subtitles = false;
    store.Set(o);
    CHECK(store.Get().difficulty == 1 && !store.Get().subtitles);
    CHECK(store.Save().ok && store.Path().filename() == "options.asettings");
    SettingsStore<settings_test::Options> again(dir, "options");
    CHECK(again.Load().ok && !again.Get().subtitles);
    // A newer file version than the code knows is refused with the defaults.
    reflect::Json j = reflect::Json::parse(ReadAll(store.Path()));
    j["type_version"] = 9;
    WriteAll(store.Path(), j.dump());
    SettingsStore<settings_test::Options> future(dir, "options");
    const SaveResult r = future.Load();
    CHECK(r.ok && r.error == SaveError::FutureVersion && future.Get().subtitles);
}

AETHER_TEST(Settings_BusVolumeInDecibels) {
    CHECK(Near(BusVolumeDb(1.0f), 0.0f) && Near(BusVolumeDb(0.5f), -6.0206f, 1e-3f) && Near(BusVolumeDb(0.1f), -20.0f, 1e-3f));
    CHECK(BusVolumeDb(0.0f) == -80.0f && BusVolumeDb(-1.0f) == -80.0f && BusVolumeDb(std::nanf("")) == -80.0f);
    CHECK(BusVolumeDb(1e-9f) == -80.0f && BusVolumeDb(5.0f) == 0.0f); // below the floor, and above full
}

AETHER_TEST(Settings_SavingKeepsABackupAndLeavesNoTempFile) {
    const stdfs::path dir = Dir("Atomic");
    SettingsStore<> store(dir);
    CHECK(store.Save().ok && !stdfs::exists(store.Path().string() + ".bak"));
    GameSettings s;
    s.vsync = false;
    store.Set(s);
    CHECK(store.Save().ok && stdfs::exists(store.Path().string() + ".bak"));
    for (const std::string& name : fs::ListDirectory(dir.string())) CHECK(name.find(".tmp") == std::string::npos);
}
