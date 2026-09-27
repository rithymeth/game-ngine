#include "aether/audio/dsp.h"

#include <algorithm>
#include <cmath>
#include <complex>

namespace aether::audio {

namespace {
constexpr f32 kPi = 3.14159265358979f;
constexpr f32 kSilenceDb = -144.0f;
} // namespace

f32 DbToGain(f32 db) { return std::pow(10.0f, db / 20.0f); }
f32 GainToDb(f32 gain) { return gain <= 0.0f ? kSilenceDb : std::max(kSilenceDb, 20.0f * std::log10(gain)); }

// --- Biquad ---------------------------------------------------------------------------------

void Biquad::Configure(Type t, f32 f, f32 q_, f32 gain, u32 rate) {
    type = t;
    frequency = f;
    q = q_;
    gain_db = gain;
    const f32 nyquist = 0.5f * static_cast<f32>(std::max(rate, 1u));
    const f32 w0 = 2.0f * kPi * std::clamp(f, 1.0f, nyquist * 0.999f) / static_cast<f32>(std::max(rate, 1u));
    const f32 c = std::cos(w0), s = std::sin(w0);
    const f32 alpha = s / (2.0f * std::max(q_, 0.01f));
    const f32 A = std::pow(10.0f, gain / 40.0f);
    const f32 sq = 2.0f * std::sqrt(A) * alpha;
    f32 b0 = 1, b1 = 0, b2 = 0, a0 = 1, a1 = 0, a2 = 0;
    switch (t) {
    case Type::LowPass: b0 = (1 - c) / 2, b1 = 1 - c, b2 = (1 - c) / 2, a0 = 1 + alpha, a1 = -2 * c, a2 = 1 - alpha; break;
    case Type::HighPass: b0 = (1 + c) / 2, b1 = -(1 + c), b2 = (1 + c) / 2, a0 = 1 + alpha, a1 = -2 * c, a2 = 1 - alpha; break;
    case Type::BandPass: b0 = alpha, b1 = 0, b2 = -alpha, a0 = 1 + alpha, a1 = -2 * c, a2 = 1 - alpha; break;
    case Type::Notch: b0 = 1, b1 = -2 * c, b2 = 1, a0 = 1 + alpha, a1 = -2 * c, a2 = 1 - alpha; break;
    case Type::Peak: b0 = 1 + alpha * A, b1 = -2 * c, b2 = 1 - alpha * A, a0 = 1 + alpha / A, a1 = -2 * c, a2 = 1 - alpha / A; break;
    case Type::LowShelf:
        b0 = A * ((A + 1) - (A - 1) * c + sq), b1 = 2 * A * ((A - 1) - (A + 1) * c), b2 = A * ((A + 1) - (A - 1) * c - sq);
        a0 = (A + 1) + (A - 1) * c + sq, a1 = -2 * ((A - 1) + (A + 1) * c), a2 = (A + 1) + (A - 1) * c - sq;
        break;
    case Type::HighShelf:
        b0 = A * ((A + 1) + (A - 1) * c + sq), b1 = -2 * A * ((A - 1) + (A + 1) * c), b2 = A * ((A + 1) + (A - 1) * c - sq);
        a0 = (A + 1) - (A - 1) * c + sq, a1 = 2 * ((A - 1) - (A + 1) * c), a2 = (A + 1) - (A - 1) * c - sq;
        break;
    }
    b0_ = b0 / a0, b1_ = b1 / a0, b2_ = b2 / a0, a1_ = a1 / a0, a2_ = a2 / a0;
}

f32 Biquad::Process(f32 x, u32 ch) {
    auto& xs = x_[ch & 1];
    auto& ys = y_[ch & 1];
    const f32 y = b0_ * x + b1_ * xs[0] + b2_ * xs[1] - a1_ * ys[0] - a2_ * ys[1];
    xs[1] = xs[0], xs[0] = x;
    ys[1] = ys[0], ys[0] = y;
    return y;
}

void Biquad::Reset() { x_ = {}, y_ = {}; }

f32 Biquad::MagnitudeAt(f32 f, u32 rate) const {
    const f32 w = 2.0f * kPi * f / static_cast<f32>(std::max(rate, 1u));
    const std::complex<f32> z1 = std::polar(1.0f, -w), z2 = std::polar(1.0f, -2.0f * w);
    return std::abs((b0_ + b1_ * z1 + b2_ * z2) / (1.0f + a1_ * z1 + a2_ * z2));
}

// --- Effects --------------------------------------------------------------------------------

FilterEffect::FilterEffect(Biquad::Type type, f32 frequency, f32 q, f32 gain_db) {
    biquad_.type = type;
    biquad_.frequency = frequency;
    biquad_.q = q;
    biquad_.gain_db = gain_db;
}

void FilterEffect::Set(f32 frequency, f32 q, f32 gain_db) {
    biquad_.frequency = frequency;
    biquad_.q = q;
    biquad_.gain_db = gain_db;
    dirty_ = true;
}

void FilterEffect::Process(f32* stereo, u32 frames, u32 rate) {
    if (dirty_ || configured_rate_ != rate) {
        biquad_.Configure(biquad_.type, biquad_.frequency, biquad_.q, biquad_.gain_db, rate);
        configured_rate_ = rate;
        dirty_ = false;
    }
    for (u32 i = 0; i < frames; ++i) {
        stereo[2 * i] = biquad_.Process(stereo[2 * i], 0);
        stereo[2 * i + 1] = biquad_.Process(stereo[2 * i + 1], 1);
    }
}

void CompressorEffect::Process(f32* stereo, u32 frames, u32 rate) {
    const f32 fs = static_cast<f32>(std::max(rate, 1u));
    const f32 attack = std::exp(-1.0f / (std::max(attack_ms, 0.01f) * 0.001f * fs));
    const f32 release = std::exp(-1.0f / (std::max(release_ms, 0.01f) * 0.001f * fs));
    const f32 slope = 1.0f - 1.0f / std::max(ratio, 1.0f);
    reduction_db_ = 0.0f;
    for (u32 i = 0; i < frames; ++i) {
        const f32 level = GainToDb(std::max(std::fabs(stereo[2 * i]), std::fabs(stereo[2 * i + 1])));
        const f32 coef = level > envelope_db_ ? attack : release;
        envelope_db_ = coef * envelope_db_ + (1.0f - coef) * level;
        const f32 reduction = std::max(0.0f, envelope_db_ - threshold_db) * slope;
        reduction_db_ = std::max(reduction_db_, reduction);
        const f32 g = DbToGain(makeup_db - reduction);
        stereo[2 * i] *= g;
        stereo[2 * i + 1] *= g;
    }
}

namespace {
constexpr int kCombTuning[8] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
constexpr int kAllPassTuning[4] = {556, 441, 341, 225};
constexpr int kStereoSpread = 23;
} // namespace

void ReverbEffect::Build(u32 rate) {
    const f64 scale = static_cast<f64>(rate) / 44100.0;
    for (int ch = 0; ch < 2; ++ch) {
        for (int i = 0; i < 8; ++i) {
            combs_[ch][i].buffer.assign(std::max<usize>(1, static_cast<usize>((kCombTuning[i] + ch * kStereoSpread) * scale)), 0.0f);
            combs_[ch][i].index = 0;
            combs_[ch][i].store = 0.0f;
        }
        for (int i = 0; i < 4; ++i) {
            allpasses_[ch][i].buffer.assign(std::max<usize>(1, static_cast<usize>((kAllPassTuning[i] + ch * kStereoSpread) * scale)), 0.0f);
            allpasses_[ch][i].index = 0;
        }
    }
    built_rate_ = rate;
}

void ReverbEffect::Reset() {
    if (built_rate_ != 0) Build(built_rate_);
}

void ReverbEffect::Process(f32* stereo, u32 frames, u32 rate) {
    if (built_rate_ != rate) Build(rate);
    const f32 feedback = std::clamp(room_size, 0.0f, 1.0f) * 0.28f + 0.7f;
    const f32 damp = std::clamp(damping, 0.0f, 1.0f) * 0.4f;
    const f32 wet1 = wet * 3.0f * (width / 2.0f + 0.5f), wet2 = wet * 3.0f * ((1.0f - width) / 2.0f);
    for (u32 i = 0; i < frames; ++i) {
        const f32 in_l = stereo[2 * i], in_r = stereo[2 * i + 1];
        const f32 input = (in_l + in_r) * 0.015f;
        f32 out[2] = {0, 0};
        for (int ch = 0; ch < 2; ++ch) {
            for (Comb& c : combs_[ch]) {
                const f32 y = c.buffer[c.index];
                c.store = y * (1.0f - damp) + c.store * damp;
                c.buffer[c.index] = input + c.store * feedback;
                if (++c.index >= c.buffer.size()) c.index = 0;
                out[ch] += y;
            }
            for (AllPass& a : allpasses_[ch]) {
                const f32 b = a.buffer[a.index];
                const f32 y = -out[ch] + b;
                a.buffer[a.index] = out[ch] + b * 0.5f;
                if (++a.index >= a.buffer.size()) a.index = 0;
                out[ch] = y;
            }
        }
        stereo[2 * i] = out[0] * wet1 + out[1] * wet2 + in_l * dry;
        stereo[2 * i + 1] = out[1] * wet1 + out[0] * wet2 + in_r * dry;
    }
}

} // namespace aether::audio
