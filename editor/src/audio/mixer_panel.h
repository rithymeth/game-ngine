#pragma once

#include "aether/audio/mixer.h"

#include <atomic>
#include <map>
#include <optional>
#include <memory>
#include <string>
#include <vector>

namespace aether::editor {

// The mixer panel (Phase 17 step 6): a channel strip per bus (fader, mute,
// live peak and RMS meters with peak hold), each bus's effects with their
// settings and bypass, and the voices playing. Works on a threaded mixer
// too: meters and voices come from its snapshot, and effect settings are
// read and written on the audio thread (through Mixer::Post).

// Meter ballistics: the level falls at `fall_db_per_second`; the peak holds
// for `hold_seconds`, then falls too.
struct MeterBallistics {
    f32 fall_db_per_second = 24.0f;
    f32 hold_seconds = 1.0f;
    f32 level_db = -144.0f, peak_db = -144.0f;
    f32 held_for = 0.0f;
    void Update(f32 input_linear, f32 dt);
};

// An effect's settings as numbers, by the effect's kind (Filter, Compressor, Reverb).
struct EffectParam {
    const char* name;
    f32 min, max;
    const char* format;
};
std::vector<EffectParam> EffectParams(const audio::AudioEffect& effect);
std::vector<f32> ReadEffect(const audio::AudioEffect& effect);
void WriteEffect(audio::AudioEffect& effect, const std::vector<f32>& values);

class MixerPanel {
public:
    explicit MixerPanel(audio::Mixer& mixer) : mixer_(mixer) {}

    void Draw();
    enum class Tab : u8 { Buses, Effects, Voices };
    void ShowTab(Tab tab) { show_tab_ = tab; } // on the next Draw

    // What the widgets do.
    void SetFader(audio::BusId bus, f32 db) { mixer_.SetBusVolume(bus, db); }
    void ToggleMute(audio::BusId bus) { mixer_.SetBusMuted(bus, !mixer_.BusMuted(bus)); }
    void SetBypass(audio::BusId bus, usize effect, bool bypass);
    void SetEffectParam(audio::BusId bus, usize effect, usize param, f32 value);
    // The panel's view of an effect's settings; false until it knows them
    // (a threaded mixer answers on its next Render).
    bool EffectValues(audio::BusId bus, usize effect, std::vector<f32>& values, bool& bypass);

    const MeterBallistics& Ballistics(audio::BusId bus, u32 channel) { return meters_[{bus, channel}]; }
    // Advances the meters from the mixer (Draw does, with ImGui's delta time).
    void UpdateMeters(f32 dt);

private:
    struct EffectView {
        // Filled on the audio thread when threaded; read after `ready`.
        std::vector<f32> values;
        bool bypass = false;
        std::atomic<bool> ready{false};
        bool requested = false;
        // A bypass set before the settings arrived: the audio thread's answer may predate it.
        std::optional<bool> bypass_override;
    };
    EffectView& ViewOf(audio::AudioEffect* effect);
    void DrawStrip(audio::BusId bus);
    void DrawEffects();
    void DrawVoices();

    audio::Mixer& mixer_;
    std::optional<Tab> show_tab_;
    std::map<std::pair<audio::BusId, u32>, MeterBallistics> meters_;
    std::map<audio::AudioEffect*, std::shared_ptr<EffectView>> effects_;
};

} // namespace aether::editor
