#include "test_framework.h"

#include "aether/pak/pak.h"
#include "aether/platform/filesystem.h"
#include "aether/reflection/serialize.h"
#include "aether/save/envelope.h"
#include "aether/save/save_system.h"

#include <cstdio>
#include <cstring>
#include <span>
#include <filesystem>
#include <fstream>

// Phase 28 step 1 (§28.1): save slots, the .asav envelope, atomic writes,
// backups, migration and async saving.

using namespace aether;
using namespace aether::save;
namespace stdfs = std::filesystem;

#define CHECK(...) AETHER_CHECK((__VA_ARGS__))

namespace save_test {
struct Profile {
    i32 level = 1;
    f32 health = 100.0f;
    std::string name = "Hero";
    std::vector<i32> inventory;
    bool hard_mode = false;
};
struct SaveOther {
    i32 x = 0;
};
// Version 2 renamed "hp" to "health" and added "shield"; the migration hook carries old saves over.
struct SaveFighter {
    f32 health = 10.0f;
    f32 shield = 5.0f;
};
void MigrateFighter(u16 from, reflect::Json& data) {
    if (from < 2 && data.contains("hp")) {
        data["health"] = data["hp"];
        data.erase("hp");
    }
}
} // namespace save_test

AETHER_REFLECT(save_test::Profile, 1, AETHER_FIELD(level, Field_EditAnywhere), AETHER_FIELD(health, Field_EditAnywhere),
               AETHER_FIELD(name, Field_EditAnywhere), AETHER_FIELD(inventory, Field_EditAnywhere), AETHER_FIELD(hard_mode, Field_EditAnywhere))
AETHER_REFLECT(save_test::SaveOther, 1, AETHER_FIELD(x, Field_EditAnywhere))
AETHER_REFLECT(save_test::SaveFighter, 2, AETHER_FIELD(health, Field_EditAnywhere), AETHER_FIELD(shield, Field_EditAnywhere))

namespace {

stdfs::path Dir(const std::string& name) {
    const stdfs::path dir = stdfs::temp_directory_path() / "aether_save_tests" / name;
    stdfs::remove_all(dir);
    return dir;
}

std::string ReadAll(const stdfs::path& file) {
    std::string text;
    CHECK(fs::ReadFileText(file.string(), text));
    return text;
}

void WriteAll(const stdfs::path& file, const std::string& text) {
    std::ofstream(file, std::ios::binary | std::ios::trunc) << text;
}

// A save file as an older (or newer) version of the game would have written it.
std::string Envelope(const char* type, u32 type_version, const reflect::Json& data) {
    const std::string dumped = data.dump();
    char sum[16];
    std::snprintf(sum, sizeof sum, "%08x", pak::Crc32(std::span<const u8>(reinterpret_cast<const u8*>(dumped.data()), dumped.size())));
    const reflect::Json j = {{"$type", "aether.save"}, {"format", 1}, {"type", type}, {"type_version", type_version},
                             {"slot", "x"}, {"timestamp_utc", 1000}, {"checksum", sum}, {"data", data}};
    return j.dump(2);
}

save_test::Profile Sample() {
    save_test::Profile p;
    p.level = 7;
    p.health = 42.5f;
    p.name = "Ada";
    p.inventory = {3, 1, 4, 1, 5};
    p.hard_mode = true;
    return p;
}

bool SameProfile(const save_test::Profile& a, const save_test::Profile& b) {
    return a.level == b.level && a.health == b.health && a.name == b.name && a.inventory == b.inventory && a.hard_mode == b.hard_mode;
}

} // namespace

AETHER_TEST(Save_RoundTripsAReflectedStruct) {
    SaveSystem saves(Dir("RoundTrip"));
    const save_test::Profile original = Sample();
    SaveResult r = saves.Save("slot1", original);
    CHECK(r.ok && r.error == SaveError::None);
    save_test::Profile loaded;
    r = saves.Load("slot1", loaded);
    CHECK(r.ok && r.warnings.empty() && SameProfile(original, loaded));
    // The file is readable JSON in an envelope naming the type, its version and the time.
    const reflect::Json j = reflect::Json::parse(ReadAll(saves.Directory() / "slot1.asav"));
    CHECK(j["$type"] == "aether.save" && j["type"] == "Profile" && j["type_version"] == 1 && j["timestamp_utc"].get<u64>() > 1000000);
    CHECK(j["data"]["level"] == 7 && j["data"]["name"] == "Ada" && j["checksum"].get<std::string>().size() == 8);
}

AETHER_TEST(Save_SlotNamesCantEscapeTheFolder) {
    SaveSystem saves(Dir("Names"));
    for (const char* bad : {"", "../evil", "a/b", "a\\b", "with space", "dot.name", ".."}) {
        CHECK(!SaveSystem::ValidSlotName(bad));
        save_test::Profile p;
        CHECK(saves.Save(bad, p).error == SaveError::InvalidSlot && saves.Load(bad, p).error == SaveError::InvalidSlot);
        CHECK(!saves.Exists(bad) && saves.DeleteSlot(bad).error == SaveError::InvalidSlot);
    }
    CHECK(SaveSystem::ValidSlotName("Slot_1-b") && SaveSystem::ValidSlotName(std::string(64, 'a')) && !SaveSystem::ValidSlotName(std::string(65, 'a')));
    CHECK(!stdfs::exists(saves.Directory().parent_path() / "evil.asav"));
}

AETHER_TEST(Save_ListsExistsAndDeletesSlots) {
    SaveSystem saves(Dir("List"));
    CHECK(saves.ListSlots().empty() && !saves.Exists("a"));
    save_test::Profile p = Sample();
    save_test::SaveOther o;
    CHECK(saves.Save("beta", p).ok && saves.Save("alpha", o).ok);
    WriteAll(saves.Directory() / "junk.asav", "not a save");
    WriteAll(saves.Directory() / "notes.txt", "ignored");
    CHECK(saves.Exists("alpha") && saves.Exists("beta") && !saves.Exists("gamma"));
    const std::vector<SlotInfo> slots = saves.ListSlots();
    CHECK(slots.size() == 3 && slots[0].slot == "alpha" && slots[1].slot == "beta" && slots[2].slot == "junk");
    CHECK(slots[0].valid && slots[0].type == "SaveOther" && slots[0].version == 1 && slots[0].bytes > 0 && slots[0].timestamp > 1000000);
    CHECK(slots[1].type == "Profile" && !slots[2].valid && slots[2].type.empty());
    CHECK(saves.DeleteSlot("alpha").ok && !saves.Exists("alpha") && saves.DeleteSlot("alpha").error == SaveError::NotFound);
    CHECK(saves.Load("alpha", o).error == SaveError::NotFound);
}

AETHER_TEST(Save_ListSlotsOnlyAdvertisesAddressableNames) {
    SaveSystem saves(Dir("Addressable"));
    const auto profile = Sample();
    if (!saves.Save("Valid_1-A", profile).ok) { CHECK(false); return; }
    const auto valid = saves.Directory() / "Valid_1-A.asav";
    const std::string payload = ReadAll(valid);
    WriteAll(saves.Directory() / "copied save.asav", payload);
    WriteAll(saves.Directory() / "manual.copy.asav.bak", payload);
    WriteAll(saves.Directory() / (std::string(65, 'x') + ".asav"), payload);
    WriteAll(saves.Directory() / "damaged.asav", "unreadable");
    stdfs::rename(valid, saves.Directory() / "Valid_1-A.asav.bak");

    const auto slots = saves.ListSlots();
    CHECK(slots.size() == 2);
    if (slots.size() == 2) {
        CHECK(slots[0].slot == "Valid_1-A" && slots[0].valid && saves.Exists(slots[0].slot));
        CHECK(slots[1].slot == "damaged" && !slots[1].valid && saves.Exists(slots[1].slot));
    }
    for (const auto& slot : slots) CHECK(SaveSystem::ValidSlotName(slot.slot));
    CHECK(ReadAll(saves.Directory() / "copied save.asav") == payload);
    CHECK(ReadAll(saves.Directory() / "manual.copy.asav.bak") == payload);
}

AETHER_TEST(Save_DeleteSlotPreservesFilesWhenAPathIsADirectory) {
    SaveSystem saves(Dir("BlockedDelete"));
    const auto profile = Sample();
    if (!saves.Save("primary", profile).ok) { CHECK(false); return; }
    const auto primary = saves.Directory() / "primary.asav";
    const auto blocked_backup = saves.Directory() / "primary.asav.bak";
    const std::string original = ReadAll(primary);
    stdfs::create_directory(blocked_backup);
    WriteAll(blocked_backup / "keep.txt", "not a save");
    CHECK(saves.DeleteSlot("primary").error == SaveError::IoError);
    CHECK(stdfs::is_regular_file(primary) && ReadAll(primary) == original);
    CHECK(stdfs::is_regular_file(blocked_backup / "keep.txt"));

    const auto blocked_primary = saves.Directory() / "backup.asav";
    const auto backup = saves.Directory() / "backup.asav.bak";
    stdfs::create_directory(blocked_primary); // an empty directory is not a save either
    WriteAll(backup, original);
    CHECK(saves.DeleteSlot("backup").error == SaveError::IoError);
    CHECK(stdfs::is_directory(blocked_primary));
    CHECK(stdfs::is_regular_file(backup) && ReadAll(backup) == original);
}

AETHER_TEST(Save_SlotListKeepsGoodSavesWhenOtherHeadersAreDamaged) {
    SaveSystem saves(Dir("DamagedList"));
    const auto profile = Sample();
    if (!saves.Save("good", profile).ok) { CHECK(false); return; }
    const auto good = reflect::Json::parse(ReadAll(saves.Directory() / "good.asav"));
    const auto write_bad = [&](const char* slot, const char* key, reflect::Json value) {
        auto bad = good;
        bad[key] = std::move(value);
        WriteAll(saves.Directory() / (std::string(slot) + ".asav"), bad.dump());
    };
    write_bad("bad_type", "type", reflect::Json::object());
    write_bad("bad_time", "timestamp_utc", "yesterday");
    write_bad("negative_time", "timestamp_utc", -1);
    write_bad("wide_version", "type_version", 65536u);
    write_bad("fractional_version", "type_version", 1.5);

    const auto slots = saves.ListSlots();
    usize valid = 0;
    for (const auto& info : slots) {
        valid += info.valid ? 1 : 0;
        CHECK(info.bytes > 0);
        if (info.slot == "good") {
            CHECK(info.valid && info.type == "Profile" && info.version == 1 && info.timestamp > 0);
        } else {
            CHECK(!info.valid && info.type.empty() && info.version == 0 && info.timestamp == 0);
        }
    }
    CHECK(slots.size() == 6 && valid == 1);
    save_test::Profile loaded;
    CHECK(saves.Load("good", loaded).ok && SameProfile(loaded, profile));
}

AETHER_TEST(Save_WritesAreAtomicAndLeaveNoTempFile) {
    const stdfs::path dir = Dir("Atomic");
    SaveSystem saves(dir);
    save_test::Profile p = Sample();
    CHECK(saves.Save("a", p).ok && saves.Save("a", p).ok);
    for (const std::string& name : fs::ListDirectory(dir.string())) CHECK(name.find(".tmp") == std::string::npos);
    // The file helper itself: replaces, makes the folder, lists sorted.
    const std::string file = (dir / "sub/x.bin").string();
    const char first[] = "first", second[] = "second one";
    CHECK(fs::WriteFileAtomic(file, first, 5) && fs::WriteFileAtomic(file, second, 10));
    CHECK(ReadAll(file) == "second one" && fs::ListDirectory((dir / "sub").string()) == std::vector<std::string>{"x.bin"});
    CHECK(fs::ListDirectory((dir / "nowhere").string()).empty());
    // A target that can't be replaced (a folder in the way) fails and leaves no temp file.
    stdfs::create_directories(dir / "sub/blocked");
    CHECK(!fs::WriteFileAtomic((dir / "sub/blocked").string(), first, 5));
    CHECK(!stdfs::exists(dir / "sub/blocked.tmp"));
}

AETHER_TEST(Save_KeepsABackupAndRecoversFromADamagedSave) {
    SaveSystem saves(Dir("Backup"));
    save_test::Profile first = Sample();
    CHECK(saves.Save("a", first).ok);
    CHECK(!stdfs::exists(saves.Directory() / "a.asav.bak")); // nothing to back up yet
    save_test::Profile second = first;
    second.level = 8;
    CHECK(saves.Save("a", second).ok && stdfs::exists(saves.Directory() / "a.asav.bak"));
    const auto paired = saves.ListSlots();
    CHECK(paired.size() == 1 && paired.front().slot == "a" && paired.front().valid &&
          paired.front().bytes == fs::FileSize((saves.Directory() / "a.asav").string()));
    save_test::Profile loaded;
    CHECK(saves.Load("a", loaded).ok && loaded.level == 8);
    // The main file is damaged: the previous save loads, with a note.
    WriteAll(saves.Directory() / "a.asav", "{ truncated");
    SaveResult r = saves.Load("a", loaded);
    CHECK(r.ok && loaded.level == 7 && r.warnings.size() == 1 && r.warnings[0].find("previous") != std::string::npos);
    // Gone altogether: the same.
    stdfs::remove(saves.Directory() / "a.asav");
    CHECK(saves.Load("a", loaded).ok && loaded.level == 7);
    CHECK(saves.Exists("a"));
    const auto slots = saves.ListSlots();
    CHECK(slots.size() == 1 && slots.front().slot == "a" && slots.front().valid &&
          slots.front().type == "Profile" && slots.front().bytes > 0);
    // Deleting the slot removes the backup too.
    CHECK(saves.DeleteSlot("a").ok && !stdfs::exists(saves.Directory() / "a.asav.bak"));
    CHECK(!saves.Exists("a") && saves.ListSlots().empty());
}

AETHER_TEST(Save_RepairDoesNotReplaceAHealthyBackupWithDamagedBytes) {
    SaveSystem saves(Dir("KeepBackup"));
    save_test::Profile original = Sample();
    if (!saves.Save("a", original).ok) { CHECK(false); return; }
    auto newer = original;
    newer.level = 9;
    if (!saves.Save("a", newer).ok) { CHECK(false); return; }
    const auto primary = saves.Directory() / "a.asav";
    const auto backup = saves.Directory() / "a.asav.bak";
    const auto healthy = ReadAll(backup);

    auto damaged = reflect::Json::parse(ReadAll(primary));
    damaged["data"]["level"] = 99; // Valid JSON, invalid save checksum.
    WriteAll(primary, damaged.dump());
    save_test::Profile loaded;
    CHECK(saves.Load("a", loaded).ok && loaded.level == original.level);

    auto recovered = original;
    recovered.level = 10;
    CHECK(saves.Save("a", recovered).ok && ReadAll(backup) == healthy);
    CHECK(saves.Load("a", loaded).ok && loaded.level == 10);
    stdfs::remove(primary);
    CHECK(saves.Load("a", loaded).ok && loaded.level == original.level);
}

AETHER_TEST(Save_BlockedBackupRotationPreservesThePreviousPrimary) {
    SaveSystem saves(Dir("BlockedBackupRotation"));
    auto profile = Sample();
    CHECK(saves.Save("a", profile).ok);
    const auto primary = saves.Directory() / "a.asav";
    const auto backup = saves.Directory() / "a.asav.bak";
    const auto previous = ReadAll(primary);
    CHECK(!previous.empty());
    CHECK(stdfs::create_directory(backup));
    WriteAll(backup / "keep.txt", "not a backup");
    profile.level = 11;
    const auto result = saves.Save("a", profile);
    CHECK(!result.ok && result.error == SaveError::IoError && result.message.find("backup") != std::string::npos);
    CHECK(ReadAll(primary) == previous);
    CHECK(ReadAll(backup / "keep.txt") == "not a backup");
    CHECK(!stdfs::exists(saves.Directory() / "a.asav.bak.tmp"));
    save_test::Profile loaded;
    CHECK(saves.Load("a", loaded).ok && loaded.level == Sample().level);
}

AETHER_TEST(Save_DetectsCorruptionAndTruncation) {
    SaveSystem saves(Dir("Corrupt"));
    save_test::Profile p = Sample();
    CHECK(saves.Save("a", p).ok);
    const stdfs::path file = saves.Directory() / "a.asav";
    const std::string good = ReadAll(file);
    // A changed value: the checksum no longer matches.
    std::string edited = good;
    const usize at = edited.find("\"level\": 7");
    CHECK(at != std::string::npos);
    edited.replace(at, 10, "\"level\": 9");
    WriteAll(file, edited);
    save_test::Profile loaded;
    loaded.level = 123;
    SaveResult r = saves.Load("a", loaded);
    CHECK(!r.ok && r.error == SaveError::Corrupt && r.message.find("checksum") != std::string::npos && loaded.level == 123); // untouched
    // Cut short.
    WriteAll(file, good.substr(0, good.size() / 2));
    CHECK(saves.Load("a", loaded).error == SaveError::Corrupt);
    // Not a save at all, valid JSON but the wrong kind, empty.
    WriteAll(file, "[1, 2, 3]");
    CHECK(saves.Load("a", loaded).error == SaveError::Corrupt);
    WriteAll(file, "{\"hello\": 1}");
    CHECK(saves.Load("a", loaded).error == SaveError::Corrupt);
    WriteAll(file, "");
    CHECK(saves.Load("a", loaded).error == SaveError::Corrupt);
    // An unknown envelope format.
    reflect::Json j = reflect::Json::parse(good);
    j["format"] = 99;
    WriteAll(file, j.dump());
    CHECK(saves.Load("a", loaded).error == SaveError::Corrupt);
}

AETHER_TEST(Save_RefusesTheWrongTypeAndNewerVersions) {
    SaveSystem saves(Dir("Types"));
    save_test::Profile p = Sample();
    CHECK(saves.Save("a", p).ok);
    save_test::SaveOther o;
    SaveResult r = saves.Load("a", o);
    CHECK(!r.ok && r.error == SaveError::WrongType && r.message.find("Profile") != std::string::npos && r.message.find("SaveOther") != std::string::npos);
    // A save from a newer version of the struct isn't guessed at, and isn't replaced by the backup.
    WriteAll(saves.Directory() / "future.asav", Envelope("Profile", 9, reflect::Json{{"$v", 9}, {"level", 5}}));
    save_test::Profile loaded;
    r = saves.Load("future", loaded);
    CHECK(!r.ok && r.error == SaveError::FutureVersion && loaded.level == 1);
}

AETHER_TEST(Save_MigratesOlderVersionsAndKeepsDefaultsForNewFields) {
    reflect::RegisterMigration<save_test::SaveFighter>(&save_test::MigrateFighter);
    SaveSystem saves(Dir("Migrate"));
    // Version 1 called it "hp" and had no shield.
    stdfs::create_directories(saves.Directory());
    WriteAll(saves.Directory() / "old.asav", Envelope("SaveFighter", 1, reflect::Json{{"$v", 1}, {"hp", 77.0f}}));
    save_test::SaveFighter f;
    const SaveResult r = saves.Load("old", f);
    CHECK(r.ok && f.health == 77.0f && f.shield == 5.0f); // renamed by the hook; the new field keeps its default
    // Saving again writes the current version.
    CHECK(saves.Save("old", f).ok);
    const reflect::Json j = reflect::Json::parse(ReadAll(saves.Directory() / "old.asav"));
    CHECK(j["type_version"] == 2 && j["data"]["health"] == 77.0f);
    // An unknown field in the data (from another build) is ignored, with a warning.
    WriteAll(saves.Directory() / "extra.asav", Envelope("SaveFighter", 2, reflect::Json{{"$v", 2}, {"health", 1.0f}, {"mystery", 3}}));
    save_test::SaveFighter g;
    CHECK(saves.Load("extra", g).ok && g.health == 1.0f);
}

AETHER_TEST(Save_AsyncSavesCompleteThroughPump) {
    SaveSystem saves(Dir("Async"));
    save_test::Profile p = Sample();
    std::vector<std::string> done;
    saves.SaveAsync("a", p, [&](const SaveResult& r) { done.push_back(r.ok ? "a ok" : "a failed"); });
    p.level = 99; // changed right after the call: the save has the old value
    saves.Flush();
    CHECK(saves.Pending() == 0 && done.empty()); // the callback waits for Pump
    CHECK(saves.Pump() == 1 && done == std::vector<std::string>{"a ok"} && saves.Pump() == 0);
    save_test::Profile loaded;
    CHECK(saves.Load("a", loaded).ok && loaded.level == 7);
    // An invalid slot is reported the same way.
    saves.SaveAsync("../x", p, [&](const SaveResult& r) { done.push_back(r.error == SaveError::InvalidSlot ? "bad slot" : "?"); });
    CHECK(saves.Pump() == 1 && done.back() == "bad slot");
}

AETHER_TEST(Save_AsyncSavesToOneSlotHappenInOrderAndTheQueueDrains) {
    const stdfs::path dir = Dir("AsyncOrder");
    std::vector<int> order;
    {
        SaveSystem saves(dir);
        save_test::Profile p;
        for (int i = 1; i <= 20; ++i) {
            p.level = i;
            saves.SaveAsync("a", p, [&order, i](const SaveResult&) { order.push_back(i); });
        }
        saves.Flush();
        saves.Pump();
        CHECK(order.size() == 20);
        for (usize i = 0; i < order.size(); ++i) CHECK(order[i] == static_cast<int>(i) + 1);
        save_test::Profile loaded;
        CHECK(saves.Load("a", loaded).ok && loaded.level == 20); // the last one wins
        p.level = 21;
        saves.SaveAsync("b", p); // not flushed: the destructor finishes it
    }
    SaveSystem again(dir);
    save_test::Profile loaded;
    CHECK(again.Load("b", loaded).ok && loaded.level == 21);
}

AETHER_TEST(Save_SyncAndAsyncSavesDontCollide) {
    SaveSystem saves(Dir("Mixed"));
    save_test::Profile p = Sample();
    for (int i = 0; i < 10; ++i) {
        p.level = i;
        saves.SaveAsync("a", p);
        CHECK(saves.Save("b", p).ok);
        save_test::Profile loaded;
        CHECK(saves.Load("b", loaded).ok && loaded.level == i);
    }
    saves.Flush();
    save_test::Profile loaded;
    CHECK(saves.Load("a", loaded).ok && loaded.level == 9);
}

AETHER_TEST(Save_ActiveSystemIsClearedOnDestruction) {
    CHECK(SaveSystem::Active() == nullptr);
    {
        SaveSystem saves(Dir("Active"));
        saves.MakeActive();
        CHECK(SaveSystem::Active() == &saves);
    }
    CHECK(SaveSystem::Active() == nullptr);
}

namespace fuzz_save {
struct FuzzProfile {
    i32 level = 1;
    f32 health = 100.0f;
    std::string name = "Hero";
    std::vector<i32> inventory;
    bool hard_mode = false;
};
} // namespace fuzz_save
AETHER_REFLECT(fuzz_save::FuzzProfile, 1, AETHER_FIELD(level, Field_EditAnywhere), AETHER_FIELD(health, Field_EditAnywhere),
               AETHER_FIELD(name, Field_EditAnywhere), AETHER_FIELD(inventory, Field_EditAnywhere), AETHER_FIELD(hard_mode, Field_EditAnywhere))
// The envelope from memory (Phase 47 step 1): a field of the wrong type is a damaged file,
// not an exception.
AETHER_TEST(Save_EnvelopeFromMemoryReadsAndSurvivesWrongTypes) {
    fuzz_save::FuzzProfile written;
    written.level = 7;
    written.inventory = {1, 2, 3};
    const reflect::TypeInfo& info = reflect::Reflect<fuzz_save::FuzzProfile>();
    const std::string text = aether::save::envelope::Make("aether.save", "slot1", info, &written);
    const std::span<const u8> bytes(reinterpret_cast<const u8*>(text.data()), text.size());

    fuzz_save::FuzzProfile read;
    CHECK(aether::save::envelope::ReadFromMemory(bytes, "memory", "aether.save", info, &read).ok);
    CHECK(read.level == 7 && read.inventory == std::vector<i32>({1, 2, 3}));
    const aether::save::envelope::EnvelopeInfo inspected = aether::save::envelope::InspectBytes(bytes);
    CHECK(inspected.ok && inspected.checksum_ok && inspected.type == "FuzzProfile" && inspected.slot == "slot1");

    bool threw = false;
    try {
        for (const char* wrong : {R"({"$type":5,"format":"x","type":[1],"type_version":{},"checksum":7,"data":{}})",
                                  R"({"$type":"aether.save","format":"x","data":{}})", R"({"$type":"aether.save","format":1,"checksum":[],"data":{}})",
                                  R"({"$type":"aether.save","format":1,"checksum":"x","type":3,"data":{}})"}) {
            const std::span<const u8> b(reinterpret_cast<const u8*>(wrong), std::strlen(wrong));
            fuzz_save::FuzzProfile target;
            const aether::save::SaveResult result = aether::save::envelope::ReadFromMemory(b, "memory", "aether.save", info, &target);
            CHECK(!result.ok);
        }
    } catch (...) {
        threw = true;
    }
    CHECK(!threw);
}
