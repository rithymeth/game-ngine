#include "aether/assets/image.h"
#include "aether/assets/importer.h"
#include "aether/assets/texture_importer.h"
#include "test_framework.h"

#include <atomic>
#include <filesystem>
#include <fstream>

using namespace aether;
using namespace aether::assets;
namespace stdfs = std::filesystem;

namespace {

const stdfs::path kRepoTextures = stdfs::path(AETHER_REPO_ASSETS_DIR) / "textures";

stdfs::path FreshProject(const char* name) {
    stdfs::path dir = stdfs::temp_directory_path() / name;
    stdfs::remove_all(dir);
    stdfs::create_directories(dir / "Content");
    return dir;
}

// Counts how often it actually runs, to prove cache hits skip the work.
class CountingImporter final : public IAssetImporter {
public:
    explicit CountingImporter(std::atomic<int>& runs) : runs_(runs) {}
    const char* Name() const override { return "Texture"; }
    u32 Version() const override { return 7; }
    nlohmann::json DefaultSettings() const override { return {{"quality", 1}}; }
    ImportResult Import(const ImportContext& context) const override {
        ++runs_;
        ImportResult r;
        r.ok = true;
        std::string tag = "q=" + context.settings["quality"].dump();
        r.data.assign(tag.begin(), tag.end());
        return r;
    }

private:
    std::atomic<int>& runs_;
};

} // namespace

AETHER_TEST(TextureImporter_DownsampleIsGammaCorrect) {
    // A black and a white pixel side by side, over one row.
    std::vector<u8> pixels = {0, 0, 0, 255, 255, 255, 255, 255};
    std::vector<u8> srgb = DownsampleRGBA8(pixels, 2, 1, /*srgb=*/true);
    std::vector<u8> linear = DownsampleRGBA8(pixels, 2, 1, /*srgb=*/false);
    AETHER_CHECK(srgb.size() == 4 && linear.size() == 4);
    // Half the light is sRGB ~188, not 128: averaging encoded values would darken it.
    AETHER_CHECK(srgb[0] == 188 && srgb[1] == 188 && srgb[2] == 188);
    AETHER_CHECK(linear[0] == 128);
    AETHER_CHECK(srgb[3] == 255 && linear[3] == 255); // alpha is always linear
}

AETHER_TEST(TextureImporter_BuildsMipChainFromARealPng) {
    std::vector<std::string> warnings;
    nlohmann::json settings = TextureImporter().DefaultSettings();
    AssetRecord record;
    record.path = "checker_a.png";
    TextureImporter importer;
    ImportResult result = importer.Import({record, kRepoTextures / "checker_a.png", settings});
    AETHER_CHECK(result.ok && result.warnings.empty());

    TextureData texture;
    AETHER_CHECK(DecodeTextureData(result.data, texture));
    AETHER_CHECK(texture.width == 64 && texture.height == 64 && texture.srgb);
    AETHER_CHECK(texture.mips.size() == 7); // 64, 32, 16, 8, 4, 2, 1
    AETHER_CHECK(texture.mips.back().size() == 4);
    AETHER_CHECK(texture.mips[1].size() == 32u * 32u * 4u);

    // The 2x2 corners texture: its 1x1 mip is the (linear-space) average.
    ImageData corners;
    AETHER_CHECK(DecodeImageFile((kRepoTextures / "test_corners.png").string(), corners));
    ImportResult small = importer.Import({record, kRepoTextures / "test_corners.png", settings});
    TextureData tiny;
    AETHER_CHECK(small.ok && DecodeTextureData(small.data, tiny) && tiny.mips.size() == 2);
    AETHER_CHECK(tiny.mips[1] == DownsampleRGBA8(corners.pixels, 2, 2, true));
}

AETHER_TEST(TextureImporter_SettingsChangeTheOutput) {
    AssetRecord record;
    TextureImporter importer;
    nlohmann::json settings = {{"srgb", false}, {"generate_mips", false}, {"max_size", 16}};
    ImportResult result = importer.Import({record, kRepoTextures / "checker_a.png", settings});
    TextureData texture;
    AETHER_CHECK(result.ok && DecodeTextureData(result.data, texture));
    AETHER_CHECK(texture.width == 16 && texture.height == 16); // halved until it fits
    AETHER_CHECK(texture.mips.size() == 1 && !texture.srgb);

    nlohmann::json wrong = {{"srgb", "yes"}, {"max_size", "big"}};
    ImportResult tolerant = importer.Import({record, kRepoTextures / "checker_a.png", wrong});
    AETHER_CHECK(tolerant.ok && tolerant.warnings.size() == 2); // defaults used

    ImportResult bad = importer.Import({record, kRepoTextures / "does_not_exist.png", settings});
    AETHER_CHECK(!bad.ok && !bad.error.empty());

    TextureData ignored;
    AETHER_CHECK(!DecodeTextureData({1, 2, 3}, ignored));
    std::vector<u8> truncated = result.data;
    truncated.pop_back();
    AETHER_CHECK(!DecodeTextureData(truncated, ignored));
}

AETHER_TEST(Import_CachesByContentSettingsAndVersion) {
    stdfs::path project = FreshProject("aether_test_import_cache");
    stdfs::copy_file(kRepoTextures / "checker_a.png", project / "Content/checker.png");
    AssetDatabase db(project / "Content");
    db.Scan();
    AssetGuid guid = db.FindByPath("checker.png")->guid;

    std::atomic<int> runs{0};
    ImporterRegistry importers;
    importers.Register(std::make_unique<CountingImporter>(runs));
    DerivedDataCache cache(project / "Intermediate/DDC");

    ImportOutput first = ImportAsset(db, guid, importers, cache);
    AETHER_CHECK(first.ok && !first.from_cache && runs == 1);
    AETHER_CHECK(std::string(first.data.begin(), first.data.end()) == "q=1"); // default settings used
    AETHER_CHECK(!db.Find(guid)->needs_import);
    AssetMeta meta;
    AETHER_CHECK(LoadAssetMeta(db.MetaPath(guid), meta) && meta.importer_version == 7);

    // Same input again (e.g. the next editor session): served from the cache.
    ImportOutput second = ImportAsset(db, guid, importers, cache);
    AETHER_CHECK(second.ok && second.from_cache && runs == 1 && second.data == first.data);

    // Changing a setting in the .ameta is a different input.
    meta.settings["quality"] = 3;
    AETHER_CHECK(SaveAssetMeta(db.MetaPath(guid), meta));
    ImportOutput third = ImportAsset(db, guid, importers, cache);
    AETHER_CHECK(third.ok && !third.from_cache && runs == 2);
    AETHER_CHECK(std::string(third.data.begin(), third.data.end()) == "q=3");
    // ...and changing it back hits the cache again: nothing re-imported.
    meta.settings["quality"] = 1;
    AETHER_CHECK(SaveAssetMeta(db.MetaPath(guid), meta));
    AETHER_CHECK(ImportAsset(db, guid, importers, cache).from_cache && runs == 2);

    // A different target platform is a different entry.
    AETHER_CHECK(!ImportAsset(db, guid, importers, cache, "android").from_cache && runs == 3);

    // Editing the source file invalidates it.
    std::ofstream(project / "Content/checker.png", std::ios::app) << "extra";
    db.Scan();
    AETHER_CHECK(db.Find(guid)->needs_import);
    AETHER_CHECK(!ImportAsset(db, guid, importers, cache).from_cache && runs == 4);
    AETHER_CHECK(cache.Hits() == 2);
    stdfs::remove_all(project);
}

AETHER_TEST(Import_AllPendingWithTheRealTextureImporter) {
    stdfs::path project = FreshProject("aether_test_import_all");
    stdfs::copy_file(kRepoTextures / "checker_a.png", project / "Content/a.png");
    stdfs::copy_file(kRepoTextures / "checker_b.png", project / "Content/b.png");
    std::ofstream(project / "Content/broken.png") << "not a png";
    std::ofstream(project / "Content/hero.gltf") << "{}"; // no Model importer yet
    AssetDatabase db(project / "Content");
    db.Scan();

    ImporterRegistry importers = ImporterRegistry::WithBuiltins();
    AETHER_CHECK(importers.Find("Texture") != nullptr && importers.Find("Model") == nullptr);
    DerivedDataCache cache(project / "Intermediate/DDC");
    ImportAllResult result = ImportAll(db, importers, cache);
    AETHER_CHECK(result.imported == 2 && result.failed == 1 && result.skipped == 1 && result.from_cache == 0);
    AETHER_CHECK(result.errors.size() == 1 && result.errors[0].find("broken.png") != std::string::npos);

    // A second pass has nothing to do; after wiping the .ameta hashes (as a
    // fresh clone would have), everything comes from the cache.
    AETHER_CHECK(ImportAll(db, importers, cache).imported == 0);
    for (const char* name : {"a.png", "b.png"}) {
        AssetMeta meta;
        stdfs::path meta_path = project / "Content" / (std::string(name) + ".ameta");
        AETHER_CHECK(LoadAssetMeta(meta_path, meta));
        meta.source_hash.clear();
        AETHER_CHECK(SaveAssetMeta(meta_path, meta));
    }
    db.Scan();
    ImportAllResult again = ImportAll(db, importers, cache);
    AETHER_CHECK(again.from_cache == 2 && again.imported == 0);

    // DDC keys depend on every input.
    std::string k = DerivedDataCache::MakeKey("Texture", 1, "h", "{}", "default");
    AETHER_CHECK(k.size() == 32);
    AETHER_CHECK(k == DerivedDataCache::MakeKey("Texture", 1, "h", "{}", "default"));
    AETHER_CHECK(k != DerivedDataCache::MakeKey("Texture", 2, "h", "{}", "default"));
    AETHER_CHECK(k != DerivedDataCache::MakeKey("Texture", 1, "h", "{}", "android"));
    AETHER_CHECK(DerivedDataCache::MakeKey("ab", 1, "c", "", "") != DerivedDataCache::MakeKey("a", 1, "bc", "", ""));
    stdfs::remove_all(project);
}
