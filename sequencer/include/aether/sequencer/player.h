#pragma once

#include "aether/ecs/world.h"
#include "aether/reflection/reflection.h"
#include "aether/scene/entity_guid.h"
#include "aether/sequencer/sequence.h"

#include <functional>
#include <string>
#include <vector>

// Playing a level sequence (Phase 27 step 1, §27.1): the playhead, and
// applying each track's value at the playhead to the world. Evaluation is a
// pure function of time (scrubbing back and forward gives the same result),
// and allocates nothing after Bind().

namespace aether::seq {

class SequencePlayer {
public:
    // `sequence`, `world` and `guids` must outlive the player.
    SequencePlayer(const LevelSequence& sequence, World& world, const GuidIndex& guids);
    ~SequencePlayer();
    SequencePlayer(const SequencePlayer&) = delete;
    SequencePlayer& operator=(const SequencePlayer&) = delete;

    // Resolves every track's entity and, for a Property track, its component
    // and field; clears the problems and reports what couldn't be resolved.
    // Called again after the scene's entities change (a track whose entity
    // died is also re-resolved by itself when it is next applied).
    void Bind();

    // The playhead, in seconds (clamped to 0..duration).
    void SetTime(f32 time);
    f32 Time() const { return time_; }
    f32 Duration() const { return duration_; }
    // Applies every unmuted track at the playhead.
    void Evaluate();

    void Play() { playing_ = true; }
    void Pause() { playing_ = false; }
    // Stops and returns to the start (the world keeps what was last applied).
    void Stop();
    bool Playing() const { return playing_; }
    void SetRate(f32 rate) { rate_ = rate; } // negative plays backward
    f32 Rate() const { return rate_; }
    bool loop = false;
    // Called once each time playing reaches the end (or the start, backward);
    // a looping sequence calls it at every wrap.
    std::function<void()> on_finished;

    // Advances a playing sequence by `dt` seconds, fires the event keys the
    // playhead crossed and applies it. Events fire once each, forward only
    // (scrubbing, `SetTime` and playing backward fire none); a loop fires the
    // keys at the end and then those at the start.
    void Update(f32 dt);

    // Moves the playhead forward to `time` (clamped to the duration) in one
    // step, firing the Event, Audio and Animation keys on the way, then applies
    // the tracks. For rendering frame by frame: the keys of each frame fire
    // exactly once however the frames fall. Going backward fires nothing.
    void AdvanceTo(f32 time);

    // Called for each Event key crossed, in time order.
    std::function<void(const Track&, const EventKey&)> on_event;
    // Audio and Animation keys, fired like events (forward only, once each).
    // `entity` is the track's bound entity (null for none).
    std::function<void(const Track&, Entity entity, const AudioKey&)> on_audio;
    std::function<void(const Track&, Entity entity, const AnimKey&)> on_animation;
    // Spawn tracks: `on_spawn` makes the key's prefab (placed relative to
    // `parent`, null for none) and returns its root; `on_despawn` removes it.
    // The player keeps the handle, so a key is alive exactly while the
    // playhead is inside its range: leaving it (or Stop, or destroying the
    // player) despawns, scrubbing back in respawns.
    std::function<Entity(const Track&, Entity parent, const SpawnKey&)> on_spawn;
    std::function<void(Entity)> on_despawn;
    // Camera Cut tracks: the cut in force at the playhead makes its camera
    // the highest-priority one (kCutPriority) so FindActiveCamera picks it;
    // the previous one gets its own priority back. `on_camera_cut` is also
    // told, with kNullEntity before the first cut.
    static constexpr i32 kCutPriority = 1000;
    std::function<void(Entity)> on_camera_cut;
    // How a Visibility track shows or hides its entity; called only when the
    // value changes. The default sets the entity's Active component (adding
    // one if it has none); a host can route it through Lifecycle::SetActive.
    std::function<void(Entity, bool)> set_active;

    // What went wrong binding or applying, each reported once: a missing
    // entity or component, an unknown field, a field type that can't be keyed.
    const std::vector<std::string>& Problems() const { return problems_; }

private:
    struct Target {
        Entity entity;
        ComponentId component = kInvalidComponentId; // Property
        const reflect::TypeInfo* field_type = nullptr;
        u32 field_offset = 0;
        std::vector<Entity> spawned; // Spawn: the live root of each key (null when not alive)
        std::vector<char> spawn_done; // Spawn: this key's range was entered and handled (so a destroyed spawn isn't remade while inside)
        int cut = -2;         // CameraCut: the index of the cut in force (-1 before the first, -2 none yet)
        Entity cut_camera;    // CameraCut: the camera holding the cut priority
        i32 cut_saved = 0;    // ... and the priority it had
        int shown = -1;       // Visibility: the last value applied (-1 none yet)
        bool bound = false;   // resolved and usable
        bool reported = false; // its problem has been reported
    };
    bool ResolveTrack(usize index);
    void Report(usize index, const std::string& message);
    void ApplyTransform(const Track& track, Target& target);
    void ApplyProperty(const Track& track, Target& target);
    void ApplyVisibility(const Track& track, Target& target);
    void ApplySpawns(const Track& track, Target& target);
    void ApplyCuts(const Track& track, Target& target);
    void ReleaseCut(Target& target);
    void DespawnAll();
    // Fires the Event, Audio and Animation keys in (from, to] (or [from, to]
    // when `include_from`).
    void FireEvents(f32 from, f32 to, bool include_from);

    const LevelSequence& sequence_;
    World& world_;
    const GuidIndex& guids_;
    std::vector<Target> targets_;
    std::vector<std::string> problems_;
    f32 time_ = 0.0f;
    f32 duration_ = 0.0f;
    f32 rate_ = 1.0f;
    bool playing_ = false;
    bool fresh_ = true; // nothing played yet from the start: an event at t = 0 still fires
};

} // namespace aether::seq
