#pragma once

#include "aether/core/base.h"
#include "aether/math/math.h"
#include "aether/scene/entity_guid.h"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

// Level sequences (Phase 27 step 1, docs/design/PHASE_SPECS.md §27.1): the
// data of a cutscene. A sequence is tracks; a track is bound to an entity by
// its GUID (so it survives undo and reloading the scene) and holds keyed
// values: a Transform track animates position and rotation, a Property track
// animates a field of any reflected component. Saved as `.asequence` JSON.

namespace aether::seq {

enum class Interp : u8 {
    Constant, // holds the key's value until the next key
    Linear,
    Bezier,   // a cubic Hermite curve from the keys' tangents (units per second)
};

struct Key {
    f32 time = 0.0f; // seconds
    f32 value = 0.0f;
    Interp interp = Interp::Linear; // how the segment from this key to the next is drawn
    f32 in_tangent = 0.0f;  // slope arriving at this key (Bezier)
    f32 out_tangent = 0.0f; // slope leaving it
};

struct Channel {
    std::string name;
    std::vector<Key> keys; // strictly increasing in time (Normalize sorts)

    void Normalize();
    bool Empty() const { return keys.empty(); }
    // The value at `time`: before the first key and after the last it holds
    // that key's value. An empty channel gives 0.
    f32 Evaluate(f32 time) const;
};

struct RotationKey {
    f32 time = 0.0f;
    Quaternion value;
    Interp interp = Interp::Linear; // Constant holds; Linear and Bezier are a slerp the short way round
};

// A named moment: the player reports it when the playhead crosses it, and
// the host turns it into a Blueprint event, a sound, a script call...
struct EventKey {
    f32 time = 0.0f;
    std::string name;
    std::string payload; // free text for the receiver
};

enum class TrackType : u8 { Transform, Property, Event, Visibility };

struct Track {
    std::string id;   // unique in the sequence
    std::string name; // shown in the editor
    TrackType type = TrackType::Transform;
    EntityGuid binding; // the entity it animates
    bool mute = false;
    bool locked = false; // the editor won't edit it

    // Transform: channels are position x, y and z (one with no keys leaves
    // that axis alone), and `rotation` the keyed orientation (none leaves it).
    // Property: `component` and `field` name what is animated; `channels` are
    // its values: one for a number, bool or enum, one per member for a
    // struct of floats (Vec3: x, y, z; a colour: r, g, b, a).
    // Event: `events` fire as the playhead crosses them; the binding is
    // optional (the entity the event is about; none for a global one).
    // Visibility: one channel of 0 (hidden) and 1 (shown) keys, held until
    // the next key; it switches the entity's Active flag.
    std::vector<Channel> channels;
    std::vector<RotationKey> rotation;
    std::vector<EventKey> events;
    std::string component;
    std::string field;

    void Normalize();
};

struct LevelSequence {
    static constexpr u32 kVersion = 1;
    std::string name;
    f32 fps = 24.0f;       // for display and snapping; keys sit on exact seconds
    f32 duration = 0.0f;   // seconds; 0 means "to the last key"
    std::vector<Track> tracks;

    void Normalize();
    const Track* FindTrack(const std::string& id) const;
    // `duration`, or the last key's time when that is later (or duration is 0).
    f32 EffectiveDuration() const;
};

// The orientation between two quaternions the short way round (`t` in 0..1).
Quaternion Slerp(const Quaternion& a, const Quaternion& b, f32 t);
// A rotation channel's value at `time`; identity with no keys.
Quaternion EvaluateRotation(const std::vector<RotationKey>& keys, f32 time);

// Diagnostics, as "SQxxx: message": SQ001 no tracks; SQ002 keys not strictly
// increasing; SQ003 a key outside the duration; SQ004 a Property track with
// no component or field; SQ005 two tracks with one id; SQ006 a negative
// duration or an fps that isn't positive; SQ007 a track with no id; SQ008 a
// track bound to no entity; SQ009 a Transform track without its three
// position channels; SQ010 a Property track with no channels; SQ011 an
// Event key with no name; SQ012 a Visibility track that doesn't have exactly
// one channel of 0 and 1 values; SQ013 keys on a track of the wrong kind
// (events on a Transform, rotation keys on a Property...). A Event track's
// binding is optional, so SQ008 doesn't apply to it.
std::vector<std::string> ValidateSequence(const LevelSequence& sequence);

// .asequence files. FromJson leaves `out` unchanged on failure.
nlohmann::json SequenceToJson(const LevelSequence& sequence);
bool SequenceFromJson(const nlohmann::json& json, LevelSequence& out, std::string* error = nullptr);
bool LoadSequence(const std::string& file, LevelSequence& out, std::string* error = nullptr);
bool SaveSequence(const std::string& file, const LevelSequence& sequence, std::string* error = nullptr);

} // namespace aether::seq
