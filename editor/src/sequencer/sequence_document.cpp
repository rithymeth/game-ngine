#include "sequence_document.h"

#include <algorithm>
#include <cmath>

namespace aether::editor {

using namespace seq;

namespace {

constexpr usize kMaxUndo = 256;
constexpr f32 kSameTime = 0.001f;

template <typename K>
i32 IndexOfTime(const std::vector<K>& keys, f32 time) {
    for (usize i = 0; i < keys.size(); ++i) {
        if (std::fabs(keys[i].time - time) <= kSameTime) return static_cast<i32>(i);
    }
    return -1;
}

} // namespace

SequenceDocument::SequenceDocument(LevelSequence sequence) : sequence_(std::move(sequence)) {}

void SequenceDocument::Record() {
    if (in_edit_) {
        if (edit_recorded_) return;
        edit_recorded_ = true;
    }
    undo_.push_back(sequence_);
    if (undo_.size() > kMaxUndo) undo_.erase(undo_.begin());
    redo_.clear();
}

void SequenceDocument::Changed() {
    ++revision_;
    dirty_ = true;
}

void SequenceDocument::Commit(usize track) {
    if (track < sequence_.tracks.size()) sequence_.tracks[track].Normalize();
    Changed();
}

bool SequenceDocument::Undo() {
    if (undo_.empty()) return false;
    redo_.push_back(std::move(sequence_));
    sequence_ = std::move(undo_.back());
    undo_.pop_back();
    Changed();
    return true;
}

bool SequenceDocument::Redo() {
    if (redo_.empty()) return false;
    undo_.push_back(std::move(sequence_));
    sequence_ = std::move(redo_.back());
    redo_.pop_back();
    Changed();
    return true;
}

void SequenceDocument::BeginEdit() {
    in_edit_ = true;
    edit_recorded_ = false;
}

void SequenceDocument::EndEdit() {
    in_edit_ = false;
    edit_recorded_ = false;
}

usize SequenceDocument::AddTrack(TrackType type, const std::string& name, const EntityGuid& binding) {
    Record();
    Track t;
    for (int n = 1;; ++n) {
        t.id = "track" + std::to_string(n);
        if (!sequence_.FindTrack(t.id)) break;
    }
    t.name = name;
    t.type = type;
    t.binding = binding;
    switch (type) {
    case TrackType::Transform: t.channels = {Channel{"position.x", {}}, Channel{"position.y", {}}, Channel{"position.z", {}}}; break;
    case TrackType::Property: t.channels = {Channel{"value", {}}}; break;
    case TrackType::Visibility: t.channels = {Channel{"visible", {}}}; break;
    default: break;
    }
    sequence_.tracks.push_back(std::move(t));
    Changed();
    return sequence_.tracks.size() - 1;
}

bool SequenceDocument::RemoveTrack(usize track) {
    if (!Editable(track)) return false;
    Record();
    sequence_.tracks.erase(sequence_.tracks.begin() + static_cast<std::ptrdiff_t>(track));
    Changed();
    return true;
}

bool SequenceDocument::RenameTrack(usize track, const std::string& name) {
    if (!Editable(track)) return false;
    Record();
    sequence_.tracks[track].name = name;
    Changed();
    return true;
}

bool SequenceDocument::SetBinding(usize track, const EntityGuid& binding) {
    if (!Editable(track)) return false;
    Record();
    sequence_.tracks[track].binding = binding;
    Changed();
    return true;
}

bool SequenceDocument::SetMute(usize track, bool mute) {
    if (track >= sequence_.tracks.size()) return false;
    Record();
    sequence_.tracks[track].mute = mute;
    Changed();
    return true;
}

bool SequenceDocument::SetLocked(usize track, bool locked) {
    if (track >= sequence_.tracks.size()) return false;
    Record();
    sequence_.tracks[track].locked = locked;
    Changed();
    return true;
}

bool SequenceDocument::SetPropertyTarget(usize track, const std::string& component, const std::string& field) {
    if (!Editable(track) || sequence_.tracks[track].type != TrackType::Property) return false;
    Record();
    sequence_.tracks[track].component = component;
    sequence_.tracks[track].field = field;
    Changed();
    return true;
}

i32 SequenceDocument::AddKey(usize track, usize channel, f32 time, f32 value, Interp interp) {
    if (!Editable(track) || channel >= sequence_.tracks[track].channels.size() || time < 0.0f) return -1;
    Record();
    Channel& c = sequence_.tracks[track].channels[channel];
    if (const i32 same = IndexOfTime(c.keys, time); same >= 0) {
        c.keys[static_cast<usize>(same)].value = value;
        Commit(track);
        return same;
    }
    Key k;
    k.time = time;
    k.value = value;
    k.interp = interp;
    c.keys.push_back(k);
    Commit(track);
    return IndexOfTime(c.keys, time);
}

i32 SequenceDocument::AddRotationKey(usize track, f32 time, const Quaternion& value, Interp interp) {
    if (!Editable(track) || sequence_.tracks[track].type != TrackType::Transform || time < 0.0f) return -1;
    Record();
    Track& t = sequence_.tracks[track];
    if (const i32 same = IndexOfTime(t.rotation, time); same >= 0) {
        t.rotation[static_cast<usize>(same)].value = value;
        Commit(track);
        return same;
    }
    t.rotation.push_back(RotationKey{time, value, interp});
    Commit(track);
    return IndexOfTime(t.rotation, time);
}

i32 SequenceDocument::AddEvent(usize track, f32 time, const std::string& name, const std::string& payload) {
    if (!Editable(track) || sequence_.tracks[track].type != TrackType::Event || time < 0.0f) return -1;
    Record();
    Track& t = sequence_.tracks[track];
    t.events.push_back(EventKey{time, name, payload});
    Commit(track);
    // Events may share a time; the new one is the last at its time.
    i32 found = -1;
    for (usize i = 0; i < t.events.size(); ++i) {
        if (std::fabs(t.events[i].time - time) <= kSameTime && t.events[i].name == name) found = static_cast<i32>(i);
    }
    return found;
}

bool SequenceDocument::SetEvent(const KeyRef& key, const std::string& name, const std::string& payload) {
    if (!Editable(key.track) || key.lane != KeyRef::Lane::Event || key.index >= sequence_.tracks[key.track].events.size()) return false;
    Record();
    EventKey& e = sequence_.tracks[key.track].events[key.index];
    e.name = name;
    e.payload = payload;
    Changed();
    return true;
}

f32 SequenceDocument::KeyTime(const KeyRef& key) const {
    if (key.track >= sequence_.tracks.size()) return 0.0f;
    const Track& t = sequence_.tracks[key.track];
    switch (key.lane) {
    case KeyRef::Lane::Channel: return key.channel < t.channels.size() && key.index < t.channels[key.channel].keys.size() ? t.channels[key.channel].keys[key.index].time : 0.0f;
    case KeyRef::Lane::Rotation: return key.index < t.rotation.size() ? t.rotation[key.index].time : 0.0f;
    case KeyRef::Lane::Event: return key.index < t.events.size() ? t.events[key.index].time : 0.0f;
    case KeyRef::Lane::Spawn: return key.index < t.spawns.size() ? t.spawns[key.index].time : 0.0f;
    case KeyRef::Lane::Cut: return key.index < t.cuts.size() ? t.cuts[key.index].time : 0.0f;
    case KeyRef::Lane::Audio: return key.index < t.audio.size() ? t.audio[key.index].time : 0.0f;
    case KeyRef::Lane::Animation: return key.index < t.anims.size() ? t.anims[key.index].time : 0.0f;
    }
    return 0.0f;
}

usize SequenceDocument::KeyCount(usize track, KeyRef::Lane lane, usize channel) const {
    if (track >= sequence_.tracks.size()) return 0;
    const Track& t = sequence_.tracks[track];
    switch (lane) {
    case KeyRef::Lane::Channel: return channel < t.channels.size() ? t.channels[channel].keys.size() : 0;
    case KeyRef::Lane::Rotation: return t.rotation.size();
    case KeyRef::Lane::Event: return t.events.size();
    case KeyRef::Lane::Spawn: return t.spawns.size();
    case KeyRef::Lane::Cut: return t.cuts.size();
    case KeyRef::Lane::Audio: return t.audio.size();
    case KeyRef::Lane::Animation: return t.anims.size();
    }
    return 0;
}

i32 SequenceDocument::MoveKey(const KeyRef& key, f32 time) {
    if (!Editable(key.track) || time < 0.0f || key.index >= KeyCount(key.track, key.lane, key.channel)) return -1;
    Track& t = sequence_.tracks[key.track];
    // Value, rotation and cut keys can't share a time with another key of their list.
    const auto clash = [&](const auto& keys) {
        for (usize i = 0; i < keys.size(); ++i) {
            if (i != key.index && std::fabs(keys[i].time - time) <= kSameTime) return true;
        }
        return false;
    };
    const auto move = [&](auto& keys, bool allow_shared) -> i32 {
        if (!allow_shared && clash(keys)) return -1;
        Record();
        keys[key.index].time = time;
        Commit(key.track);
        // Find it again after sorting: the key now at that time with the same identity.
        for (usize i = 0; i < keys.size(); ++i) {
            if (std::fabs(keys[i].time - time) <= kSameTime) return static_cast<i32>(i);
        }
        return -1;
    };
    switch (key.lane) {
    case KeyRef::Lane::Channel: return move(t.channels[key.channel].keys, false);
    case KeyRef::Lane::Rotation: return move(t.rotation, false);
    case KeyRef::Lane::Event: return move(t.events, true);
    case KeyRef::Lane::Spawn: return move(t.spawns, true);
    case KeyRef::Lane::Cut: return move(t.cuts, false);
    case KeyRef::Lane::Audio: return move(t.audio, true);
    case KeyRef::Lane::Animation: return move(t.anims, true);
    }
    return -1;
}

bool SequenceDocument::SetKeyValue(const KeyRef& key, f32 value) {
    if (!Editable(key.track) || key.lane != KeyRef::Lane::Channel || key.index >= KeyCount(key.track, key.lane, key.channel)) return false;
    Record();
    sequence_.tracks[key.track].channels[key.channel].keys[key.index].value = value;
    Changed();
    return true;
}

bool SequenceDocument::SetKeyInterp(const KeyRef& key, Interp interp) {
    if (!Editable(key.track) || key.index >= KeyCount(key.track, key.lane, key.channel)) return false;
    Track& t = sequence_.tracks[key.track];
    if (key.lane == KeyRef::Lane::Channel) {
        Record();
        t.channels[key.channel].keys[key.index].interp = interp;
    } else if (key.lane == KeyRef::Lane::Rotation) {
        Record();
        t.rotation[key.index].interp = interp;
    } else {
        return false;
    }
    Changed();
    return true;
}

bool SequenceDocument::SetRotationValue(const KeyRef& key, const Quaternion& value) {
    if (!Editable(key.track) || key.lane != KeyRef::Lane::Rotation || key.index >= sequence_.tracks[key.track].rotation.size()) return false;
    Record();
    sequence_.tracks[key.track].rotation[key.index].value = value;
    Changed();
    return true;
}

bool SequenceDocument::RemoveKey(const KeyRef& key) {
    if (!Editable(key.track) || key.index >= KeyCount(key.track, key.lane, key.channel)) return false;
    Record();
    Track& t = sequence_.tracks[key.track];
    const auto at = static_cast<std::ptrdiff_t>(key.index);
    switch (key.lane) {
    case KeyRef::Lane::Channel: t.channels[key.channel].keys.erase(t.channels[key.channel].keys.begin() + at); break;
    case KeyRef::Lane::Rotation: t.rotation.erase(t.rotation.begin() + at); break;
    case KeyRef::Lane::Event: t.events.erase(t.events.begin() + at); break;
    case KeyRef::Lane::Spawn: t.spawns.erase(t.spawns.begin() + at); break;
    case KeyRef::Lane::Cut: t.cuts.erase(t.cuts.begin() + at); break;
    case KeyRef::Lane::Audio: t.audio.erase(t.audio.begin() + at); break;
    case KeyRef::Lane::Animation: t.anims.erase(t.anims.begin() + at); break;
    }
    Changed();
    return true;
}

bool SequenceDocument::SetDuration(f32 seconds) {
    if (seconds < 0.0f) return false;
    Record();
    sequence_.duration = seconds;
    Changed();
    return true;
}

bool SequenceDocument::SetFps(f32 fps) {
    if (!(fps > 0.0f)) return false;
    Record();
    sequence_.fps = fps;
    Changed();
    return true;
}

bool SequenceDocument::SetName(const std::string& name) {
    Record();
    sequence_.name = name;
    Changed();
    return true;
}

bool SequenceDocument::Load(const std::filesystem::path& file, std::string* error) {
    LevelSequence loaded;
    if (!LoadSequence(file.string(), loaded, error)) return false;
    sequence_ = std::move(loaded);
    sequence_.Normalize();
    undo_.clear();
    redo_.clear();
    in_edit_ = edit_recorded_ = false;
    dirty_ = false;
    ++revision_;
    return true;
}

bool SequenceDocument::Save(const std::filesystem::path& file, std::string* error) {
    if (!SaveSequence(file.string(), sequence_, error)) return false;
    dirty_ = false;
    return true;
}

} // namespace aether::editor
