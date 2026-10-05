#include "aether/save/save_system.h"

#include "aether/core/log.h"
#include "aether/save/envelope.h"
#include "aether/platform/filesystem.h"

#include <algorithm>
#include <atomic>
#include <ctime>

namespace aether::save {

namespace stdfs = std::filesystem;
using reflect::Json;

namespace {

std::atomic<SaveSystem*> g_active{nullptr};

constexpr const char* kKind = "aether.save";

SaveResult Fail(SaveError e, std::string m) { return envelope::Fail(e, std::move(m)); }
SaveResult Ok() { return envelope::Ok(); }

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
