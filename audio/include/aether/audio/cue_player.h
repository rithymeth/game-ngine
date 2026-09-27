#pragma once

#include "aether/audio/cue.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace aether::audio {

// The sounds cues play, by name (Phase 17 step 3). A sound is either
// decoded in memory or kept compressed and streamed each time it plays.
class SoundBank {
public:
    void Add(const std::string& name, SoundWave sound);
    // Streams `bytes` (WAV, Ogg Vorbis or FLAC) instead of decoding it up front.
    bool AddStreamed(const std::string& name, std::shared_ptr<const std::vector<u8>> bytes, std::string* error = nullptr);
    bool Contains(const std::string& name) const;
    f64 Duration(const std::string& name) const; // seconds; -1 if unknown
    const SoundWave* Find(const std::string& name) const; // nullptr for streamed sounds
    bool IsStreamed(const std::string& name) const;
    std::unique_ptr<AudioStream> Open(const std::string& name) const; // streamed sounds only

private:
    struct Entry {
        std::shared_ptr<const SoundWave> wave;
        std::shared_ptr<const std::vector<u8>> bytes;
        f64 duration = 0.0;
    };
    std::map<std::string, Entry> entries_;
};

using CueHandle = u32; // 0 is none

struct CuePlayParams {
    f32 volume_db = 0.0f;
    f32 pitch = 1.0f;
    f32 fade_in = 0.0f;
    Vec3 position{}, velocity{}; // for spatial cues
    bool force_2d = false;       // play a spatial cue without 3D (Play Sound 2D)
};

// Plays cues on a mixer: evaluates each cue, schedules its sounds
// sample-accurately with voice delays, and keeps endless loops going by
// scheduling each repeat a little ahead of time. Call Update every frame.
class CuePlayer {
public:
    CuePlayer(Mixer& mixer, const SoundBank& bank, u64 seed = 0x5EED5EEDull) : mixer_(mixer), bank_(bank), seed_(seed) {}

    // The cue must outlive what it plays.
    CueHandle Play(const SoundCue& cue, const CuePlayParams& params = {});
    bool Stop(CueHandle cue, f32 fade_out = 0.0f);
    void StopAll();
    bool IsPlaying(CueHandle cue) const;
    bool SetPosition(CueHandle cue, const Vec3& position, const Vec3& velocity = {});
    bool SetVolume(CueHandle cue, f32 db);
    std::vector<VoiceId> Voices(CueHandle cue) const;
    usize ActiveCount() const { return instances_.size(); }

    // Schedules the next repeat of endless loops due within `lookahead`, and
    // forgets cues that have finished.
    void Update();
    f32 lookahead = 0.25f; // seconds

    // The choices a cue remembers (Random's last pick, Sequence's position).
    CueState& StateOf(const SoundCue& cue);

private:
    struct Instance {
        CueHandle handle = 0;
        const SoundCue* cue = nullptr;
        CuePlayParams params;
        u64 start_frame = 0;
        std::vector<std::pair<VoiceId, f32>> voices; // with each sound's own volume
        std::vector<CueTail> tails;
    };
    Instance* Find(CueHandle handle);
    const Instance* Find(CueHandle handle) const;
    void Schedule(Instance& instance, const CuePlan& plan);
    SoundDuration Durations() const;

    Mixer& mixer_;
    const SoundBank& bank_;
    u64 seed_;
    std::vector<Instance> instances_;
    std::map<const SoundCue*, CueState> states_;
    CueHandle next_handle_ = 1;
};

} // namespace aether::audio
