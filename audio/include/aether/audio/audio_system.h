#pragma once

#include "aether/audio/cue_player.h"
#include "aether/ecs/world.h"
#include "aether/reflection/reflection.h"
#include "aether/scene/entity_guid.h"

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace aether {

// A request from gameplay or Blueprints, applied by the next AudioSystem::Update.
struct AudioSourceCommand {
    enum class Kind : u8 { Play, Stop, FadeIn, FadeOut, SetVolume, SetCue };
    Kind kind = Kind::Play;
    f32 value = 0.0f;
    std::string text;
};

// Plays a sound cue from an entity (Phase 17 step 4, docs/design/PHASE_SPECS.md
// §17.4). A spatial cue follows the entity's world position. The methods
// are Blueprint nodes; they queue commands for the audio system.
struct AudioSource {
    std::string cue;       // the sound cue asset (.acue)
    bool auto_play = true; // start when the entity appears
    f32 volume_db = 0.0f;
    f32 pitch = 1.0f;
    bool playing = false;                  // set by the audio system; not saved
    std::vector<AudioSourceCommand> commands; // not saved

    void Play();
    void Stop();
    void FadeIn(f32 seconds);
    void FadeOut(f32 seconds);
    void SetVolume(f32 volume_db);
    void SetCue(const std::string& cue);
    bool IsPlaying() const { return playing; }
};

// Where the player hears from: the entity's world position and facing
// (-Z forward, +Y up). The first active one (lowest entity index) is used.
struct AudioListener {
    bool active = true;
};

// A space whose reverb the listener hears inside it: full strength within
// `radius` of the entity, fading out over `blend_distance` beyond. Where zones
// overlap, the highest priority wins, then the strongest.
struct ReverbZone {
    f32 radius = 10.0f;
    f32 blend_distance = 2.0f;
    i32 priority = 0;
    f32 room_size = 0.7f; // 0..1
    f32 damping = 0.5f;   // 0..1
    f32 wet = 0.35f;      // at full strength
};

// Blueprint function library: sounds that aren't a component's (Play Sound
// 2D, Play Sound at Location, Spawn Sound Attached) and the mixer's buses.
// They act on the active AudioSystem, and do nothing without one.
struct Audio {
    static void PlaySound2D(const std::string& cue, f32 volume_db, f32 pitch);
    static void PlaySoundAtLocation(const std::string& cue, const Vec3& location, f32 volume_db, f32 pitch);
    static void SpawnSoundAttached(const std::string& cue, const Entity& target, const Vec3& offset, f32 volume_db);
    static void SetBusVolume(const std::string& bus, f32 volume_db, f32 fade_seconds);
    static void StopAllSounds();
};

// Registers AudioSource, AudioListener and ReverbZone with the ECS (idempotent).
void RegisterAudioComponents();

} // namespace aether

namespace aether::audio {

struct AudioFinishedEvent {
    Entity entity;
    std::string cue;
};

// Runs the audio components on a mixer: the listener, each AudioSource's
// cue (following its entity), attached sounds, bus fades, reverb zones and
// occlusion. Rendering is the output backend's (step 5) or the caller's.
class AudioSystem {
public:
    // `cues` finds cue assets by name. With `guids`, positions follow
    // parents (scene hierarchy); without, each Transform is in world space.
    AudioSystem(World& world, Mixer& mixer, const SoundBank& bank, std::function<const SoundCue*(const std::string&)> cues,
                const GuidIndex* guids = nullptr);
    ~AudioSystem();
    AudioSystem(const AudioSystem&) = delete;
    AudioSystem& operator=(const AudioSystem&) = delete;

    void Update(f32 dt);

    // What the Audio library's Blueprint nodes call. 0 when the cue doesn't exist or plays nothing.
    CueHandle PlaySound2D(const std::string& cue, f32 volume_db = 0.0f, f32 pitch = 1.0f);
    CueHandle PlaySoundAtLocation(const std::string& cue, const Vec3& location, f32 volume_db = 0.0f, f32 pitch = 1.0f);
    // Follows `target` (plus `offset` in its space) and stops if it's destroyed.
    CueHandle SpawnSoundAttached(const std::string& cue, Entity target, const Vec3& offset = {}, f32 volume_db = 0.0f);
    // Changes the bus's volume over `fade_seconds` (linearly in dB).
    bool SetBusVolume(const std::string& bus, f32 volume_db, f32 fade_seconds = 0.0f);
    void StopAll();

    CueHandle HandleOf(Entity source) const; // an AudioSource's playing cue
    const Listener& GetListener() const { return mixer_.GetListener(); }
    Entity ListenerEntity() const { return listener_entity_; }
    // The reverb zones' effect on `reverb_bus`: whether it exists (once any
    // zone has), the zone's strength where the listener is (0..1), and what
    // it was last set to. (The effect itself belongs to the mixer's thread.)
    bool HasReverb() const { return reverb_ != nullptr; }
    f32 ReverbStrength() const { return reverb_strength_; }
    f32 ReverbWet() const { return reverb_wet_; }
    f32 ReverbRoomSize() const { return reverb_room_size_; }
    std::string reverb_bus = "SFX"; // Master if the mixer has no such bus
    // AudioSources whose cue ended by itself in the last Update.
    const std::vector<AudioFinishedEvent>& Events() const { return events_; }
    // Missing cues and invalid ones, reported once each.
    const std::vector<std::string>& Problems() const { return problems_; }
    CuePlayer& Player() { return player_; }
    Mixer& GetMixer() { return mixer_; }

    // The system the Audio library acts on: the last one made, until it's destroyed.
    static AudioSystem* Active();
    void MakeActive();

    // The Blueprint event AudioSources' finished cues are dispatched as (argument: the cue).
    static constexpr const char* kFinishedEvent = "Event.OnAudioFinished";

private:
    struct Source {
        Entity entity;
        std::string cue;
        CueHandle handle = 0;
        Vec3 last_position;
        u64 seen = 0;
    };
    struct Attached {
        CueHandle handle = 0;
        Entity target;
        Vec3 offset, last_position;
    };
    struct BusFade {
        BusId bus = 0;
        f32 from = 0.0f, to = 0.0f, duration = 0.0f, elapsed = 0.0f;
    };
    const SoundCue* FindCue(const std::string& name);
    CueHandle Start(const std::string& cue, const CuePlayParams& params);
    bool WorldPose(Entity e, Vec3& position, Quaternion& rotation) const;
    void UpdateListener(f32 dt);
    void UpdateSources(f32 dt);
    void UpdateReverb();

    World& world_;
    Mixer& mixer_;
    const SoundBank& bank_;
    std::function<const SoundCue*(const std::string&)> cues_;
    const GuidIndex* guids_;
    CuePlayer player_;
    std::map<u32, Source> sources_; // by entity index
    std::vector<Attached> attached_;
    std::vector<BusFade> fades_;
    std::vector<AudioFinishedEvent> events_;
    std::vector<std::string> problems_;
    std::map<std::string, bool> reported_;
    Entity listener_entity_;
    Vec3 listener_last_;
    bool listener_seen_ = false;
    ReverbEffect* reverb_ = nullptr; // changed only through Mixer::Post
    f32 reverb_strength_ = 0.0f, reverb_wet_ = 0.0f, reverb_room_size_ = 0.0f, reverb_damping_ = 0.0f;
    void SetReverb(f32 wet, f32 room_size, f32 damping);
    u64 generation_ = 0;
};

} // namespace aether::audio

AETHER_REFLECT(aether::AudioSource, 1,
    AETHER_FIELD(cue, Field_EditAnywhere, {.tooltip = "The sound cue to play (.acue)"}),
    AETHER_FIELD(auto_play, Field_EditAnywhere, {.tooltip = "Start playing when the entity appears"}),
    AETHER_FIELD(volume_db, Field_EditAnywhere, {.range_min = -80.0, .range_max = 12.0, .units = "dB"}),
    AETHER_FIELD(pitch, Field_EditAnywhere, {.range_min = 0.1, .range_max = 4.0}),
    AETHER_METHOD(Play, Fn_BlueprintCallable),
    AETHER_METHOD(Stop, Fn_BlueprintCallable),
    AETHER_METHOD(FadeIn, Fn_BlueprintCallable, {"seconds"}),
    AETHER_METHOD(FadeOut, Fn_BlueprintCallable, {"seconds"}),
    AETHER_METHOD(SetVolume, Fn_BlueprintCallable, {"volume_db"}),
    AETHER_METHOD(SetCue, Fn_BlueprintCallable, {"cue"}),
    AETHER_METHOD(IsPlaying, Fn_BlueprintCallable | Fn_Pure)
)

AETHER_REFLECT(aether::AudioListener, 1,
    AETHER_FIELD(active, Field_EditAnywhere, {.tooltip = "Hear the world from this entity"})
)

AETHER_REFLECT(aether::ReverbZone, 1,
    AETHER_FIELD(radius, Field_EditAnywhere, {.range_min = 0.0, .range_max = 1000.0, .units = "m"}),
    AETHER_FIELD(blend_distance, Field_EditAnywhere, {.range_min = 0.0, .range_max = 100.0, .units = "m"}),
    AETHER_FIELD(priority, Field_EditAnywhere),
    AETHER_FIELD(room_size, Field_EditAnywhere, {.range_min = 0.0, .range_max = 1.0}),
    AETHER_FIELD(damping, Field_EditAnywhere, {.range_min = 0.0, .range_max = 1.0}),
    AETHER_FIELD(wet, Field_EditAnywhere, {.range_min = 0.0, .range_max = 1.0})
)

AETHER_REFLECT(aether::Audio, 1,
    AETHER_METHOD(PlaySound2D, Fn_BlueprintCallable, {"cue", "volume_db", "pitch"}),
    AETHER_METHOD(PlaySoundAtLocation, Fn_BlueprintCallable, {"cue", "location", "volume_db", "pitch"}),
    AETHER_METHOD(SpawnSoundAttached, Fn_BlueprintCallable, {"cue", "target", "offset", "volume_db"}),
    AETHER_METHOD(SetBusVolume, Fn_BlueprintCallable, {"bus", "volume_db", "fade_seconds"}),
    AETHER_METHOD(StopAllSounds, Fn_BlueprintCallable)
)
