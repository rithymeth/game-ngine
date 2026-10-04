#include "aether/cook/cooker.h"
#include "aether/pak/pak.h"
#include "aether/pak/vfs.h"
#include "aether/player/game.h"
#include "aether/project/project.h"
#include "aether/reflection/serialize.h"
#include "aether/scene/gameplay.h"
#include "test_framework.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

// Phase 25 step 6: encrypted archives, patches and DLCs.

using namespace aether;
using namespace aether::pak;
using nlohmann::json;
namespace stdfs = std::filesystem;

#define CHECK(...) AETHER_CHECK((__VA_ARGS__))

namespace {

std::vector<u8> Bytes(const std::string& s) { return {s.begin(), s.end()}; }

std::string Hex(std::span<const u8> bytes) {
    static const char* d = "0123456789abcdef";
    std::string out;
    for (u8 b : bytes) out += {d[b >> 4], d[b & 15]};
    return out;
}

stdfs::path Dir(const std::string& name) {
    const stdfs::path dir = stdfs::temp_directory_path() / "aether_pak_patch_tests" / name;
    stdfs::remove_all(dir);
    stdfs::create_directories(dir);
    return dir;
}

PakKey TestKey() {
    PakKey key;
    AETHER_CHECK(PakKey::FromHex("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f", key));
    return key;
}

json Scene(const std::vector<std::string>& tags) {
    json entities = json::array();
    for (const std::string& t : tags) {
        Tags tag;
        tag.names = {t};
        entities.push_back({{"components", {{"Tags", reflect::ToJson(tag)}}}});
    }
    return {{"$type", "Scene"}, {"$version", 1}, {"entities", entities}};
}

void Write(const stdfs::path& file, const std::string& text) {
    stdfs::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << text;
}

} // namespace

AETHER_TEST(Pak_ChaCha20MatchesRfc8439) {
    // RFC 8439 §2.4.2.
    const PakKey key = TestKey();
    const std::array<u8, 12> nonce = {0, 0, 0, 0, 0, 0, 0, 0x4a, 0, 0, 0, 0};
    std::vector<u8> text = Bytes(
        "Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the future, sunscreen would be it.");
    const std::vector<u8> plain = text;
    ChaCha20Xor(key, nonce, 1, text);
    CHECK(Hex(text) ==
          "6e2e359a2568f98041ba0728dd0d6981e97e7aec1d4360c20a27afccfd9fae0bf91b65c5524733ab8f593dabcd62b3571639d624e6515"
          "2ab8f530c359f0861d807ca0dbf500d6a6156a38e088a22b65e52bc514d16ccf806818ce91ab77937365af90bbf74a35be6b40b8eedf2"
          "785e42874d");
    ChaCha20Xor(key, nonce, 1, text);
    CHECK(text == plain);

    CHECK(key.ToHex() == "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");
    PakKey parsed;
    CHECK(!PakKey::FromHex("00", parsed) && !PakKey::FromHex(std::string(64, 'g'), parsed));
    CHECK(PakKey::FromHex(std::string(64, 'A'), parsed) && parsed.bytes[0] == 0xAA);
    const PakKey a = PakKey::Generate(), b = PakKey::Generate();
    CHECK(a.bytes != b.bytes && a.Id() != b.Id() && key.Id() != 0 && key.Id() == TestKey().Id());
}

AETHER_TEST(Pak_EncryptedArchivesNeedTheirKey) {
    const stdfs::path dir = Dir("Encrypted");
    const std::string secret = "the treasure is under the third palm tree";
    PakWriter writer(CompressionPolicy::None);
    writer.SetEncryption(TestKey());
    writer.Add("Content/secret.txt", secret);
    writer.Add("Content/big.bin", std::vector<u8>(100000, 7)); // compressed, then encrypted
    const std::string file = (dir / "Game.apak").string();
    CHECK(writer.Write(file));

    // The bytes on disk don't show the content (nor the paths, in the index).
    std::ifstream in(file, std::ios::binary);
    const std::string raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(raw.find("treasure") == std::string::npos && raw.find("secret.txt") == std::string::npos);

    PakReader reader;
    std::string error;
    CHECK(!reader.Open(file, &error) && error.find("encrypted") != std::string::npos);
    const std::vector<PakKey> wrong = {PakKey::Generate()};
    CHECK(!reader.Open(file, &error, wrong) && error.find("no key given is its key") != std::string::npos);
    const std::vector<PakKey> keys = {PakKey::Generate(), TestKey()};
    CHECK(reader.Open(file, &error, keys) && reader.IsEncrypted());
    std::string text;
    CHECK(reader.ReadText("Content/secret.txt", text) && text == secret);
    CHECK(reader.Verify().empty());

    VirtualFileSystem vfs;
    CHECK(!vfs.Mount(file, "", 0, &error));
    vfs.AddKey(TestKey());
    CHECK(vfs.Mount(file, "", 0, &error) && vfs.ReadText("Content/secret.txt", text) && text == secret);
}

AETHER_TEST(Pak_PatchesAddChangeAndRemove) {
    const stdfs::path dir = Dir("Patch");
    PakWriter v1;
    v1.Add("a.txt", std::string("one"));
    v1.Add("b.txt", std::string("two"));
    v1.Add("c.txt", std::string("three"));
    CHECK(v1.Write((dir / "Game.apak").string()));
    PakWriter v2;
    v2.Add("a.txt", std::string("one"));      // unchanged
    v2.Add("b.txt", std::string("TWO!"));     // changed
    v2.Add("d.txt", std::string("four"));     // added; c.txt removed
    PakReader base, updated;
    CHECK(base.Open((dir / "Game.apak").string()) && updated.OpenMemory(v2.Build()));
    PakWriter patch;
    const PatchReport report = MakePatch(base, updated, patch);
    CHECK(report.added == std::vector<std::string>{"d.txt"} && report.changed == std::vector<std::string>{"b.txt"});
    CHECK(report.removed == std::vector<std::string>{"c.txt"} && report.unchanged == 1);
    CHECK(patch.EntryCount() == 3); // b, d and the removed list
    CHECK(patch.Write((dir / "Game_p1.apak").string()));

    // Mounted over the base, the patch reads like v2.
    VirtualFileSystem vfs;
    CHECK(vfs.Mount((dir / "Game.apak").string(), "", 0) && vfs.Mount((dir / "Game_p1.apak").string(), "", 1));
    std::string text;
    CHECK(vfs.ReadText("a.txt", text) && text == "one");
    CHECK(vfs.ReadText("b.txt", text) && text == "TWO!");
    CHECK(vfs.ReadText("d.txt", text) && text == "four");
    CHECK(!vfs.Exists("c.txt") && vfs.Resolve("c.txt").empty());
    CHECK(vfs.List() == (std::vector<std::string>{"a.txt", "b.txt", "d.txt"}));
    // A loose folder above the patch can bring a removed file back.
    Write(dir / "loose" / "c.txt", "back");
    CHECK(vfs.Mount((dir / "loose").string(), "", 2) && vfs.ReadText("c.txt", text) && text == "back");
}

AETHER_TEST(Cook_PatchAndDlcArchivesPlayTogether) {
    const stdfs::path parent = Dir("CookPatch");
    ProjectPaths paths;
    std::string error;
    CHECK(CreateProject(parent, "Island", &paths, &error));
    Write(paths.content / "Scenes/main.ascene", Scene({"Hero"}).dump());
    Write(paths.content / "Scenes/old.ascene", Scene({"Old"}).dump());
    Write(paths.content / "DLC/Forest/forest.ascene", Scene({"Tree", "Tree"}).dump());
    ProjectSettings settings;
    CHECK(LoadProject(paths.file, settings, &error));
    settings.startup_scene = "Scenes/main.ascene";
    settings.always_cook = {"Scenes/old.ascene"};
    CHECK(SaveProject(paths.file, settings, &error));
    const stdfs::path out = parent / "Paks";

    // 1.0, encrypted.
    cook::CookOptions base;
    base.project_file = paths.file;
    base.output_dir = out;
    base.encryption_key = TestKey();
    const cook::CookReport v1 = cook::Cook(base);
    CHECK(v1.ok && !v1.is_patch);

    // 1.1: the startup scene changes and old.ascene goes away; the patch
    // holds only that, encrypted with the same key.
    Write(paths.content / "Scenes/main.ascene", Scene({"Hero", "Sidekick"}).dump());
    settings.always_cook.clear();
    CHECK(SaveProject(paths.file, settings, &error));
    cook::CookOptions patch = base;
    patch.patch_base = v1.pak_file;
    patch.pak_name = "Game_p1";
    const cook::CookReport v2 = cook::Cook(patch);
    CHECK(v2.ok && v2.is_patch);
    CHECK(v2.patch.changed == (std::vector<std::string>{"Content/Scenes/main.ascene", "Manifest.json"}));
    CHECK(v2.patch.removed == std::vector<std::string>{"Content/Scenes/old.ascene"} && v2.patch.added.empty());

    // A DLC: its scene, but nothing the base already ships.
    cook::CookOptions dlc = base;
    dlc.pak_name = "DLC_Forest";
    dlc.dlc_name = "Forest";
    dlc.dlc_base = v1.pak_file;
    dlc.always_cook = {"DLC/Forest/", "Scenes/main.ascene"};
    const cook::CookReport forest = cook::Cook(dlc);
    CHECK(forest.ok && forest.in_base == 1 && forest.assets.size() == 1 && forest.assets[0].path == "DLC/Forest/forest.ascene");
    cook::CookOptions empty_dlc = dlc;
    empty_dlc.always_cook.clear();
    CHECK(!cook::Cook(empty_dlc).ok);

    // The game: base + patch + DLC, by name order, with the key.
    player::GamePackage package;
    package.AddKey(TestKey());
    const std::vector<std::string> paks = player::GamePackage::FindPaks(out);
    CHECK(paks.size() == 3);
    for (usize i = 0; i < paks.size(); ++i) CHECK(package.Mount(paks[i], static_cast<int>(i), &error));
    CHECK(package.LoadManifest(&error));
    CHECK(package.Dlcs() == std::vector<std::string>{"Forest"});
    CHECK(package.FindAsset("DLC/Forest/forest.ascene") != nullptr && package.FindAsset("Scenes/old.ascene") == nullptr);
    CHECK(!package.Files().Exists("Content/Scenes/old.ascene"));
    player::Game game(package);
    CHECK(game.LoadStartupScene(&error));
    CHECK(FindEntitiesWithTag(game.GetWorld(), "Sidekick").size() == 1); // the patched scene
    CHECK(game.LoadScene("DLC/Forest/forest.ascene", &error) && FindEntitiesWithTag(game.GetWorld(), "Tree").size() == 2);

    // Without the key, the archives don't mount.
    player::GamePackage locked;
    CHECK(!locked.Mount(paks[0], 0, &error) && error.find("encrypted") != std::string::npos);
}
