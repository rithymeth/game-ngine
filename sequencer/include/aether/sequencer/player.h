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

    // Advances a playing sequence by `dt` seconds and applies it.
    void Update(f32 dt);

    // What went wrong binding or applying, each reported once: a missing
    // entity or component, an unknown field, a field type that can't be keyed.
    const std::vector<std::string>& Problems() const { return problems_; }

private:
    struct Target {
        Entity entity;
        ComponentId component = kInvalidComponentId; // Property
        const reflect::TypeInfo* field_type = nullptr;
        u32 field_offset = 0;
        bool bound = false;   // resolved and usable
        bool reported = false; // its problem has been reported
    };
    bool ResolveTrack(usize index);
    void Report(usize index, const std::string& message);
    void ApplyTransform(const Track& track, Target& target);
    void ApplyProperty(const Track& track, Target& target);

    const LevelSequence& sequence_;
    World& world_;
    const GuidIndex& guids_;
    std::vector<Target> targets_;
    std::vector<std::string> problems_;
    f32 time_ = 0.0f;
    f32 duration_ = 0.0f;
    f32 rate_ = 1.0f;
    bool playing_ = false;
};

} // namespace aether::seq
