#include "aether/save/save_system.h"

#include "aether/core/log.h"
#include "aether/pak/pak.h"
#include "aether/platform/filesystem.h"

#include <algorithm>
#include <atomic>
#include <ctime>

namespace aether::save {

namespace stdfs = std::filesystem;
using reflect::Json;

namespace {

std::atomic<SaveSystem*> g_active{nullptr};

constexpr u32 kFormat = 1;

SaveResult Fail(SaveError error, std::string message) {
    SaveResult r;
    r.error = error;
    r.message = std::move(message);
    AETHER_LOG_WARN("Save", "%s", r.message.c_str());
    return r;
}

SaveResult Ok() {
    SaveResult r;
    r.ok = true;
    return r;
}

u32 Checksum(const std::string& text) {
    return pak::Crc32(std::span<const u8>(reinterpret_cast<const u8*>(text.data()), text.size()));
}

std::string Hex(u32 value) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%08x", value);
    return buf;
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
    Json data = reflect::ToJson(type, object);
    const std::string dumped = data.dump();
    Json envelope = {{"$type", "aether.save"},
                     {"format", kFormat},
                     {"type", type.name},
                     {"type_version", type.version},
                     {"slot", std::string(slot)},
                     {"timestamp_utc", static_cast<u64>(std::time(nullptr))},
                     {"checksum", Hex(Checksum(dumped))},
                     {"data", std::move(data)}};
    return envelope.dump(2);
}

SaveResult SaveSystem::Write(std::string_view slot, const std::string& contents) {
    std::lock_guard<std::mutex> lock(io_mutex_);
    const stdfs::path file = PathOf(slot);
    std::error_code ec;
    stdfs::create_directories(directory_, ec);
    if (stdfs::exists(file, ec)) {
        // Keep the previous save: if this write or the new file turns out bad, it is still there.
        stdfs::path backup = file;
        backup += ".bak";
        stdfs::copy_file(file, backup, stdfs::copy_options::overwrite_existing, ec);
    }
    if (!fs::WriteFileAtomic(file.string(), contents.data(), contents.size())) {
        return Fail(SaveError::IoError, "couldn't write " + file.string());
    }
    return Ok();
}

SaveResult SaveSystem::Save(std::string_view slot, const reflect::TypeInfo& type, const void* object) {
    if (!ValidSlotName(slot)) return Fail(SaveError::InvalidSlot, "'" + std::string(slot) + "' isn't a valid slot name");
    return Write(slot, Envelope(slot, type, object));
}

SaveResult SaveSystem::LoadFile(const stdfs::path& file, const reflect::TypeInfo& type, void* object) const {
    std::vector<u8> bytes;
    if (!fs::ReadFileBytes(file.string(), bytes)) return Fail(SaveError::NotFound, "no save at " + file.string());
    const Json envelope = Json::parse(bytes.begin(), bytes.end(), nullptr, /*allow_exceptions=*/false);
    if (envelope.is_discarded() || !envelope.is_object() || envelope.value("$type", "") != "aether.save" || !envelope.contains("data")) {
        return Fail(SaveError::Corrupt, file.string() + " isn't a readable save file");
    }
    if (envelope.value("format", 0u) == 0 || envelope.value("format", 0u) > kFormat) {
        return Fail(SaveError::Corrupt, file.string() + " has a save format this version doesn't know");
    }
    if (envelope.value("checksum", "") != Hex(Checksum(envelope["data"].dump()))) {
        return Fail(SaveError::Corrupt, file.string() + " has been changed or damaged (its checksum doesn't match)");
    }
    if (envelope.value("type", "") != type.name) {
        return Fail(SaveError::WrongType, file.string() + " holds a '" + envelope.value("type", "") + "', not a '" + type.name + "'");
    }
    if (envelope.value("type_version", 0u) > type.version) {
        return Fail(SaveError::FutureVersion, file.string() + " was saved by a newer version of '" + type.name + "' (" +
                                                  std::to_string(envelope.value("type_version", 0u)) + ", this code has " + std::to_string(type.version) + ")");
    }
    reflect::LoadReport report;
    if (!reflect::FromJson(type, object, envelope["data"], &report)) {
        return Fail(SaveError::Corrupt, file.string() + " doesn't have the shape of a '" + type.name + "'");
    }
    SaveResult r = Ok();
    r.warnings = std::move(report.warnings);
    return r;
}

SaveResult SaveSystem::Load(std::string_view slot, const reflect::TypeInfo& type, void* object) {
    if (!ValidSlotName(slot)) return Fail(SaveError::InvalidSlot, "'" + std::string(slot) + "' isn't a valid slot name");
    std::lock_guard<std::mutex> lock(io_mutex_);
    const stdfs::path file = PathOf(slot);
    stdfs::path backup = file;
    backup += ".bak";
    SaveResult result = LoadFile(file, type, object);
    if (result.ok) return result;
    // A wrong type or a newer version is the caller's to hear about; damage or a missing file falls back to the backup.
    if ((result.error == SaveError::Corrupt || result.error == SaveError::NotFound) && fs::Exists(backup.string())) {
        SaveResult from_backup = LoadFile(backup, type, object);
        if (from_backup.ok) {
            from_backup.warnings.push_back("the save was unreadable (" + result.message + "): loaded the previous one");
            return from_backup;
        }
    }
    return result;
}

bool SaveSystem::Exists(std::string_view slot) const {
    if (!ValidSlotName(slot)) return false;
    std::lock_guard<std::mutex> lock(io_mutex_);
    return fs::Exists(PathOf(slot).string());
}

std::vector<SlotInfo> SaveSystem::ListSlots() const {
    std::vector<SlotInfo> out;
    std::lock_guard<std::mutex> lock(io_mutex_);
    for (const std::string& name : fs::ListDirectory(directory_.string())) {
        if (name.size() <= 5 || name.compare(name.size() - 5, 5, ".asav") != 0) continue;
        SlotInfo info;
        info.slot = name.substr(0, name.size() - 5);
        const stdfs::path file = directory_ / name;
        info.bytes = fs::FileSize(file.string());
        std::vector<u8> bytes;
        if (fs::ReadFileBytes(file.string(), bytes)) {
            const Json j = Json::parse(bytes.begin(), bytes.end(), nullptr, false);
            if (!j.is_discarded() && j.is_object() && j.value("$type", "") == "aether.save") {
                info.type = j.value("type", "");
                info.timestamp = j.value("timestamp_utc", static_cast<u64>(0));
                info.version = static_cast<u16>(j.value("type_version", 0u));
                info.valid = true;
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
    const bool a = stdfs::remove(file, ec);
    const bool b = stdfs::remove(backup, ec);
    if (!a && !b) return Fail(SaveError::NotFound, "no save in slot '" + std::string(slot) + "'");
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
