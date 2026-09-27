#pragma once

#include "aether/core/base.h"

#include <string>
#include <vector>

namespace aether::audio {

// A sound's samples (Phase 17 step 1, docs/design/PHASE_SPECS.md §17.1):
// interleaved floats in -1..1 at a sample rate, mono or stereo.
struct SoundWave {
    std::string name;
    u32 sample_rate = 48000;
    u32 channels = 1; // 1 or 2
    std::vector<f32> samples;
    usize Frames() const { return channels == 0 ? 0 : samples.size() / channels; }
    f32 Duration() const { return sample_rate == 0 ? 0.0f : static_cast<f32>(Frames()) / static_cast<f32>(sample_rate); }
};

// WAV: 8/16/24/32-bit PCM and 32-bit float, plain or WAVE_FORMAT_EXTENSIBLE;
// other chunks are skipped. Up to 2 channels.
bool DecodeWav(const std::vector<u8>& bytes, SoundWave& out, std::string* error = nullptr);
// 16-bit PCM, or 32-bit float with `bits` = 32.
std::vector<u8> EncodeWav(const SoundWave& sound, u32 bits = 16);

// A sine tone, for tests and the editor's preview.
SoundWave GenerateTone(f32 frequency, f32 seconds, u32 sample_rate = 48000, f32 amplitude = 0.5f, u32 channels = 1);

} // namespace aether::audio
