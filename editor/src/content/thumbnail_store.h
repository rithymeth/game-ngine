#pragma once

#include "aether/assets/asset_database.h"
#include "aether/assets/content_browser.h"
#include "aether/core/base.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace aether::editor {

// The Content Browser's grid pictures: up to 96 px thumbnails of the assets that have one (textures and materials, through
// AssetThumbnail), rendered on a worker thread so a folder full of new textures never stalls a frame. A picture is remembered in
// memory and on disk under Saved/Thumbnails, keyed by the asset and by a stamp of its source (size and modified time), its .ameta
// and its dependencies' source hashes. Reopening the project, or showing a folder twice, decodes the disk copy instead of importing
// the asset again; a changed source gets a new stamp and is rendered once more.
class ThumbnailStore {
public:
    static constexpr u32 kSize = 96;

    explicit ThumbnailStore(std::filesystem::path project_file);
    ~ThumbnailStore();
    ThumbnailStore(const ThumbnailStore&) = delete;
    ThumbnailStore& operator=(const ThumbnailStore&) = delete;

    // The database the entries come from. The panel sets it after every scan; jobs keep it alive while they run, so a rescan can
    // replace it under them without a lock.
    void SetDatabase(std::shared_ptr<assets::AssetDatabase> database);

    // `entry`'s picture and the key that identifies the version of it. The picture is null while it is queued or being rendered,
    // and for kinds that have none (models, scripts, scenes, folders). The first call for an unchanged asset with a disk copy
    // decodes it here, on the calling thread.
    struct Picture {
        const assets::Thumbnail* image = nullptr;
        std::string key; // compare with the one a texture was uploaded from; a changed key means new pixels
    };
    Picture Get(const assets::ContentEntry& entry);
    // Moves finished pictures into the memory cache. Called once a frame by the browser.
    void Pump();
    // Waits for the queue to drain, then takes the results. For tests and a clean shutdown; never call it from the worker.
    void Wait();

    // What happened this session, for the panel's status line and for tests.
    usize Rendered() const { return rendered_.load(); }
    usize LoadedFromDisk() const { return loaded_.load(); }
    usize WithoutPicture() const { return without_.load(); }
    usize Queued() const;

private:
    struct Entry {
        std::string key;
        std::optional<assets::Thumbnail> picture;
    };
    struct Job {
        std::shared_ptr<assets::AssetDatabase> database;
        assets::AssetGuid guid;
        std::string key;
    };
    struct Done {
        std::string guid;
        std::string key;
        std::optional<assets::Thumbnail> picture;
    };

    std::string KeyFor(const assets::AssetDatabase& database, const assets::ContentEntry& entry);
    // The .ameta's import settings as a number: changes when a setting changes, and not when an import merely rewrites the
    // sidecar's source hash. A no-op import (the DDC hit) still writes the file, which is why its timestamp cannot be the key.
    u64 MetaStamp(const assets::AssetDatabase& database, const assets::AssetGuid& guid);
    std::filesystem::path PathFor(const std::string& guid, const std::string& key) const;
    void Work();
    void Store(const Job& job, const assets::Thumbnail& picture);

    std::filesystem::path content_;
    std::filesystem::path thumbnails_;
    std::filesystem::path ddc_;
    std::shared_ptr<assets::AssetDatabase> database_;      // UI thread
    std::shared_ptr<assets::AssetDatabase> jobs_database_; // the worker's own copy: importing sub-assets must not race the panel
    std::unordered_map<std::string, std::pair<u64, u64>> meta_stamps_; // .ameta path -> (file stamp, settings stamp)

    mutable std::mutex jobs_mutex_;
    mutable std::condition_variable jobs_cv_;
    mutable std::condition_variable idle_cv_;    std::deque<Job> jobs_;
    bool working_ = false;
    bool quit_ = false;
    std::thread worker_;

    std::mutex done_mutex_;
    std::vector<Done> done_;

    std::unordered_map<std::string, Entry> cache_; // UI thread, by asset GUID
    std::unordered_set<std::string> pending_;      // UI thread, GUID\nkey in the queue or being worked
    std::atomic<usize> rendered_{0};
    std::atomic<usize> loaded_{0};
    std::atomic<usize> without_{0};
};

} // namespace aether::editor
