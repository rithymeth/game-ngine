#include "aether/assets/asset_handle.h"
#include "aether/assets/hot_reload.h"
#include "aether/assets/texture_importer.h"
#include "test_framework.h"

#include <filesystem>
#include <fstream>

using namespace aether;
using namespace aether::assets;
namespace stdfs = std::filesystem;
using namespace std::chrono_literals;

namespace {

const stdfs::path kRepoTextures = stdfs::path(AETHER_REPO_ASSETS_DIR) / "textures";

stdfs::path FreshFolder(const char* name) {
    stdfs::path dir = stdfs::temp_directory_path() / name;
    stdfs::remove_all(dir);
    stdfs::create_directories(dir);
    return dir;
}

// Writes a file and gives it a distinct modification time, so the change is
// seen even on filesystems with coarse timestamps.
void WriteFile(const stdfs::path& file, const std::string& text, int stamp) {
    stdfs::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << text;
    stdfs::last_write_time(file, stdfs::file_time_type::clock::now() + std::chrono::seconds(stamp));
}

void CopyFile(const stdfs::path& from, const stdfs::path& to, int stamp) {
    stdfs::create_directories(to.parent_path());
    stdfs::copy_file(from, to, stdfs::copy_options::overwrite_existing);
    stdfs::last_write_time(to, stdfs::file_time_type::clock::now() + std::chrono::seconds(stamp));
}

const AssetChange* FindChange(const std::vector<AssetChange>& changes, AssetChange::Kind kind) {
    for (const AssetChange& change : changes) {
        if (change.kind == kind) {
            return &change;
        }
    }
    return nullptr;
}

} // namespace

AETHER_TEST(FileWatcher_DebouncesAndReportsSettledChanges) {
    stdfs::path dir = FreshFolder("aether_test_watcher");
    WriteFile(dir / "existing.txt", "old", 1);
    WriteFile(dir / ".git/HEAD", "ref", 1);
    FileWatcher watcher(dir, 200ms);
    const auto t0 = FileWatcher::Clock::now();
    AETHER_CHECK(watcher.Poll(t0).empty()); // the starting files aren't changes

    WriteFile(dir / "sub/new.txt", "hello", 2);
    AETHER_CHECK(watcher.Poll(t0 + 10ms).empty() && watcher.PendingCount() == 1);
    AETHER_CHECK(watcher.Poll(t0 + 150ms).empty()); // not settled yet
    std::vector<FileChange> added = watcher.Poll(t0 + 210ms);
    AETHER_CHECK(added.size() == 1);
    if (added.size() == 1) {
        AETHER_CHECK(added[0].path == "sub/new.txt" && added[0].kind == FileChange::Kind::Added);
    }

    // Two saves in quick succession are one change, reported 200 ms after the last.
    WriteFile(dir / "existing.txt", "new", 3);
    AETHER_CHECK(watcher.Poll(t0 + 300ms).empty());
    WriteFile(dir / "existing.txt", "newer", 4);
    AETHER_CHECK(watcher.Poll(t0 + 450ms).empty());
    AETHER_CHECK(watcher.Poll(t0 + 600ms).empty()); // 150 ms after the second save
    std::vector<FileChange> modified = watcher.Poll(t0 + 700ms);
    AETHER_CHECK(modified.size() == 1 && modified[0].kind == FileChange::Kind::Modified);

    // Save via temp file + rename: only the original is reported, as Modified.
    WriteFile(dir / "existing.txt.tmp", "atomic", 5);
    AETHER_CHECK(watcher.Poll(t0 + 800ms).empty());
    stdfs::remove(dir / "existing.txt");
    AETHER_CHECK(watcher.Poll(t0 + 850ms).empty());
    stdfs::rename(dir / "existing.txt.tmp", dir / "existing.txt");
    AETHER_CHECK(watcher.Poll(t0 + 1000ms).empty()); // the rename is itself a change: wait again
    std::vector<FileChange> replaced = watcher.Poll(t0 + 1250ms);
    AETHER_CHECK(replaced.size() == 1);
    if (replaced.size() == 1) {
        AETHER_CHECK(replaced[0].path == "existing.txt" && replaced[0].kind == FileChange::Kind::Modified);
    }

    // Removal; hidden folders are ignored; Refresh() swallows a change.
    stdfs::remove(dir / "sub/new.txt");
    WriteFile(dir / ".git/HEAD", "other", 6);
    WriteFile(dir / "mine.txt", "written by us", 6);
    watcher.Refresh("mine.txt");
    std::vector<FileChange> removed = watcher.Poll(t0 + 1300ms);
    AETHER_CHECK(removed.empty() && watcher.PendingCount() == 1);
    removed = watcher.Poll(t0 + 1600ms);
    AETHER_CHECK(removed.size() == 1);
    if (removed.size() == 1) {
        AETHER_CHECK(removed[0].kind == FileChange::Kind::Removed && removed[0].path == "sub/new.txt");
    }

    // Created then deleted before settling: nothing.
    WriteFile(dir / "blip.txt", "x", 7);
    watcher.Poll(t0 + 1700ms);
    stdfs::remove(dir / "blip.txt");
    AETHER_CHECK(watcher.Poll(t0 + 2000ms).empty() && watcher.PendingCount() == 0);
    stdfs::remove_all(dir);
}

AETHER_TEST(AssetStore_HandlesSeeReloadsThroughGenerations) {
    AssetStore<std::string> store;
    const AssetGuid guid = NewAssetGuid();
    AssetHandle<std::string> early = store.Acquire(guid);
    AETHER_CHECK(early.IsValid() && !early.IsLoaded() && early.Generation() == 0 && early.Guid() == guid);

    store.Set(guid, "v1");
    AssetHandle<std::string> late = store.Acquire(guid);
    AETHER_CHECK(*early.Get() == "v1" && early.Generation() == 1 && late.Generation() == 1);

    // A hot reload: both handles see the new data, and the generation tells
    // anything built from the old data to rebuild.
    const u32 built = early.Generation();
    store.Set(guid, "v2");
    AETHER_CHECK(*late.Get() == "v2" && early->size() == 2 && early.Generation() != built);
    AETHER_CHECK(store.Contains(guid) && store.Size() == 1);

    // Dropping it from the store leaves existing handles with the last data.
    store.Remove(guid);
    AETHER_CHECK(!store.Contains(guid) && *early.Get() == "v2");
    AETHER_CHECK(!AssetHandle<std::string>().IsValid() && AssetHandle<std::string>().Get() == nullptr);
}

AETHER_TEST(HotReload_ReimportsEditedTexturesAndTracksFiles) {
    stdfs::path project = FreshFolder("aether_test_hot_reload");
    stdfs::path content = project / "Content";
    CopyFile(kRepoTextures / "checker_a.png", content / "brick.png", 0);
    std::ofstream(content / "level.ascene") << "{}";
    AssetDatabase db(content);
    db.Scan();
    ImporterRegistry importers = ImporterRegistry::WithBuiltins();
    DerivedDataCache cache(project / "Intermediate/DDC");
    ImportAll(db, importers, cache);
    const AssetGuid brick = db.FindByPath("brick.png")->guid;
    std::ofstream(content / "level.ascene") << "{\"tex\": \"" << ToString(brick) << "\"}";
    db.Scan();

    // What the editor keeps: loaded textures behind handles.
    AssetStore<TextureData> textures;
    TextureData first;
    AETHER_CHECK(DecodeTextureData(ImportAsset(db, brick, importers, cache).data, first));
    textures.Set(brick, first);
    AssetHandle<TextureData> handle = textures.Acquire(brick);

    HotReloader reloader(db, importers, cache, 200ms);
    auto t = FileWatcher::Clock::now();
    AETHER_CHECK(reloader.Update(t).empty());

    // An artist saves a new version of the texture.
    CopyFile(kRepoTextures / "checker_b.png", content / "brick.png", 10);
    AETHER_CHECK(reloader.Update(t += 50ms).empty()); // debouncing
    std::vector<AssetChange> changes = reloader.Update(t += 250ms);
    AETHER_CHECK(changes.size() == 1);
    if (changes.size() != 1) {
        stdfs::remove_all(project);
        return;
    }
    const AssetChange& edit = changes[0];
    AETHER_CHECK(edit.kind == AssetChange::Kind::Reimported && edit.guid == brick && edit.output.ok);
    AETHER_CHECK(!edit.output.from_cache);
    AETHER_CHECK(edit.dependents == std::vector<AssetGuid>{db.FindByPath("level.ascene")->guid});
    TextureData second;
    AETHER_CHECK(DecodeTextureData(edit.output.data, second));
    textures.Set(brick, second);
    AETHER_CHECK(handle.Generation() == 2 && handle->mips[0] != first.mips[0]);

    // Our own .ameta write during the import doesn't come back as a change.
    AETHER_CHECK(reloader.Update(t += 500ms).empty());
    AETHER_CHECK(reloader.Update(t += 500ms).empty());

    // Editing import settings in the .ameta reimports; rewriting the same
    // settings (as MarkImported does) doesn't.
    AssetMeta meta;
    AETHER_CHECK(LoadAssetMeta(db.MetaPath(brick), meta));
    meta.settings["generate_mips"] = false;
    AETHER_CHECK(SaveAssetMeta(db.MetaPath(brick), meta));
    stdfs::last_write_time(db.MetaPath(brick), stdfs::file_time_type::clock::now() + 20s);
    reloader.Update(t += 10ms);
    changes = reloader.Update(t += 300ms);
    TextureData no_mips;
    AETHER_CHECK(changes.size() == 1 && changes[0].kind == AssetChange::Kind::Reimported);
    AETHER_CHECK(changes.size() == 1 && DecodeTextureData(changes[0].output.data, no_mips) && no_mips.mips.size() == 1);
    AETHER_CHECK(SaveAssetMeta(db.MetaPath(brick), meta));
    stdfs::last_write_time(db.MetaPath(brick), stdfs::file_time_type::clock::now() + 30s);
    reloader.Update(t += 10ms);
    AETHER_CHECK(reloader.Update(t += 300ms).empty());

    // A new file is added (and imported); a broken one fails but is reported.
    CopyFile(kRepoTextures / "checker_a.png", content / "props/crate.png", 40);
    WriteFile(content / "props/broken.png", "not a png", 40);
    reloader.Update(t += 10ms);
    changes = reloader.Update(t += 300ms);
    const AssetChange* added = FindChange(changes, AssetChange::Kind::Added);
    const AssetChange* failed = FindChange(changes, AssetChange::Kind::Failed);
    AETHER_CHECK(changes.size() == 2 && added != nullptr && failed != nullptr);
    AETHER_CHECK(added->path == "props/crate.png" && added->output.ok);
    AETHER_CHECK(failed->path == "props/broken.png" && !failed->output.error.empty());
    AETHER_CHECK(reloader.Update(t += 500ms).empty()); // the new .ameta files aren't changes

    // Renamed outside the editor, together with its .ameta: a move, same GUID.
    stdfs::create_directories(content / "walls");
    stdfs::rename(content / "brick.png", content / "walls/brick.png");
    stdfs::rename(content / "brick.png.ameta", content / "walls/brick.png.ameta");
    reloader.Update(t += 10ms);
    changes = reloader.Update(t += 300ms);
    const AssetChange* moved = FindChange(changes, AssetChange::Kind::Moved);
    AETHER_CHECK(changes.size() == 1 && moved != nullptr && moved->guid == brick);
    AETHER_CHECK(moved->old_path == "brick.png" && moved->path == "walls/brick.png");
    AETHER_CHECK(moved->dependents.size() == 1);

    // Deleted: reported missing, and imported again when it comes back.
    stdfs::remove(content / "walls/brick.png");
    reloader.Update(t += 10ms);
    changes = reloader.Update(t += 300ms);
    AETHER_CHECK(changes.size() == 1 && changes[0].kind == AssetChange::Kind::Missing && changes[0].guid == brick);
    CopyFile(kRepoTextures / "checker_b.png", content / "walls/brick.png", 50);
    reloader.Update(t += 10ms);
    changes = reloader.Update(t += 300ms);
    AETHER_CHECK(changes.size() == 1 && changes[0].kind == AssetChange::Kind::Reimported);
    AETHER_CHECK(changes.size() == 1 && changes[0].guid == brick && changes[0].output.ok);

    // The broken file was reported once, not on every change since; fixing it
    // imports it.
    CopyFile(kRepoTextures / "test_corners.png", content / "props/broken.png", 60);
    reloader.Update(t += 10ms);
    changes = reloader.Update(t += 300ms);
    AETHER_CHECK(changes.size() == 1 && changes[0].kind == AssetChange::Kind::Reimported);
    AETHER_CHECK(changes.size() == 1 && changes[0].path == "props/broken.png" && changes[0].output.ok);
    stdfs::remove_all(project);
}
