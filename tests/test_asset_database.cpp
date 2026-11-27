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

AETHER_TEST(AssetGuid_DerivedSubAssetGuidsAreStableAndDistinct) {
    AssetGuid parent;
    AETHER_CHECK(ParseAssetGuid("357e2f58-fe46-4aaf-b2fb-4253e498ee34", parent));
    const AssetGuid mesh = DeriveSubAssetGuid(parent, "Mesh", "mesh:0");
    // Pinned: the derivation is part of the content format (fresh cooks on any
    // machine must agree), so changing it is a breaking change.
    AETHER_CHECK(ToString(mesh) == "681c4446-4106-83e0-a4fd-dc5a4d52e955");
    AETHER_CHECK(mesh == DeriveSubAssetGuid(parent, "Mesh", "mesh:0"));
    AETHER_CHECK((mesh.hi & 0xF000) == 0x8000 && (mesh.lo >> 62) == 2); // v8, RFC variant
    AssetGuid other_parent = parent;
    other_parent.lo ^= 1;
    std::set<std::string> seen = {ToString(mesh)};
    seen.insert(ToString(DeriveSubAssetGuid(parent, "Mesh", "mesh:1")));
    seen.insert(ToString(DeriveSubAssetGuid(parent, "Material", "mesh:0")));
    seen.insert(ToString(DeriveSubAssetGuid(parent, "Mesh", "material:0")));
    seen.insert(ToString(DeriveSubAssetGuid(other_parent, "Mesh", "mesh:0")));
    seen.insert(ToString(DeriveSubAssetGuid(parent, "Mes", "hmesh:0"))); // length-prefixed fields
    AETHER_CHECK(seen.size() == 6);
}

AETHER_TEST(AssetGuid_StringAndReflection) {
    AssetGuid g = NewAssetGuid();
    AssetGuid back;
    AETHER_CHECK(!g.IsNull() && ParseAssetGuid(ToString(g), back) && back == g);
    AETHER_CHECK(reflect::ToJson(reflect::Reflect<AssetGuid>(), &g) == ToString(g)); // string via converter
    AETHER_CHECK(ImporterForExtension("a/b/Brick.PNG") == "Texture");
    AETHER_CHECK(ImporterForExtension("hero.glb") == "Model");
    AETHER_CHECK(ImporterForExtension("Audio/alert.ACUE") == "SoundCue");
    AETHER_CHECK(ImporterForExtension("UI/Hud.aui") == "UILayout" && ImporterForExtension("UI/Dark.ATHEME") == "UITheme");
    AETHER_CHECK(ImporterForExtension("Fx/Sparks.avfx") == "ParticleSystem");
    AETHER_CHECK(ImporterForExtension("Graphs/M_Lit.amat") == "Material" && ImporterForExtension("AI/BT_Guard.abt") == "BehaviorTree");
    AETHER_CHECK(ImporterForExtension("World/Hills.aterrain") == "Terrain" && ImporterForExtension("World/Forest.afoliage") == "Foliage");
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

AETHER_TEST(AssetDatabase_SavingUnchangedMetadataLeavesTheFileAlone) {
    // A cook saves the metadata of every imported asset, with or without changes.
    // Writing identical bytes back changes the file's modified time and, on a
    // checkout with CRLF endings, leaves a freshly cooked project showing every
    // sidecar as modified. A save that changes nothing must not touch the file.
    const stdfs::path root = FreshContent("aether_test_meta_idempotent");
    Write(root / "t.png", "x");
    AssetDatabase db(root);
    AETHER_CHECK(db.Scan().created_meta == 1);
    AssetMeta meta;
    AETHER_CHECK(LoadAssetMeta(root / "t.png.ameta", meta));

    // Give the sidecar the CRLF endings a Windows checkout has. Read it back
    // in binary: the text-mode helper would strip the CRs it is testing for.
    const auto read_exact = [](const stdfs::path& file) {
        std::ifstream in(file, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), {});
    };
    const std::string lf = read_exact(root / "t.png.ameta");
    std::string crlf;
    for (const char c : lf) {
        if (c == '\n') crlf += '\r';
        crlf += c;
    }
    Write(root / "t.png.ameta", crlf);
    AETHER_CHECK(read_exact(root / "t.png.ameta") == crlf);

    // The same metadata: the file is left exactly as it was, endings included.
    AETHER_CHECK(SaveAssetMeta(root / "t.png.ameta", meta));
    AETHER_CHECK(read_exact(root / "t.png.ameta") == crlf);

    // Different metadata: it is written.
    meta.labels = {"Changed"};
    AETHER_CHECK(SaveAssetMeta(root / "t.png.ameta", meta));
    AETHER_CHECK(read_exact(root / "t.png.ameta") != crlf);
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

// Phase 43 step 6: the editor's asset validation reports what is wrong without importing or
// opening anything -- assets whose source has gone, and references to GUIDs that no longer
// exist. It reports rather than repairs, so a person decides what to do.
AETHER_TEST(AssetDatabase_ValidateReportsMissingSources) {
    stdfs::path root = FreshContent("aether_test_content_validate");
    Write(root / "Textures/brick.png", "fake png bytes");
    Write(root / "Models/hero.gltf", "{}");
    AssetDatabase db(root);
    AETHER_CHECK(db.Scan().assets == 2);
    AETHER_CHECK(ValidateAssets(db).empty()); // a healthy project reports nothing

    // A source that goes away leaves its .ameta behind; the scan marks the record missing
    // and validation is what turns that into something a person can act on.
    stdfs::remove(root / "Textures/brick.png");
    db.Scan();
    const std::vector<AssetIssue> issues = ValidateAssets(db);
    AETHER_CHECK(issues.size() == 1);
    AETHER_CHECK(issues[0].kind == AssetIssueKind::MissingSource);
    AETHER_CHECK(issues[0].path == "Textures/brick.png");
    AETHER_CHECK(db.Find(issues[0].guid) != nullptr); // the record is still there, its source is not

    stdfs::remove_all(root);
}

AETHER_TEST(AssetDatabase_ReportsUnknownTyped2DDependencies) {
    const auto root = FreshContent("aether_test_unknown_2d");
    const AssetGuid texture = NewAssetGuid(), tileset = NewAssetGuid(), decoy = NewAssetGuid();
    using nlohmann::json;
    Write(root / "Hero.aatlas", json({{"$type", "SpriteAtlas"}, {"texture", ToString(texture)},
          {"description", ToString(decoy)}}).dump());
    Write(root / "Terrain.atileset", json({{"$type", "Tileset"}, {"texture", ToString(texture)}}).dump());
    Write(root / "World.atilemap", json({{"$type", "Tilemap"}, {"tileset", ToString(tileset)}}).dump());
    Write(root / "Placeholder.aatlas", json({{"$type", "SpriteAtlas"}, {"texture", ToString(AssetGuid{})}}).dump());
    Write(root / "WrongType.aatlas", json({{"$type", "Notes"}, {"texture", ToString(decoy)}}).dump());
    AssetDatabase db(root);
    db.Scan();
    const auto issues = ValidateAssets(db);
    AETHER_CHECK(issues.size() == 3);
    if (issues.size() != 3) return;
    AETHER_CHECK(issues[0].path == "Hero.aatlas" && issues[0].dependency == texture);
    AETHER_CHECK(issues[1].path == "Terrain.atileset" && issues[1].dependency == texture);
    AETHER_CHECK(issues[2].path == "World.atilemap" && issues[2].dependency == tileset);
    for (const auto& issue : issues) AETHER_CHECK(issue.kind == AssetIssueKind::BrokenDependency);
}

AETHER_TEST(AssetDatabase_TilemapScanKeepsOnlyEffectiveJsonReferences) {
    const auto root = FreshContent("aether_test_tilemap_dependency_scan");
    Write(root / "Nested.png", "texture source");
    Write(root / "Stale.png", "texture source");
    AssetDatabase db(root);
    db.Scan();
    const auto* nested = db.FindByPath("Nested.png");
    const auto* stale = db.FindByPath("Stale.png");
    if (!nested || !stale) { AETHER_CHECK(false); return; }
    const AssetGuid nested_guid = nested->guid, stale_guid = stale->guid;
    const AssetGuid tileset = NewAssetGuid(), overwritten = NewAssetGuid();
    const auto map = [](const AssetGuid& guid) { return R"({"$type":"Tilemap","tileset":")" + ToString(guid) + '"'; };
    Write(root / "World.atilemap", map(tileset) +
          R"(,"layers":[{"cells":[-1,4,1.5,[[7,8],{"name":"Nested.png","old":"Stale.png","old":0}]]}]})");
    Write(root / "Duplicate.atilemap", map(overwritten) + R"(,"tileset":null})");
    Write(root / "Malformed.atilemap", map(tileset) + R"(,"items":["Nested.png",")" +
          ToString(stale_guid) + R"(",1]} trailing)");
    db.Scan();

    const auto dependencies = [&](const char* path) {
        const auto* record = db.FindByPath(path);
        AETHER_CHECK(record != nullptr);
        return record ? record->dependencies : std::vector<AssetGuid>{};
    };
    const auto world_deps = dependencies("World.atilemap");
    AETHER_CHECK(std::set<AssetGuid>(world_deps.begin(), world_deps.end()) ==
                 (std::set<AssetGuid>{nested_guid, tileset}));
    AETHER_CHECK(dependencies("Duplicate.atilemap").empty());
    // The old raw known-GUID pass still works on malformed JSON. Explicit
    // missing links and textual asset paths require a valid parsed document.
    AETHER_CHECK(dependencies("Malformed.atilemap") == std::vector<AssetGuid>{stale_guid});
    stdfs::remove_all(root);
}

AETHER_TEST(AssetDatabase_RawGuidScanFindsAdjacentAndFinalTokens) {
    const auto root = FreshContent("aether_test_raw_guid_scan");
    Write(root / "First.png", "texture source");
    Write(root / "Last.png", "texture source");
    Write(root / "Unreferenced.png", "texture source");
    AssetDatabase db(root);
    db.Scan();
    const auto* first = db.FindByPath("First.png");
    const auto* last = db.FindByPath("Last.png");
    const auto* unreferenced = db.FindByPath("Unreferenced.png");
    if (!first || !last || !unreferenced) { AETHER_CHECK(false); return; }
    const AssetGuid first_guid = first->guid, last_guid = last->guid;
    std::string malformed = ToString(unreferenced->guid);
    malformed[0] = 'g'; // all separator positions match, but it's not hex
    std::string upper = ToString(first_guid);
    for (char& ch : upper) {
        if (ch >= 'a' && ch <= 'f') ch = static_cast<char>(ch - 'a' + 'A');
    }
    // Raw binary/text fallback must see GUIDs without a JSON string boundary,
    // including an embedded NUL before the tokens and no suffix after the last.
    const std::string bytes = malformed + '\0' + upper + ToString(last_guid);
    Write(root / "Raw.ascene", bytes);
    Write(root / "Raw.atilemap", bytes);
    Write(root / "Exact.atilemap", ToString(last_guid));
    db.Scan();
    for (const char* path : {"Raw.ascene", "Raw.atilemap"}) {
        const auto* record = db.FindByPath(path);
        AETHER_CHECK(record && std::set<AssetGuid>(record->dependencies.begin(), record->dependencies.end()) ==
                     (std::set<AssetGuid>{first_guid, last_guid}));
    }
    const auto* exact = db.FindByPath("Exact.atilemap");
    AETHER_CHECK(exact && exact->dependencies == std::vector<AssetGuid>{last_guid});
    stdfs::remove_all(root);
}

AETHER_TEST(AssetDatabase_Reports2DReferencesToMissingSourcesWithMetadata) {
    const auto root = FreshContent("aether_test_missing_referenced_sheet");
    using nlohmann::json;
    Write(root / "Sheet.png", "texture source");
    AssetDatabase db(root);
    db.Scan();
    const auto* sheet = db.FindByPath("Sheet.png");
    AETHER_CHECK(sheet != nullptr);
    if (!sheet) return;
    const AssetGuid texture = sheet->guid;
    Write(root / "Atlas.aatlas", json({{"$type", "SpriteAtlas"}, {"texture", ToString(texture)}}).dump());
    db.Scan();
    AETHER_CHECK(ValidateAssets(db).empty());
    stdfs::remove(root / "Sheet.png");
    db.Scan();
    const auto* missing = db.Find(texture);
    AETHER_CHECK(missing && missing->missing);
    const auto issues = ValidateAssets(db);
    AETHER_CHECK(issues.size() == 2);
    if (issues.size() == 2) {
        AETHER_CHECK(issues[0].path == "Atlas.aatlas" && issues[0].kind == AssetIssueKind::BrokenDependency &&
                     issues[0].dependency == texture);
        AETHER_CHECK(issues[1].path == "Sheet.png" && issues[1].kind == AssetIssueKind::MissingSource);
    }
    Write(root / "Sheet.png", "restored texture source");
    db.Scan();
    AETHER_CHECK(ValidateAssets(db).empty());
}

// A material graph names its textures by asset GUID inside the file, so nothing else would follow them: the cook used to
// carry them only because a .amat was not an asset and its roots were handled as plain files. Now that the Content
// Browser lists graphs, the scan has to record the references a graph makes.
AETHER_TEST(AssetDatabase_AMaterialGraphsTexturesAreDependencies) {
    stdfs::path root = FreshContent("aether_test_material_dependencies");
    Write(root / "Textures/sky.png", "fake png bytes");
    AssetDatabase db(root);
    AETHER_CHECK(db.Scan().assets == 1);
    const AssetGuid sky = db.FindByPath("Textures/sky.png")->guid;
    Write(root / "Materials/Sky.amat", "{\"parameters\":[{\"name\":\"Albedo\",\"type\":\"texture\",\"texture\":\"" + ToString(sky) + "\"}]}");
    db.Scan();
    const AssetRecord* material = db.FindByPath("Materials/Sky.amat");
    AETHER_CHECK(material != nullptr && material->importer == "Material");
    if (material != nullptr) AETHER_CHECK(material->dependencies == std::vector<AssetGuid>{sky});
    stdfs::remove_all(root);
}
