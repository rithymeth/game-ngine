#pragma once

#include "aether/audio/mixer.h"

#include <vector>

namespace aether::editor {

// The waveform preview (Phase 17 step 6, docs/design/PHASE_SPECS.md §17.6):
// a sound drawn as its min/max per pixel column, zoomed and scrolled, with a
// playhead, a selected region and preview playback through a mixer.

struct WaveformColumn {
    f32 min = 0.0f, max = 0.0f, rms = 0.0f;
};
// `columns` columns over frames [from, to) of one channel (to 0 = the end).
std::vector<WaveformColumn> BuildPeaks(const audio::SoundWave& sound, u32 columns, u32 channel = 0, usize from = 0, usize to = 0);

struct SoundStats {
    f32 peak = 0.0f;       // linear, over all channels
    f32 peak_db = -144.0f;
    f32 rms_db = -144.0f;
    f32 dc_offset = 0.0f;  // the mean
    usize clipped = 0;     // samples at or beyond full scale
};
SoundStats AnalyzeSound(const audio::SoundWave& sound);

class WaveformView {
public:
    // The sound must outlive the view (or be replaced first).
    void SetSound(const audio::SoundWave* sound);
    const audio::SoundWave* Sound() const { return sound_; }
    // Preview through this mixer (optional).
    void SetMixer(audio::Mixer* mixer) { mixer_ = mixer; }

    void Draw(f32 height = 160.0f);

    // Visible range, in frames. Zoom keeps the frame under `anchor` (0..1 across the view) in place.
    usize ViewStart() const { return view_start_; }
    usize ViewFrames() const { return view_frames_; }
    void ZoomAt(f32 factor, f32 anchor = 0.5f); // factor > 1 zooms in
    void ScrollBy(i64 frames);
    void ShowAll();

    // Seconds.
    f32 Playhead() const { return playhead_; }
    void SetPlayhead(f32 seconds);
    // A region (loop or trim), in seconds; empty when from >= to.
    void SetSelection(f32 from, f32 to);
    f32 SelectionFrom() const { return sel_from_; }
    f32 SelectionTo() const { return sel_to_; }
    bool HasSelection() const { return sel_to_ > sel_from_; }

    // Preview: from the playhead (looping the selection if there is one).
    bool Play();
    void Stop();
    bool Playing() const;
    // Follows the preview voice (call each frame; Draw does).
    void Tick();
    const SoundStats& Stats() const { return stats_; }

private:
    void Clamp();
    const audio::SoundWave* sound_ = nullptr;
    audio::Mixer* mixer_ = nullptr;
    audio::VoiceId voice_ = 0;
    usize view_start_ = 0, view_frames_ = 0;
    f32 playhead_ = 0.0f, sel_from_ = 0.0f, sel_to_ = 0.0f;
    bool dragging_selection_ = false;
    SoundStats stats_;
};

} // namespace aether::editor
