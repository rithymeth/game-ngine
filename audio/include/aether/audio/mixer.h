#pragma once

#include "aether/audio/dsp.h"
#include "aether/audio/sound.h"
#include "aether/audio/spatial.h"

#include <algorithm>
#include <functional>

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace aether::audio {

// The software mixer (Phase 17 step 1, docs/design/PHASE_SPECS.md §17.1):
// voices play sounds into buses, buses run their effects and mix into their
// parents up to Master, and Render writes interleaved stereo. It isn't
// thread-safe by itself; the device backend (step 5) hands commands to the
// audio thread.

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

    u32 SampleRate() const { return sample_rate_; }

    // --- Buses ------------------------------------------------------------------
    // Master exists from the start; parents come before children.
    BusId AddBus(const std::string& name, BusId parent = kMasterBus);
    // Master with Music, SFX, UI and Voice under it.
    void AddDefaultBuses();
    BusId FindBus(const std::string& name) const; // kInvalidBus if none
    static constexpr BusId kInvalidBus = ~0u;
    usize BusCount() const { return buses_.size(); }
    const std::string& BusName(BusId bus) const { return buses_[bus].name; }
    BusId BusParent(BusId bus) const { return buses_[bus].parent; }
    bool SetBusVolume(BusId bus, f32 db);
    f32 BusVolume(BusId bus) const { return bus < buses_.size() ? buses_[bus].volume_db : 0.0f; }
    bool SetBusMuted(BusId bus, bool muted);
    bool BusMuted(BusId bus) const { return bus < buses_.size() && buses_[bus].muted; }
    AudioEffect* AddEffect(BusId bus, std::unique_ptr<AudioEffect> effect);
    const BusMeter& Meter(BusId bus) const { return buses_[bus].meter; }

    // --- Voices -----------------------------------------------------------------
    // The sound must outlive the voice. Returns 0 for an empty sound or a bad bus.
    VoiceId Play(const SoundWave* sound, const PlayParams& params = {});
    // Stops now, or fades out over `fade_out` seconds first.
    bool Stop(VoiceId voice, f32 fade_out = 0.0f);
    void StopAll();
    bool SetVolume(VoiceId voice, f32 db);
    bool SetPitch(VoiceId voice, f32 pitch);
    bool SetPan(VoiceId voice, f32 pan);
    bool IsPlaying(VoiceId voice) const;
    f32 PlaybackTime(VoiceId voice) const; // seconds into the sound; -1 if not playing
    usize VoiceCount() const { return voices_.size(); }
    bool GetVoiceInfo(VoiceId voice, VoiceInfo& out) const;

    // --- 3D (step 2) --------------------------------------------------------------
    void SetListener(const Listener& listener) { listener_ = listener; }
    const Listener& GetListener() const { return listener_; }
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
    void SetMaxVoices(u32 count) { max_voices_ = std::max(count, 1u); }
    u32 MaxVoices() const { return max_voices_; }
    f32 virtual_threshold_db = -80.0f;
    usize RealVoiceCount() const;
    usize VirtualVoiceCount() const { return voices_.size() - RealVoiceCount(); }

    // Mixes `frames` of interleaved stereo into `out` (overwriting it).
    void Render(f32* out, u32 frames);

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
    void AssignChannels();

    u32 sample_rate_;
    std::vector<Bus> buses_;
    std::vector<Voice> voices_;
    VoiceId next_id_ = 1;
    Listener listener_;
    u32 max_voices_ = 64;
};

} // namespace aether::audio
