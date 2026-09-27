#pragma once

#include "aether/audio/dsp.h"
#include "aether/audio/sound.h"
#include "aether/audio/spatial.h"
#include "aether/audio/spsc_queue.h"
#include "aether/audio/stream.h"

#include <algorithm>
#include <atomic>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace aether::audio {

// The software mixer (Phase 17 step 1, docs/design/PHASE_SPECS.md §17.1):
// voices play sounds into buses, buses run their effects and mix into their
// parents up to Master, and Render writes interleaved stereo. By default
// everything happens on one thread; SetThreaded (step 5) splits it between
// the game thread and an audio thread that renders.

using BusId = u32;
using VoiceId = u32; // 0 is no voice
constexpr BusId kMasterBus = 0;

// What happens to a voice that loses its channel to the voice limit or goes
// inaudible.
enum class VirtualMode : u8 {
    Continue, // keeps time silently and comes back where it would have been
    Restart,  // comes back from its start
    Stop,     // ends
};

struct PlayParams {
    BusId bus = kMasterBus;
    f32 volume_db = 0.0f;
    f32 pitch = 1.0f;   // playback rate multiplier (2 = an octave up)
    f32 pan = 0.0f;     // -1 left .. 1 right
    bool loop = false;
    f32 start_time = 0.0f; // seconds into the sound
    f32 fade_in = 0.0f;    // seconds
    f64 delay = 0.0;       // seconds after the next Render starts, sample-accurate (step 3)
    // Voice limiting: higher priorities keep their channels (0..255).
    u8 priority = 128;
    VirtualMode virtual_mode = VirtualMode::Continue;
    // 3D (step 2). Off: a plain 2D voice.
    bool spatial = false;
    Vec3 position{}, velocity{};
    f32 spatial_blend = 1.0f; // 0 = 2D (pan above, no falloff) .. 1 = fully 3D
    AttenuationSettings attenuation{};
    f32 doppler = 1.0f;       // doppler factor; 0 turns it off
    bool occlusion = false;   // ask the occlusion hook about this voice
};

// A voice's current state, for tests, the debugger and the editor.
struct VoiceInfo {
    bool is_virtual = false;
    f32 distance = 0.0f;
    f32 attenuation = 1.0f; // distance gain after spatial blend
    f32 occlusion = 0.0f;   // smoothed, 0..1
    f32 pan = 0.0f;         // after spatial blend
    f32 pitch = 1.0f;       // including doppler
    f32 lowpass_hz = kOpenCutoff;
    f32 audibility = 1.0f;  // the gain the voice would reach Master with
};

struct BusMeter {
    f32 peak[2] = {0, 0}; // the last Render's, linear
    f32 rms[2] = {0, 0};
};

class Mixer {
public:
    explicit Mixer(u32 sample_rate = 48000);
    ~Mixer();
    Mixer(const Mixer&) = delete;
    Mixer& operator=(const Mixer&) = delete;

    u32 SampleRate() const { return sample_rate_; }

    // --- Buses ------------------------------------------------------------------
    // Master exists from the start; parents come before children. Buses are
    // made before the mixer is threaded (kInvalidBus after).
    BusId AddBus(const std::string& name, BusId parent = kMasterBus);
    // Master with Music, SFX, UI and Voice under it.
    void AddDefaultBuses();
    BusId FindBus(const std::string& name) const; // kInvalidBus if none
    static constexpr BusId kInvalidBus = ~0u;
    usize BusCount() const { return buses_.size(); }
    const std::string& BusName(BusId bus) const { return buses_[bus].name; }
    BusId BusParent(BusId bus) const { return buses_[bus].parent; }
    bool SetBusVolume(BusId bus, f32 db);
    f32 BusVolume(BusId bus) const;
    bool SetBusMuted(BusId bus, bool muted);
    bool BusMuted(BusId bus) const;
    // Once threaded, change the returned effect's settings only through Post.
    AudioEffect* AddEffect(BusId bus, std::unique_ptr<AudioEffect> effect);
    BusMeter Meter(BusId bus) const;

    // --- Voices -----------------------------------------------------------------
    // The sound must outlive the voice. Returns 0 for an empty sound or a bad bus.
    VoiceId Play(const SoundWave* sound, const PlayParams& params = {});
    // Plays a stream, decoding as it goes (step 3). The voice owns it.
    VoiceId PlayStream(std::unique_ptr<AudioStream> stream, const PlayParams& params = {});
    // Stops now, or fades out over `fade_out` seconds first.
    bool Stop(VoiceId voice, f32 fade_out = 0.0f);
    void StopAll();
    bool SetVolume(VoiceId voice, f32 db);
    bool SetPitch(VoiceId voice, f32 pitch);
    bool SetPan(VoiceId voice, f32 pan);
    bool IsPlaying(VoiceId voice) const;
    f32 PlaybackTime(VoiceId voice) const; // seconds into the sound; -1 if not playing
    usize VoiceCount() const;
    bool GetVoiceInfo(VoiceId voice, VoiceInfo& out) const;

    // --- 3D (step 2) --------------------------------------------------------------
    void SetListener(const Listener& listener);
    const Listener& GetListener() const { return threaded_ ? game_listener_ : listener_; }
    bool SetPosition(VoiceId voice, const Vec3& position, const Vec3& velocity = {});
    f32 speed_of_sound = 343.0f; // world units per second
    // Occlusion: how blocked the path from the listener to a source is, 0..1
    // (a physics raycast, typically). Called by UpdateOcclusion on the game
    // thread for voices that ask for it; the result is smoothed.
    std::function<f32(const Vec3& listener, const Vec3& source)> occlusion_query;
    void UpdateOcclusion();
    bool SetOcclusion(VoiceId voice, f32 amount); // for hosts that compute it themselves
    f32 occlusion_volume_db = -12.0f;   // at full occlusion
    f32 occlusion_lowpass_hz = 1200.0f; // at full occlusion
    f32 occlusion_smoothing = 0.1f;     // seconds to follow a change

    // --- Voice limiting (step 2) ----------------------------------------------------
    // At most this many voices mix; the rest go virtual by priority, then
    // audibility. Voices quieter than the threshold go virtual regardless.
    void SetMaxVoices(u32 count);
    u32 MaxVoices() const { return game_max_voices_; }
    f32 virtual_threshold_db = -80.0f;
    usize RealVoiceCount() const;
    usize VirtualVoiceCount() const;

    // Mixes `frames` of interleaved stereo into `out` (overwriting it).
    void Render(f32* out, u32 frames);
    // The mixer's clock: frames rendered so far.
    u64 FramesRendered() const;

    // --- Threading (step 5) -----------------------------------------------------------
    // Threaded, one audio thread calls Render while the game thread calls the
    // rest. Changes become commands, handed over without locks and applied in
    // order at the start of the next Render. Queries read what the last
    // Render published, plus the game thread's own pending changes (a voice
    // just played counts as playing). Plays remember the game's clock, so
    // delays stay sample-accurate if they cover the output's latency. The
    // public fields (speed_of_sound, ...) are set before threading or through
    // Post. Switch only while nothing is rendering.
    void SetThreaded(bool threaded);
    bool Threaded() const { return threaded_; }
    // Runs `edit` on the mixer: now, or on the audio thread before the next Render.
    void Post(std::function<void(Mixer&)> edit);

private:
    struct Bus {
        std::string name;
        BusId parent = kMasterBus;
        f32 volume_db = 0.0f;
        f32 gain = 1.0f, current_gain = 1.0f; // target and smoothed
        bool muted = false;
        std::vector<std::unique_ptr<AudioEffect>> effects;
        std::vector<f32> buffer;
        BusMeter meter;
    };
    struct Voice {
        VoiceId id = 0;
        const SoundWave* sound = nullptr;
        // Streamed voices: `position` counts frames from the start without
        // wrapping at loops, and `window` holds decoded frames from `window_start`.
        std::shared_ptr<AudioStream> stream;
        std::vector<f32> window;
        i64 window_start = 0;
        u64 delay_frames = 0;
        BusId bus = kMasterBus;
        f64 position = 0.0; // in source frames
        f32 pitch = 1.0f;
        f32 gain = 1.0f, current_gain = 1.0f;
        f32 pan = 0.0f;
        bool loop = false;
        f32 fade = 1.0f, fade_step = 0.0f; // per output frame; negative while fading out
        bool stopping = false;
        u8 priority = 128;
        VirtualMode virtual_mode = VirtualMode::Continue;
        bool is_virtual = false, was_virtual = false;
        bool spatial = false, occlusion = false;
        Vec3 position3d, velocity;
        f32 spatial_blend = 1.0f, doppler = 1.0f;
        f32 spatial_gain = 1.0f; // attenuation times occlusion, this block
        bool started = false;    // has been through a Render
        AttenuationSettings attenuation;
        f32 occlusion_target = 0.0f;
        VoiceInfo info;              // this block's spatial results
        f32 lowpass[2] = {0.0f, 0.0f}; // one-pole state per channel
    };
    Voice* Find(VoiceId id);
    const Voice* Find(VoiceId id) const;
    // Adds the voice's next `frames` to `out` (stereo); false once it has finished.
    // `ramp_from`/`ramp_to` scale the block linearly, to fade voices in and
    // out of virtual without a click.
    bool MixVoice(Voice& v, f32* out, u32 frames, f32 ramp_from, f32 ramp_to);
    // Advances a virtual voice without mixing it; false once it has finished.
    bool AdvanceVirtual(Voice& v, u32 frames);
    void Spatialize(Voice& v, u32 frames, const std::vector<f32>& bus_gains);
    // `issued_at`: the game's clock when it asked (threaded), to take off the delay.
    void Start(Voice v, const PlayParams& params, VoiceId id, u64 issued_at);
    VoiceId NewId();
    bool DoStop(VoiceId voice, f32 fade_out);
    bool DoSetBusVolume(BusId bus, f32 db);

    // Threading.
    struct Command {
        u64 seq = 0;
        std::function<void(Mixer&)> apply;
    };
    struct Snapshot {
        struct VoiceState {
            VoiceId id = 0;
            VoiceInfo info;
            f32 time = 0.0f;
        };
        u64 applied = 0; // the last command applied
        u64 frames = 0;
        std::vector<VoiceState> voices; // by id
        std::vector<BusMeter> meters;
        usize real = 0;
    };
    u64 Submit(std::function<void(Mixer&)> apply); // game thread
    void ApplyCommands();                          // audio thread
    void Publish();                                // audio thread
    const Snapshot& View() const;                  // game thread
    const Snapshot::VoiceState* Seen(VoiceId id) const;
    bool PendingStop(VoiceId id) const;
    void CollectGarbage() const;
    // Decodes a streamed voice's frames [from, to) into its window.
    static void Fill(Voice& v, i64 from, i64 to);
    static u64 Frames(const Voice& v) { return v.stream ? v.stream->Frames() : v.sound->Frames(); }
    static u32 Rate(const Voice& v) { return v.stream ? v.stream->SampleRate() : v.sound->sample_rate; }
    static u32 Channels(const Voice& v) { return v.stream ? v.stream->Channels() : v.sound->channels; }
    void AssignChannels();

    u32 sample_rate_;
    std::vector<Bus> buses_;
    std::vector<Voice> voices_;
    VoiceId next_id_ = 1;
    u64 frames_rendered_ = 0;
    Listener listener_;
    u32 max_voices_ = 64;

    // Threading: the command queue in, used commands back, and the published
    // snapshots (a triple buffer: the audio thread writes one, the game thread
    // reads another, and they swap through `middle_`).
    bool threaded_ = false;
    std::unique_ptr<SpscQueue<Command*>> commands_, garbage_;
    u64 next_seq_ = 1;
    u64 applied_ = 0;
    mutable Snapshot slots_[3];
    mutable u32 front_ = 0;
    u32 back_ = 2;
    mutable std::atomic<u32> middle_{1};
    static constexpr u32 kFresh = 4;
    // The game thread's own view: bus settings, the listener, and changes not yet applied.
    struct GameBus {
        f32 volume_db = 0.0f;
        bool muted = false;
    };
    std::vector<GameBus> game_buses_;
    Listener game_listener_;
    u32 game_max_voices_ = 64;
    mutable std::deque<std::pair<u64, VoiceId>> pending_plays_, pending_stops_;
    u64 stop_all_seq_ = 0;
    std::map<VoiceId, Vec3> game_occluded_; // voices asking for occlusion, and where they are
};

} // namespace aether::audio
