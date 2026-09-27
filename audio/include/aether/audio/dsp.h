#pragma once

#include "aether/core/base.h"

#include <array>
#include <memory>
#include <string>
#include <vector>

namespace aether::audio {

// Signal processing for the mixer's effects (Phase 17 step 1,
// docs/design/PHASE_SPECS.md §17.1). Buffers are interleaved stereo.

f32 DbToGain(f32 db);
f32 GainToDb(f32 gain); // -inf for 0 (clamped to -144)

// A second-order filter from Robert Bristow-Johnson's cookbook.
struct Biquad {
    enum class Type : u8 { LowPass, HighPass, BandPass, Notch, LowShelf, HighShelf, Peak };
    Type type = Type::LowPass;
    f32 frequency = 1000.0f; // Hz
    f32 q = 0.7071f;
    f32 gain_db = 0.0f; // shelves and peak

    void Configure(Type type, f32 frequency, f32 q, f32 gain_db, u32 sample_rate);
    f32 Process(f32 x, u32 channel);
    void Reset();
    // The filter's gain at a frequency (for tests and the editor's curve).
    f32 MagnitudeAt(f32 frequency, u32 sample_rate) const;

private:
    f32 b0_ = 1, b1_ = 0, b2_ = 0, a1_ = 0, a2_ = 0;
    std::array<std::array<f32, 2>, 2> x_{}, y_{}; // per channel: last two inputs and outputs
};

// An effect on a bus, run in order on its mixed stereo signal.
class AudioEffect {
public:
    virtual ~AudioEffect() = default;
    virtual const char* Name() const = 0;
    virtual void Process(f32* stereo, u32 frames, u32 sample_rate) = 0;
    virtual void Reset() {}
    bool bypass = false;
};

class FilterEffect final : public AudioEffect {
public:
    FilterEffect(Biquad::Type type, f32 frequency, f32 q = 0.7071f, f32 gain_db = 0.0f);
    const char* Name() const override { return "Filter"; }
    void Process(f32* stereo, u32 frames, u32 sample_rate) override;
    void Reset() override { biquad_.Reset(); }
    void Set(f32 frequency, f32 q, f32 gain_db); // takes effect on the next block
    const Biquad& Filter() const { return biquad_; }

private:
    Biquad biquad_;
    u32 configured_rate_ = 0;
    bool dirty_ = true;
};

// A feed-forward compressor on the louder channel (so the stereo image holds).
class CompressorEffect final : public AudioEffect {
public:
    f32 threshold_db = -18.0f;
    f32 ratio = 4.0f;
    f32 attack_ms = 5.0f;
    f32 release_ms = 100.0f;
    f32 makeup_db = 0.0f;
    const char* Name() const override { return "Compressor"; }
    void Process(f32* stereo, u32 frames, u32 sample_rate) override;
    void Reset() override { envelope_db_ = -144.0f; }
    f32 GainReductionDb() const { return reduction_db_; } // the last block's, for the meter

private:
    f32 envelope_db_ = -144.0f, reduction_db_ = 0.0f;
};

// Freeverb (Jezar's): 8 parallel comb filters and 4 series all-passes per
// channel, the right channel's delays offset for width.
class ReverbEffect final : public AudioEffect {
public:
    f32 room_size = 0.5f; // 0..1
    f32 damping = 0.5f;   // 0..1
    f32 wet = 0.33f, dry = 1.0f;
    f32 width = 1.0f;
    const char* Name() const override { return "Reverb"; }
    void Process(f32* stereo, u32 frames, u32 sample_rate) override;
    void Reset() override;

private:
    struct Comb {
        std::vector<f32> buffer;
        usize index = 0;
        f32 store = 0.0f;
    };
    struct AllPass {
        std::vector<f32> buffer;
        usize index = 0;
    };
    void Build(u32 sample_rate);
    u32 built_rate_ = 0;
    std::array<std::array<Comb, 8>, 2> combs_;
    std::array<std::array<AllPass, 4>, 2> allpasses_;
};

} // namespace aether::audio
