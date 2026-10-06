#pragma once

#include "aether/core/base.h"
#include "aether/save/save_bag.h"
#include "aether/save/save_result.h"
#include "aether/reflection/reflection.h"
#include "aether/reflection/serialize.h"

#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

// Save games (Phase 28 step 1, docs/design/PHASE_SPECS.md §28.1): a game's
// reflected struct (AETHER_REFLECT) saved to a named slot and loaded back.
//
//   struct MySave { i32 level = 1; f32 health = 100; std::string name; };
//   AETHER_REFLECT(MySave, 2, AETHER_FIELD(level, ...), ...)
//   SaveSystem saves(dir);
//   saves.Save("slot1", mySave);
//   saves.Load("slot1", mySave);
//
// A slot is one `<slot>.asav` file in the system's folder: JSON text (so it
// can be read, diffed and opened in the editor) in an envelope that names the
// struct type and its version, the time, and a CRC-32 of the data. Writes are
// atomic (temp file, then rename) and keep the previous save as
// `<slot>.asav.bak`; loading a damaged file falls back to the backup. Schema
// changes use the reflection layer's migration hooks
// (reflect::RegisterMigration), which loading runs for older saves; a save
// from a newer version than the code knows is refused, not guessed at.

namespace aether::save {

struct SlotInfo {
    std::string slot;
    std::string type;       // the struct's name; empty if the file couldn't be read
    u64 timestamp = 0;      // unix seconds when saved
    u64 bytes = 0;          // the file's size
    u16 version = 0;        // the struct's schema version when saved
    bool valid = false;     // a readable save file
};

using SaveCallback = std::function<void(const SaveResult&)>;

class SaveSystem {
public:
    // Saves live in `directory` (made on the first save).
    explicit SaveSystem(std::filesystem::path directory);
    // Finishes queued saves. Callbacks that weren't Pump()ed by now are dropped.
    ~SaveSystem();
    SaveSystem(const SaveSystem&) = delete;
    SaveSystem& operator=(const SaveSystem&) = delete;

    const std::filesystem::path& Directory() const { return directory_; }
    static bool ValidSlotName(std::string_view slot);

    SaveResult Save(std::string_view slot, const reflect::TypeInfo& type, const void* object);
    SaveResult Load(std::string_view slot, const reflect::TypeInfo& type, void* object);
    template <typename T>
    SaveResult Save(std::string_view slot, const T& object) {
        return Save(slot, reflect::Reflect<T>(), &object);
    }
    template <typename T>
    SaveResult Load(std::string_view slot, T& object) {
        return Load(slot, reflect::Reflect<T>(), &object);
    }

    // Saving without a hitch: the object is turned into text now, on the
    // calling thread (so it can change right after the call), and a worker
    // thread writes the file. Saves to one slot happen in the order made.
    // `done` is called from Pump() on the thread that pumps.
    void SaveAsync(std::string_view slot, const reflect::TypeInfo& type, const void* object, SaveCallback done = {});
    template <typename T>
    void SaveAsync(std::string_view slot, const T& object, SaveCallback done = {}) {
        SaveAsync(slot, reflect::Reflect<T>(), &object, std::move(done));
    }
    // Runs the callbacks of the saves that finished; returns how many.
    usize Pump();
    // Blocks until every queued save is written (then Pump() delivers them).
    void Flush();
    usize Pending() const;

    bool Exists(std::string_view slot) const;
    // Every readable or unreadable `.asav` in the folder, by slot name.
    std::vector<SlotInfo> ListSlots() const;
    // Removes the slot and its backup; NotFound if there was neither.
    SaveResult DeleteSlot(std::string_view slot);

    // The system the host made active (the one the Blueprint nodes act on).
    static SaveSystem* Active();
    void MakeActive();

    // What Blueprints and Luau use (the SaveGames library, §28.4): the system's
    // own bag of named values, the world the CaptureWorld / RestoreWorld nodes
    // act on (set by the host), the last error message, and the async saves
    // that finished (the host calls Pump(), then TakeFinishedSaves(), and
    // dispatches Event.OnSaveFinished). Main thread only.
    SaveBag& Bag() { return bag_; }
    struct WorldContext {
        World* world = nullptr;
        GuidIndex* guids = nullptr;
        WorldTracker* tracker = nullptr;
        Lifecycle* lifecycle = nullptr;
    };
    void SetWorldContext(const WorldContext& context) { world_context_ = context; }
    const WorldContext& GetWorldContext() const { return world_context_; }
    const std::string& LastError() const { return last_error_; }
    void SetLastError(std::string message) { last_error_ = std::move(message); }
    struct FinishedSave {
        std::string slot;
        bool success = false;
    };
    void NoteFinishedSave(FinishedSave finished) { finished_saves_.push_back(std::move(finished)); }
    std::vector<FinishedSave> TakeFinishedSaves() {
        std::vector<FinishedSave> out;
        out.swap(finished_saves_);
        return out;
    }

private:
    struct Job {
        std::string slot;
        std::string contents;
        SaveCallback done;
    };
    struct Done {
        SaveResult result;
        SaveCallback callback;
    };
    std::filesystem::path PathOf(std::string_view slot) const;
    std::string Envelope(std::string_view slot, const reflect::TypeInfo& type, const void* object) const;
    SaveResult Write(std::string_view slot, const std::string& contents);
    void Worker();

    std::filesystem::path directory_;
    mutable std::mutex io_mutex_; // one file operation at a time
    mutable std::mutex queue_mutex_;
    std::condition_variable wake_, idle_;
    std::deque<Job> jobs_;
    std::deque<Done> finished_;
    SaveBag bag_;
    WorldContext world_context_;
    std::string last_error_;
    std::vector<FinishedSave> finished_saves_;
    bool busy_ = false;
    bool stop_ = false;
    std::thread worker_;
};

} // namespace aether::save
