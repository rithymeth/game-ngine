#pragma once

#include "aether/sequencer/sequence.h"

#include <filesystem>
#include <string>
#include <vector>

// A level sequence being edited (Phase 27 step 5, docs/design/PHASE_SPECS.md
// §27.6): tracks and keys added, moved, edited and removed, with undo and
// redo (whole-sequence snapshots: a sequence is small), a dirty flag, and
// the diagnostics. A drag is one undo step (BeginEdit .. EndEdit). Keys on a
// locked track, and the track itself, can't be changed until it is unlocked.

namespace aether::editor {

// Which key list of a track a key is in, and where.
struct KeyRef {
    enum class Lane : u8 { Channel, Rotation, Event, Spawn, Cut, Audio, Animation };
    usize track = 0;
    Lane lane = Lane::Channel;
    usize channel = 0; // Lane::Channel: which channel
    usize index = 0;
    bool operator==(const KeyRef& o) const { return track == o.track && lane == o.lane && channel == o.channel && index == o.index; }
};

class SequenceDocument {
public:
    explicit SequenceDocument(seq::LevelSequence sequence = {});

    const seq::LevelSequence& Sequence() const { return sequence_; }
    u64 Revision() const { return revision_; }
    bool dirty() const { return dirty_; }
    // ValidateSequence on the current state.
    std::vector<std::string> Problems() const { return seq::ValidateSequence(sequence_); }

    // Undo and redo; a group is one step.
    bool Undo();
    bool Redo();
    bool CanUndo() const { return !undo_.empty(); }
    bool CanRedo() const { return !redo_.empty(); }
    // Everything between Begin and End is one undo step (a drag).
    void BeginEdit();
    void EndEdit();

    // Tracks. AddTrack makes the channels its kind needs (Transform: x, y, z;
    // Property and Visibility: one) and a unique id; returns its index.
    usize AddTrack(seq::TrackType type, const std::string& name, const EntityGuid& binding = {});
    bool RemoveTrack(usize track);
    bool RenameTrack(usize track, const std::string& name);
    bool SetBinding(usize track, const EntityGuid& binding);
    bool SetMute(usize track, bool mute);
    bool SetLocked(usize track, bool locked); // works on a locked track (it's how to unlock)
    bool SetPropertyTarget(usize track, const std::string& component, const std::string& field);

    // Value keys: a key at the same time (within a millisecond) has its value
    // replaced. Returns the key's index after sorting, or -1.
    i32 AddKey(usize track, usize channel, f32 time, f32 value, seq::Interp interp = seq::Interp::Linear);
    i32 MoveKey(const KeyRef& key, f32 time); // any lane; the new index, or -1 (a clash with another key's time is refused)
    bool SetKeyValue(const KeyRef& key, f32 value);       // Channel lane
    bool SetKeyInterp(const KeyRef& key, seq::Interp interp); // Channel and Rotation lanes
    bool SetRotationValue(const KeyRef& key, const Quaternion& value);
    i32 AddRotationKey(usize track, f32 time, const Quaternion& value, seq::Interp interp = seq::Interp::Linear);
    i32 AddEvent(usize track, f32 time, const std::string& name, const std::string& payload = {});
    bool SetEvent(const KeyRef& key, const std::string& name, const std::string& payload);
    bool RemoveKey(const KeyRef& key);

    // Time of any key, and how many keys a track has in a lane (for drawing).
    f32 KeyTime(const KeyRef& key) const;
    usize KeyCount(usize track, KeyRef::Lane lane, usize channel = 0) const;

    bool SetDuration(f32 seconds);
    bool SetFps(f32 fps);
    bool SetName(const std::string& name);

    // Replaces the document with the file's (not undoable; clears the history);
    // on failure the document is unchanged and `error` says why.
    bool Load(const std::filesystem::path& file, std::string* error = nullptr);
    // Writes the file and clears the dirty flag.
    bool Save(const std::filesystem::path& file, std::string* error = nullptr);

private:
    bool Editable(usize track) const { return track < sequence_.tracks.size() && !sequence_.tracks[track].locked; }
    void Record();
    void Changed();
    // Sorts the track's keys and applies the change.
    void Commit(usize track);

    seq::LevelSequence sequence_;
    std::vector<seq::LevelSequence> undo_, redo_;
    bool in_edit_ = false;
    bool edit_recorded_ = false;
    u64 revision_ = 1;
    bool dirty_ = false;
};

} // namespace aether::editor
