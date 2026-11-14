#include "content/thumbnail_store.h"

#include "aether/assets/image.h"
#include "aether/assets/png_writer.h"
#include "aether/project/project.h"

#include <cstdio>
#include <fstream>

namespace aether::editor {

namespace stdfs = std::filesystem;

namespace {

u64 Mix(u64 h, u64 v) { return h ^ (v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2)); }

// A file's size and modified time as one number, 0 when it is not there. 0 never matches a real stamp, so a source that comes back
// after a miss is rendered again.
u64 FileStamp(const stdfs::path& file) {
    std::error_code ec;
    const u64 size = stdfs::file_size(file, ec);
    if (ec) return 0;
    const auto modified = stdfs::last_write_time(file, ec);
    if (ec) return 0;
    return Mix(size, static_cast<u64>(modified.time_since_epoch().count())) | 1;
}

std::string Hex(u64 value) {
    char text[17] = {};
    std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(value));
    return text;
}

} // namespace

ThumbnailStore::ThumbnailStore(stdfs::path project_file) {
    const ProjectPaths paths = ProjectPaths::ForFile(project_file);
    content_ = paths.content;
    thumbnails_ = paths.saved / "Thumbnails";
    ddc_ = paths.intermediate / "DDC";
    worker_ = std::thread([this] { Work(); });
}

ThumbnailStore::~ThumbnailStore() {
    {
        const std::lock_guard lock(jobs_mutex_);
        quit_ = true;
    }
    jobs_cv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void ThumbnailStore::SetDatabase(std::shared_ptr<assets::AssetDatabase> database) {
    database_ = std::move(database);
    // An import registers sub-assets in the database it is given, so the worker gets a copy of its own: the panel keeps reading
    // the one it scanned.
    jobs_database_ = database_ ? std::make_shared<assets::AssetDatabase>(*database_) : nullptr;
    meta_stamps_.clear();
}

u64 ThumbnailStore::MetaStamp(const assets::AssetDatabase& database, const assets::AssetGuid& guid) {
    const stdfs::path file = database.MetaPath(guid);
    const u64 file_stamp = FileStamp(file);
    if (file_stamp == 0) return 0;
    if (const auto found = meta_stamps_.find(file.string()); found != meta_stamps_.end() && found->second.first == file_stamp) {
        return found->second.second;
    }
    assets::AssetMeta meta;
    u64 settings = 1;
    if (assets::LoadAssetMeta(file, meta)) {
        for (const char c : meta.settings.dump()) settings = Mix(settings, static_cast<unsigned char>(c));
    }
    meta_stamps_[file.string()] = {file_stamp, settings};
    return settings;
}

std::string ThumbnailStore::KeyFor(const assets::AssetDatabase& database, const assets::ContentEntry& entry) {
    const assets::AssetRecord* record = entry.asset;
    if (record == nullptr || record->missing) return {};
    const assets::AssetRecord* source = record->IsSubAsset() ? database.Find(record->parent) : record;
    if (source == nullptr || source->missing) return {};
    u64 stamp = Mix(kSize, FileStamp(content_ / source->path));
    stamp = Mix(stamp, MetaStamp(database, record->guid));
    for (const char c : record->importer) stamp = Mix(stamp, static_cast<unsigned char>(c));
    for (const assets::AssetGuid& dependency : record->dependencies) { // a material's texture changing changes its picture
        if (const assets::AssetRecord* found = database.Find(dependency)) {
            for (const char c : found->source_hash) stamp = Mix(stamp, static_cast<unsigned char>(c));
        }
    }
    return Hex(stamp);
}

stdfs::path ThumbnailStore::PathFor(const std::string& guid, const std::string& key) const {
    return thumbnails_ / (guid + "-" + key + ".png");
}

ThumbnailStore::Picture ThumbnailStore::Get(const assets::ContentEntry& entry) {
    if (!database_ || entry.is_folder || entry.asset == nullptr) return {};
    const std::string guid = assets::ToString(entry.asset->guid);
    const std::string key = KeyFor(*database_, entry);
    if (key.empty()) return {};
    if (const auto found = cache_.find(guid); found != cache_.end() && found->second.key == key) {
        return Picture{found->second.picture ? &*found->second.picture : nullptr, key};
    }
    const stdfs::path file = PathFor(guid, key);
    std::error_code ec;
    if (stdfs::is_regular_file(file, ec)) {
        assets::ImageData image;
        if (assets::DecodeImageFile(file.string(), image) && image.width > 0 && image.height > 0) {
            Entry cached;
            cached.key = key;
            cached.picture = assets::Thumbnail{image.width, image.height, std::move(image.pixels)};
            ++loaded_;
            cache_[guid] = std::move(cached);
            return Picture{&*cache_[guid].picture, key};
        }
        stdfs::remove(file, ec); // a broken copy is rendered again
    }
    if (pending_.insert(guid + "\n" + key).second) {
        {
            const std::lock_guard lock(jobs_mutex_);
            jobs_.push_back(Job{jobs_database_, entry.asset->guid, key});
        }
        jobs_cv_.notify_one();
    }
    return Picture{nullptr, key};
}

void ThumbnailStore::Pump() {
    std::vector<Done> done;
    {
        const std::lock_guard lock(done_mutex_);
        done.swap(done_);
    }
    for (Done& result : done) {
        pending_.erase(result.guid + "\n" + result.key);
        Entry entry;
        entry.key = result.key;
        if (result.picture && !result.picture->rgba.empty()) {
            ++rendered_;
            entry.picture = std::move(result.picture);
        } else {
            ++without_;
        }
        cache_[result.guid] = std::move(entry);
    }
}

void ThumbnailStore::Wait() {
    std::unique_lock lock(jobs_mutex_);
    idle_cv_.wait(lock, [this] { return jobs_.empty() && !working_; });
    lock.unlock();
    Pump();
}

usize ThumbnailStore::Queued() const {
    const std::lock_guard lock(jobs_mutex_);
    return jobs_.size();
}

void ThumbnailStore::Store(const Job& job, const assets::Thumbnail& picture) {
    const std::vector<u8> png = assets::EncodePngRgba8(picture.width, picture.height, picture.rgba);
    if (png.empty()) return;
    const stdfs::path file = PathFor(assets::ToString(job.guid), job.key);
    std::error_code ec;
    stdfs::create_directories(file.parent_path(), ec);
    const stdfs::path temporary = file.string() + ".tmp";
    {
        std::ofstream out(temporary, std::ios::binary);
        out.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
        if (!out) {
            stdfs::remove(temporary, ec);
            return;
        }
    }
    stdfs::remove(file, ec);
    stdfs::rename(temporary, file, ec);
    // Drop copies for an older stamp of this asset; the newest key is the one just written.
    const std::string prefix = assets::ToString(job.guid) + "-";
    for (stdfs::directory_iterator it(thumbnails_, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (name.size() > prefix.size() && name.compare(0, prefix.size(), prefix) == 0 && it->path() != file) stdfs::remove(it->path(), ec);
    }
}

void ThumbnailStore::Work() {
    // The worker owns the importer registry and derived data cache: the browser never imports, so these are not shared with it.
    const assets::ImporterRegistry importers = assets::ImporterRegistry::WithBuiltins();
    assets::DerivedDataCache cache(ddc_);
    for (;;) {
        Job job;
        {
            std::unique_lock lock(jobs_mutex_);
            jobs_cv_.wait(lock, [this] { return quit_ || !jobs_.empty(); });
            if (quit_) return; // closing: a queued picture is not worth blocking the shutdown for
            job = std::move(jobs_.front());
            jobs_.pop_front();
            working_ = true;
        }
        Done result;
        result.guid = assets::ToString(job.guid);
        result.key = job.key;
        if (job.database) {
            try {
                result.picture = assets::AssetThumbnail(*job.database, job.guid, importers, cache, kSize);
            } catch (...) {
                result.picture = std::nullopt; // a kind that cannot be read is one with no picture, not a reason to stop
            }
        }
        if (result.picture && !result.picture->rgba.empty()) Store(job, *result.picture);
        {
            const std::lock_guard lock(done_mutex_);
            done_.push_back(std::move(result));
        }
        {
            const std::lock_guard lock(jobs_mutex_);
            working_ = false;
        }
        idle_cv_.notify_all();
    }
}

} // namespace aether::editor
