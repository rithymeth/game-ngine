#include "aether/save/save_system.h"
#include "aether/core/json_util.h"
#include "envelope_detail.h"

#include "aether/core/log.h"
#include "aether/save/envelope.h"
#include "aether/platform/filesystem.h"

#include <algorithm>
#include <atomic>
#include <ctime>
#include <limits>
#include <map>

namespace aether::save {

namespace stdfs = std::filesystem;
using reflect::Json;

namespace {

std::atomic<SaveSystem*> g_active{nullptr};

constexpr const char* kKind = "aether.save";

SaveResult Fail(SaveError e, std::string m) { return envelope::Fail(e, std::move(m)); }
SaveResult Ok() { return envelope::Ok(); }

// Slot metadata can be absent in older files, but a present value must fit
// exactly. JSON's numeric conversions otherwise silently truncate or wrap.
bool ReadSlotNumber(const Json& data, const char* key, u64 max, u64& out) {
    const auto value = data.find(key);
    if (value == data.end()) { out = 0; return true; }
    if (!value->is_number_integer() || (!value->is_number_unsigned() && value->get<i64>() < 0)) return false;
    out = value->get<u64>();
    return out <= max;
}

} // namespace

SaveSystem::SaveSystem(stdfs::path directory) : directory_(std::move(directory)) {
    worker_ = std::thread([this] { Worker(); });
}

SaveSystem::~SaveSystem() {
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        stop_ = true;
    }
    wake_.notify_all();
    if (worker_.joinable()) worker_.join(); // the worker finishes what is queued first
    SaveSystem* self = this;
    g_active.compare_exchange_strong(self, nullptr);
}

SaveSystem* SaveSystem::Active() { return g_active.load(); }
void SaveSystem::MakeActive() { g_active.store(this); }

bool SaveSystem::ValidSlotName(std::string_view slot) {
    if (slot.empty() || slot.size() > 64) return false;
    for (char c : slot) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) return false;
    }
    return true;
}

stdfs::path SaveSystem::PathOf(std::string_view slot) const { return directory_ / (std::string(slot) + ".asav"); }

std::string SaveSystem::Envelope(std::string_view slot, const reflect::TypeInfo& type, const void* object) const {
    return envelope::Make(kKind, slot, type, object);
}

SaveResult SaveSystem::Write(std::string_view slot, const std::string& contents) {
    std::lock_guard<std::mutex> lock(io_mutex_);
    return envelope::Write(PathOf(slot), contents);
}

SaveResult SaveSystem::Save(std::string_view slot, const reflect::TypeInfo& type, const void* object) {
    if (!ValidSlotName(slot)) return Fail(SaveError::InvalidSlot, "'" + std::string(slot) + "' isn't a valid slot name");
    return Write(slot, Envelope(slot, type, object));
}

SaveResult SaveSystem::Load(std::string_view slot, const reflect::TypeInfo& type, void* object) {
    if (!ValidSlotName(slot)) return Fail(SaveError::InvalidSlot, "'" + std::string(slot) + "' isn't a valid slot name");
    std::lock_guard<std::mutex> lock(io_mutex_);
    return envelope::ReadWithBackup(PathOf(slot), kKind, type, object);
}

bool SaveSystem::Exists(std::string_view slot) const {
    if (!ValidSlotName(slot)) return false;
    std::lock_guard<std::mutex> lock(io_mutex_);
    auto file = PathOf(slot);
    std::error_code ec;
    if (stdfs::is_regular_file(file, ec)) return true;
    file += ".bak";
    return stdfs::is_regular_file(file, ec);
}

std::vector<SlotInfo> SaveSystem::ListSlots() const {
    std::vector<SlotInfo> out;
    std::lock_guard<std::mutex> lock(io_mutex_);
    // Prefer the primary when it exists, but don't hide a recoverable slot
    // just because the backup is the only file left.
    std::map<std::string, std::string> files;
    constexpr std::string_view primary_suffix = ".asav";
    constexpr std::string_view backup_suffix = ".asav.bak";
    for (const std::string& name : fs::ListDirectory(directory_.string())) {
        const bool backup = name.ends_with(backup_suffix);
        if (!backup && !name.ends_with(primary_suffix)) continue;
        const std::string slot = name.substr(0, name.size() - (backup ? backup_suffix.size() : primary_suffix.size()));
        if (!ValidSlotName(slot)) continue; // don't advertise files that the slot API cannot address
        if (backup) files.try_emplace(slot, name);
        else files.insert_or_assign(slot, name);
    }
    out.reserve(files.size());
    for (const auto& [slot, name] : files) {
        SlotInfo info;
        info.slot = slot;
        const stdfs::path file = directory_ / name;
        info.bytes = fs::FileSize(file.string());
        std::vector<u8> bytes;
        if (fs::ReadFileBytes(file.string(), bytes, envelope::kMaxEnvelopeBytes)) {
            const Json j = Json::parse(bytes.begin(), bytes.end(), nullptr, false);
            if (JsonString(j, "$type") == kKind) {
                const auto type = j.find("type");
                const auto payload = j.find("data");
                const auto checksum = j.find("checksum");
                u64 timestamp = 0, version = 0, format = 0;
                // Validate the envelope without needing the game's reflected type.
                if (type != j.end() && type->is_string() && !type->get_ref<const std::string&>().empty() &&
                    payload != j.end() && checksum != j.end() && checksum->is_string() &&
                    ReadSlotNumber(j, "format", envelope::kFormat, format) && format > 0 &&
                    ReadSlotNumber(j, "timestamp_utc", std::numeric_limits<u64>::max(), timestamp) &&
                    ReadSlotNumber(j, "type_version", std::numeric_limits<u16>::max(), version) &&
                    *checksum == envelope::detail::ChecksumText(payload->dump())) {
                    info.type = type->get<std::string>();
                    info.timestamp = timestamp;
                    info.version = static_cast<u16>(version);
                    info.valid = true;
                }
            }
        }
        out.push_back(std::move(info));
    }
    return out;
}

SaveResult SaveSystem::DeleteSlot(std::string_view slot) {
    if (!ValidSlotName(slot)) return Fail(SaveError::InvalidSlot, "'" + std::string(slot) + "' isn't a valid slot name");
    std::lock_guard<std::mutex> lock(io_mutex_);
    std::error_code ec;
    const stdfs::path file = PathOf(slot);
    stdfs::path backup = file;
    backup += ".bak";
    // A directory is not a save, and must not cause the other file to be deleted.
    for (const stdfs::path& path : {file, backup}) {
        const stdfs::file_status status = stdfs::symlink_status(path, ec);
        if (ec && ec != std::errc::no_such_file_or_directory) {
            return Fail(SaveError::IoError, "cannot inspect '" + path.filename().string() + "': " + ec.message());
        }
        if (stdfs::is_directory(status)) {
            return Fail(SaveError::IoError, "'" + path.filename().string() + "' is a directory");
        }
    }
    bool deleted = false;
    // If deleting the backup fails, do not also delete the primary.
    for (const stdfs::path& path : {backup, file}) {
        const bool removed = stdfs::remove(path, ec);
        if (ec) return Fail(SaveError::IoError, "cannot delete '" + path.filename().string() + "': " + ec.message());
        deleted = deleted || removed;
    }
    if (!deleted) return Fail(SaveError::NotFound, "no save in slot '" + std::string(slot) + "'");
    return Ok();
}

void SaveSystem::SaveAsync(std::string_view slot, const reflect::TypeInfo& type, const void* object, SaveCallback done) {
    if (!ValidSlotName(slot)) {
        // Reported through the same path as any other result.
        std::lock_guard<std::mutex> lock(queue_mutex_);
        finished_.push_back({Fail(SaveError::InvalidSlot, "'" + std::string(slot) + "' isn't a valid slot name"), std::move(done)});
        return;
    }
    Job job{std::string(slot), Envelope(slot, type, object), std::move(done)}; // the snapshot, taken here
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        jobs_.push_back(std::move(job));
    }
    wake_.notify_one();
}

void SaveSystem::Worker() {
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            wake_.wait(lock, [this] { return stop_ || !jobs_.empty(); });
            if (jobs_.empty()) return; // stopping, and nothing left
            job = std::move(jobs_.front());
            jobs_.pop_front();
            busy_ = true;
        }
        SaveResult result = Write(job.slot, job.contents);
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            finished_.push_back({std::move(result), std::move(job.done)});
            busy_ = false;
        }
        idle_.notify_all();
    }
}

void SaveSystem::Flush() {
    std::unique_lock<std::mutex> lock(queue_mutex_);
    idle_.wait(lock, [this] { return jobs_.empty() && !busy_; });
}

usize SaveSystem::Pending() const {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    return jobs_.size() + (busy_ ? 1 : 0);
}

usize SaveSystem::Pump() {
    std::deque<Done> ready;
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        ready.swap(finished_);
    }
    for (Done& d : ready) {
        if (d.callback) d.callback(d.result);
    }
    return ready.size();
}

} // namespace aether::save
