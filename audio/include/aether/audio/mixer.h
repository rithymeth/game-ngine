#pragma once

#include "aether/audio/dsp.h"
#include "aether/audio/sound.h"

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

struct PlayParams {
    BusId bus = kMasterBus;
    f32 volume_db = 0.0f;
    f32 pitch = 1.0f;   // playback rate multiplier (2 = an octave up)
    f32 pan = 0.0f;     // -1 left .. 1 right
    bool loop = false;
    f32 start_time = 0.0f; // seconds into the sound
    f32 fade_in = 0.0f;    // seconds
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
    };
    Voice* Find(VoiceId id);
    const Voice* Find(VoiceId id) const;
    // Adds the voice's next `frames` to `out` (stereo); false once it has finished.
    bool MixVoice(Voice& v, f32* out, u32 frames);

    u32 sample_rate_;
    std::vector<Bus> buses_;
    std::vector<Voice> voices_;
    VoiceId next_id_ = 1;
};

} // namespace aether::audio
