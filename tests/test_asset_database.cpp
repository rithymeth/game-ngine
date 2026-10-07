#include "aether/assets/asset_database.h"
#include "aether/reflection/serialize.h"
#include "test_framework.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>

using namespace aether;
using namespace aether::assets;
namespace stdfs = std::filesystem;

namespace {

stdfs::path FreshContent(const char* name) {
    stdfs::path dir = stdfs::temp_directory_path() / name;
    stdfs::remove_all(dir);
    stdfs::create_directories(dir);
    return dir;
}

void Write(const stdfs::path& file, const std::string& text) {
    stdfs::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << text;
}

std::string ReadText(const stdfs::path& file) {
    std::ifstream f(file);
    return std::string(std::istreambuf_iterator<char>(f), {});
}

} // namespace

AETHER_TEST(AssetGuid_StringAndReflection) {
    AssetGuid g = NewAssetGuid();
    AssetGuid back;
    AETHER_CHECK(!g.IsNull() && ParseAssetGuid(ToString(g), back) && back == g);
    AETHER_CHECK(reflect::ToJson(reflect::Reflect<AssetGuid>(), &g) == ToString(g)); // string via converter
    AETHER_CHECK(ImporterForExtension("a/b/Brick.PNG") == "Texture");
    AETHER_CHECK(ImporterForExtension("hero.glb") == "Model");
    AETHER_CHECK(ImporterForExtension("Audio/alert.ACUE") == "SoundCue");
    AETHER_CHECK(ImporterForExtension("notes.txt").empty());
}

AETHER_TEST(AssetMetaFromMemory_RejectsWrongTypesWithoutThrowing) {
    const std::string valid = R"({"$type":"AssetMeta","guid":"00000000-0000-4000-8000-000000000002","importer_version":7,"importer":"Texture","source_hash":"abc"})";
    AssetMeta meta;
    std::string error;
    AETHER_CHECK(LoadAssetMetaFromMemory(std::span<const u8>(reinterpret_cast<const u8*>(valid.data()), valid.size()), meta, &error));
    AETHER_CHECK(meta.importer == "Texture" && meta.importer_version == 7 && meta.source_hash == "abc");

    const std::string malformed = R"({"$type":"AssetMeta","guid":"00000000-0000-4000-8000-000000000002","importer":{},"importer_version":"new","source_hash":[],"labels":{},"sub_assets":{"mesh:0":{"guid":"00000000-0000-4000-8000-000000000003","importer":false}}})";
    AETHER_CHECK(LoadAssetMetaFromMemory(std::span<const u8>(reinterpret_cast<const u8*>(malformed.data()), malformed.size()), meta, &error));
    AETHER_CHECK(meta.importer.empty() && meta.importer_version == 1 && meta.source_hash.empty());
    AETHER_CHECK(meta.labels.empty() && meta.sub_assets.size() == 1 && meta.sub_assets[0].importer.empty());
}

AETHER_TEST(AssetDatabase_ScanCreatesStableMetadata) {
    stdfs::path root = FreshContent("aether_test_content_scan");
    Write(root / "Textures/brick.png", "fake png bytes");
    Write(root / "Models/hero.gltf", "{}");
    Write(root / "Maps/Level_01.ascene", "{}");
    Write(root / "readme.txt", "not an asset");
    Write(root / ".git/objects/x.png", "hidden: ignored");

    AssetDatabase db(root);
    ScanResult first = db.Scan();
    AETHER_CHECK(first.assets == 3 && first.created_meta == 3 && first.needs_import == 3);
    AETHER_CHECK(first.warnings.empty());
    AETHER_CHECK(stdfs::exists(root / "Textures/brick.png.ameta"));
    AETHER_CHECK(!stdfs::exists(root / "readme.txt.ameta") && !stdfs::exists(root / ".git/objects/x.png.ameta"));

    const AssetRecord* brick = db.FindByPath("Textures/brick.png"); // '/'-separated on every platform
    AETHER_CHECK(brick != nullptr && brick->importer == "Texture" && !brick->missing);
    AETHER_CHECK(db.Find(brick->guid) == brick);
    AssetGuid brick_guid = brick->guid;

    std::string meta_text = ReadText(root / "Textures/brick.png.ameta");
    AETHER_CHECK(meta_text.find("\"$type\": \"AssetMeta\"") != std::string::npos);
    AETHER_CHECK(meta_text.find(ToString(brick_guid)) != std::string::npos);

    // Rescanning changes nothing: same GUIDs, no new files.
    ScanResult second = db.Scan();
    AETHER_CHECK(second.created_meta == 0 && second.assets == 3);
    AETHER_CHECK(db.FindByPath("Textures/brick.png")->guid == brick_guid);
    AETHER_CHECK(ReadText(root / "Textures/brick.png.ameta") == meta_text);

    std::vector<const AssetRecord*> all = db.All();
    AETHER_CHECK(all.size() == 3 && all[0]->path == "Maps/Level_01.ascene" && all[2]->path == "Textures/brick.png");
    stdfs::remove_all(root);
}

AETHER_TEST(AssetDatabase_MoveKeepsIdentity) {
    stdfs::path root = FreshContent("aether_test_content_move");
    Write(root / "brick.png", "pixels");
    AssetDatabase db(root);
    db.Scan();
    AssetGuid guid = db.FindByPath("brick.png")->guid;

    std::string error;
    AETHER_CHECK(db.Move(guid, "Environment/Walls/red_brick.png", &error));
    AETHER_CHECK(stdfs::exists(root / "Environment/Walls/red_brick.png"));
    AETHER_CHECK(stdfs::exists(root / "Environment/Walls/red_brick.png.ameta"));
    AETHER_CHECK(!stdfs::exists(root / "brick.png") && !stdfs::exists(root / "brick.png.ameta"));
    AETHER_CHECK(db.Find(guid)->path == "Environment/Walls/red_brick.png");
    AETHER_CHECK(db.FindByPath("brick.png") == nullptr);

    // A fresh scan (e.g. next editor start) finds the same GUID at the new path.
    AssetDatabase reopened(root);
    AETHER_CHECK(reopened.Scan().created_meta == 0);
    AETHER_CHECK(reopened.FindByPath("Environment/Walls/red_brick.png")->guid == guid);

    // Moving onto an existing asset is refused.
    Write(root / "other.png", "x");
    reopened.Scan();
    AETHER_CHECK(!reopened.Move(guid, "other.png", &error));
    AETHER_CHECK(error.find("already exists") != std::string::npos);
    stdfs::remove_all(root);
}

AETHER_TEST(AssetDatabase_MissingSourcesKeepTheirMetadata) {
    stdfs::path root = FreshContent("aether_test_content_missing");
    Write(root / "sound.wav", "RIFF");
    AssetDatabase db(root);
    db.Scan();
    AssetGuid guid = db.FindByPath("sound.wav")->guid;

    stdfs::path parked = stdfs::temp_directory_path() / "aether_test_parked.wav";
    stdfs::rename(root / "sound.wav", parked); // e.g. switched to a branch without it
    ScanResult gone = db.Scan();
    AETHER_CHECK(gone.missing == 1);
    AETHER_CHECK(db.Find(guid)->missing);
    AETHER_CHECK(stdfs::exists(root / "sound.wav.ameta")); // not deleted

    stdfs::rename(parked, root / "sound.wav"); // it comes back
    ScanResult back = db.Scan();
    AETHER_CHECK(back.missing == 0 && back.created_meta == 0);
    AETHER_CHECK(!db.Find(guid)->missing); // same identity as before
    stdfs::remove_all(root);
}

AETHER_TEST(AssetDatabase_DuplicateGuidsFromCopiedFiles) {
    stdfs::path root = FreshContent("aether_test_content_dup");
    Write(root / "a.png", "one");
    AssetDatabase db(root);
    db.Scan();
    AssetGuid original = db.FindByPath("a.png")->guid;

    // Copy the file *and* its sidecar in a file manager: two assets, one GUID.
    stdfs::copy_file(root / "a.png", root / "a_copy.png");
    stdfs::copy_file(root / "a.png.ameta", root / "a_copy.png.ameta");
    stdfs::last_write_time(root / "a_copy.png.ameta",
                           stdfs::last_write_time(root / "a.png.ameta") + std::chrono::seconds(10));

    ScanResult result = db.Scan();
    AETHER_CHECK(result.duplicates_fixed == 1 && result.warnings.size() == 1);
    AETHER_CHECK(db.FindByPath("a.png")->guid == original); // the older one keeps it
    AssetGuid copy = db.FindByPath("a_copy.png")->guid;
    AETHER_CHECK(copy != original && !copy.IsNull());
    AssetMeta meta;
    AETHER_CHECK(LoadAssetMeta(root / "a_copy.png.ameta", meta) && meta.guid == copy); // persisted
    stdfs::remove_all(root);
}

AETHER_TEST(AssetDatabase_ChangedSourcesNeedImport) {
    stdfs::path root = FreshContent("aether_test_content_changed");
    Write(root / "t.png", "v1");
    AssetDatabase db(root);
    db.Scan();
    AssetGuid guid = db.FindByPath("t.png")->guid;
    AETHER_CHECK(db.Find(guid)->needs_import); // never imported

    std::string error;
    AETHER_CHECK(db.MarkImported(guid, &error));
    AETHER_CHECK(!db.Find(guid)->needs_import);
    AETHER_CHECK(db.Scan().needs_import == 0); // remembered in the .ameta

    Write(root / "t.png", "v2 edited in an image editor");
    AETHER_CHECK(db.Scan().needs_import == 1);
    AETHER_CHECK(db.Find(guid)->needs_import);
    AETHER_CHECK(db.Find(guid)->source_hash == HashFile(root / "t.png"));

    // Importer settings and labels in the .ameta survive MarkImported.
    AssetMeta meta;
    AETHER_CHECK(LoadAssetMeta(root / "t.png.ameta", meta));
    meta.settings["srgb"] = true;
    meta.labels = {"Environment"};
    AETHER_CHECK(SaveAssetMeta(root / "t.png.ameta", meta));
    AETHER_CHECK(db.MarkImported(guid, &error));
    AssetMeta after;
    AETHER_CHECK(LoadAssetMeta(root / "t.png.ameta", after));
    AETHER_CHECK(after.settings["srgb"] == true && after.labels.size() == 1 && after.source_hash == HashFile(root / "t.png"));
    stdfs::remove_all(root);
}

AETHER_TEST(AssetDatabase_BadMetadataIsReportedNotFatal) {
    stdfs::path root = FreshContent("aether_test_content_bad");
    Write(root / "ok.png", "x");
    Write(root / "broken.png", "y");
    Write(root / "broken.png.ameta", "{ not json");
    AssetDatabase db(root);
    ScanResult result = db.Scan();
    AETHER_CHECK(result.warnings.size() == 1);
    // The unreadable sidecar (think: merge conflict markers) is never
    // overwritten — that would lose the asset's GUID. The asset is skipped
    // until it's fixed.
    AETHER_CHECK(result.assets == 1 && result.created_meta == 1);
    AETHER_CHECK(db.FindByPath("broken.png") == nullptr);
    AETHER_CHECK(ReadText(root / "broken.png.ameta") == "{ not json");

    AssetDatabase nowhere(root / "does_not_exist");
    AETHER_CHECK(nowhere.Scan().warnings.size() == 1);
    stdfs::remove_all(root);
}
