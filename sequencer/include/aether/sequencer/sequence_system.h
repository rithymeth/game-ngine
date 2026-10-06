#pragma once

#include "aether/ecs/world.h"
#include "aether/reflection/reflection.h"
#include "aether/scene/entity_guid.h"
#include "aether/scene/lifecycle.h"
#include "aether/sequencer/player.h"

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

// Sequences in a scene (Phase 27 step 2, docs/design/PHASE_SPECS.md §27.2): a
// SequenceComponent plays a .asequence from an entity, the SequenceSystem
// runs them each frame, and the component's methods are Blueprint nodes
// (and Luau calls) the way AudioSource's are.

namespace aether {

// A request from gameplay or Blueprints, applied by the next SequenceSystem::Update.
struct SequenceCommand {
    enum class Kind : u8 { Play, Pause, Stop, SetTime, SetRate, SetLoop };
    Kind kind = Kind::Play;
    f32 value = 0.0f;
};

// Plays a level sequence from an entity. The methods queue commands for the
// sequence system; `time` and `playing` are written back by it (not saved).
struct SequenceComponent {
    std::string sequence;  // the sequence asset (.asequence)
    bool auto_play = false; // start when the entity appears
    bool loop = false;
    f32 rate = 1.0f;       // negative plays backward
    bool destroy_when_finished = false; // the entity goes when the sequence ends
    f32 time = 0.0f;
    bool playing = false;
    std::vector<SequenceCommand> commands; // not saved

    void Play();
    void Pause();
    void Stop();
    void SetTime(f32 seconds);
    void SetRate(f32 rate);
    void SetLoop(bool loop);
    bool IsPlaying() const { return playing; }
    f32 GetTime() const { return time; }
};

// Blueprint function library: play a sequence without placing a component.
// Acts on the active SequenceSystem, and does nothing without one.
struct Sequencer {
    static void PlaySequence(const std::string& sequence, bool loop);
    static void StopAll();
};

// Registers SequenceComponent with the ECS (idempotent).
void RegisterSequenceComponents();

} // namespace aether

namespace aether::seq {

// What a playing sequence reported in the last Update: a Marker is an Event
// key the playhead crossed; Audio and Animation are those keys crossed (for
// the host to act on with its audio and animation systems); Finished is the
// sequence reaching its end.
struct SequenceEvent {
    enum class Kind : u8 { Marker, Finished, Audio, Animation };
    Kind kind = Kind::Marker;
    Entity entity;       // the SequenceComponent's entity
    std::string name;    // Marker: the key's name; Finished: the sequence asset; Audio: the cue; Animation: the montage
    std::string payload; // Marker: the key's payload; Audio and Animation: the action ("play", "stop", "fade_in", "fade_out")
    Entity subject;      // Marker, Audio, Animation: the track's bound entity, if any
    f32 value = 0.0f;    // Audio: volume in dB; Animation: the rate
    f32 fade = 0.0f;     // Audio: fade seconds
};

// Makes a Spawn key's prefab (placed relative to `parent`, null for none) and
// returns its root, or null.
using SequenceSpawner = std::function<Entity(const Track&, Entity parent, const SpawnKey&)>;
using SequenceLookup = std::function<const LevelSequence*(const std::string&)>;

// Runs every SequenceComponent: a player each (made when the component
// appears, with auto_play starting it), queued commands, time written back,
// Visibility tracks through Lifecycle::SetActive when given one, and events
// collected for the host to dispatch.
class SequenceSystem {
public:
    // `find` returns a sequence by asset path, or null. With `lifecycle`,
    // visibility changes and destroy_when_finished go through it (so
    // OnEnable/OnDisable fire).
    SequenceSystem(World& world, GuidIndex& guids, SequenceLookup find, Lifecycle* lifecycle = nullptr);
    ~SequenceSystem();
    SequenceSystem(const SequenceSystem&) = delete;
    SequenceSystem& operator=(const SequenceSystem&) = delete;

    void Update(f32 dt);

    // How Spawn tracks make their prefab (the host instantiates it); without
    // one they spawn nothing. Despawning goes through Lifecycle when given.
    void SetSpawner(SequenceSpawner spawner) { spawner_ = std::move(spawner); }

    // What the Sequencer library calls: a new entity that plays `sequence`
    // and is destroyed when it ends. Null if the sequence can't be found.
    Entity PlaySequence(const std::string& sequence, bool loop = false);
    void StopAll();

    const std::vector<SequenceEvent>& Events() const { return events_; }
    // The strongest fade any playing sequence asks for this frame (Fade
    // tracks): amount 0 is none. Drawing it over the screen is the host's.
    const FadeState& Fade() const { return fade_; }
    // Missing sequences and the players' own problems, reported once each.
    const std::vector<std::string>& Problems() const { return problems_; }
    // The player behind a component's entity (null before its first Update).
    SequencePlayer* PlayerOf(Entity entity);

    static SequenceSystem* Active();
    void MakeActive();

    // The Blueprint events, dispatched by the host on the component's
    // entity: finished (argument: the sequence) and an Event key crossed
    // (arguments: its name, its payload).
    static constexpr const char* kFinishedEvent = "Event.OnSequenceFinished";
    static constexpr const char* kMarkerEvent = "Event.OnSequenceEvent";

private:
    struct Slot {
        Entity entity;
        std::string path;
        const LevelSequence* sequence = nullptr;
        std::unique_ptr<SequencePlayer> player;
        bool finished = false; // set by the player's callback during Update
        u64 seen = 0;
    };
    void Report(const std::string& message);

    World& world_;
    GuidIndex& guids_;
    SequenceLookup find_;
    SequenceSpawner spawner_;
    Lifecycle* lifecycle_;
    std::map<u32, Slot> slots_; // by entity index
    std::vector<SequenceEvent> events_;
    FadeState fade_;
    std::vector<std::string> problems_;
    std::map<std::string, bool> reported_;
    u64 generation_ = 0;
};

} // namespace aether::seq

AETHER_REFLECT(aether::SequenceComponent, 1,
    AETHER_FIELD(sequence, Field_EditAnywhere, {.tooltip = "The level sequence to play (.asequence)"}),
    AETHER_FIELD(auto_play, Field_EditAnywhere, {.tooltip = "Start playing when the entity appears"}),
    AETHER_FIELD(loop, Field_EditAnywhere),
    AETHER_FIELD(rate, Field_EditAnywhere, {.tooltip = "Playback speed; negative plays backward", .range_min = -4.0, .range_max = 4.0}),
    AETHER_FIELD(destroy_when_finished, Field_EditAnywhere),
    AETHER_METHOD(Play, Fn_BlueprintCallable),
    AETHER_METHOD(Pause, Fn_BlueprintCallable),
    AETHER_METHOD(Stop, Fn_BlueprintCallable),
    AETHER_METHOD(SetTime, Fn_BlueprintCallable, {"seconds"}),
    AETHER_METHOD(SetRate, Fn_BlueprintCallable, {"rate"}),
    AETHER_METHOD(SetLoop, Fn_BlueprintCallable, {"loop"}),
    AETHER_METHOD(IsPlaying, Fn_BlueprintCallable | Fn_Pure),
    AETHER_METHOD(GetTime, Fn_BlueprintCallable | Fn_Pure)
)

AETHER_REFLECT(aether::Sequencer, 1,
    AETHER_METHOD(PlaySequence, Fn_BlueprintCallable, {"sequence", "loop"}),
    AETHER_METHOD(StopAll, Fn_BlueprintCallable)
)
