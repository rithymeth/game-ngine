#pragma once

#include "aether/ecs/world.h"
#include "aether/reflection/reflection.h"
#include "aether/scene/entity_guid.h"
#include "aether/sequencer/sequence.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

// Playing a level sequence (Phase 27 step 1, §27.1): the playhead, and
// applying each track's value at the playhead to the world. Evaluation is a
// pure function of time (scrubbing back and forward gives the same result),
// and allocates nothing after Bind().

namespace aether::seq {

// Finds a sequence by asset path, for Subsequence tracks. The sequences it
// returns must outlive the player.
using SequenceResolver = std::function<const LevelSequence*(const std::string&)>;

// What a Fade track asks the screen to show: `amount` 0 (nothing) to 1 (all
// of `color`).
struct FadeState {
    f32 amount = 0.0f;
    Vec4 color{0.0f, 0.0f, 0.0f, 1.0f};
    bool operator==(const FadeState& o) const { return amount == o.amount && color.x == o.color.x && color.y == o.color.y && color.z == o.color.z && color.w == o.color.w; }
};

class SequencePlayer {
public:
    // `sequence`, `world` and `guids` must outlive the player. `resolver`
    // finds the sequences Subsequence tracks play (without one they report a
    // problem and play nothing); `path` is this sequence's own asset path,
    // which lets a sequence that plays itself be caught at once.
    SequencePlayer(const LevelSequence& sequence, World& world, const GuidIndex& guids, SequenceResolver resolver = {}, std::string path = {});
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

    void Play() { playing_ = true; completed_ = false; }
    void Pause() { playing_ = false; }
    // Stops and returns to the start (the world keeps what was last applied).
    void Stop();
    // Finishes from the current position without firing intermediate Event,
    // Audio or Animation keys. Applies the final pose, releases temporary
    // spawns/camera cuts/fade, stops active cues and montages, and calls
    // on_finished once.
    void Skip();
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
    // `entity` is the track's bound entity (null for none). Active cues and
    // montages receive a synthetic Stop when the player is deactivated.
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

    // Fade tracks: the strongest fade at the playhead (the highest amount
    // among the unmuted Fade tracks and the sequences playing inside this one;
    // its colour). `on_fade` is called when it changes. Stop clears it.
    const FadeState& Fade() const { return fade_; }
    std::function<void(const FadeState&)> on_fade;
    static constexpr int kMaxSubsequenceDepth = 4;

    // What went wrong binding or applying, each reported once: a missing
    // entity or component, an unknown field, a field type that can't be keyed.
    const std::vector<std::string>& Problems() const { return problems_; }

private:
    struct ActiveAudio {
        std::string cue;
        Entity entity;
    };
    struct ActiveAnimation {
        std::string montage;
        Entity entity;
        f32 rate = 1.0f;
    };
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
        std::vector<std::unique_ptr<SequencePlayer>> children; // Subsequence: one per key (null when it couldn't be made)
        std::vector<char> child_active; // Subsequence: the playhead is inside this key's range
        std::unique_ptr<Track> cleanup_track; // Audio/Animation: survives an editor edit before preview teardown
        std::vector<ActiveAudio> active_audio; // cues and the subjects that received Play
        std::vector<ActiveAnimation> active_animation; // montages and the subjects that received Play
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
    void ApplySubsequences(const Track& track, Target& target);
    void Deactivate(); // despawns, stops cues/montages, releases cuts and fade
    void ReleaseCut(Target& target);
    void DespawnAll();
    SequencePlayer(const LevelSequence& sequence, World& world, const GuidIndex& guids, SequenceResolver resolver, std::vector<std::string> path);
    // Fires the Event, Audio and Animation keys in (from, to] (or [from, to]
    // when `include_from`).
    void FireEvents(f32 from, f32 to, bool include_from);

    const LevelSequence& sequence_;
    World& world_;
    const GuidIndex& guids_;
    SequenceResolver resolver_;
    std::vector<std::string> path_; // the sequences this one is nested in, outermost first, then itself (when known)
    FadeState fade_;
    FadeState pending_fade_;
    bool final_pose_ = false; // evaluating a child's first or last pose as it leaves its range: no spawns, cuts or hooks
    std::vector<Target> targets_;
    std::vector<std::string> problems_;
    f32 time_ = 0.0f;
    f32 duration_ = 0.0f;
    f32 rate_ = 1.0f;
    bool playing_ = false;
    bool completed_ = false;
    bool fresh_ = true; // nothing played yet from the start: an event at t = 0 still fires
};

} // namespace aether::seq
