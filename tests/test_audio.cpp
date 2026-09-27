#include "aether/audio/mixer.h"
#include "test_framework.h"

#include <cmath>
#include <cstring>

using namespace aether;
using namespace aether::audio;

// Phase 17 step 1: sounds, voices, buses and effects, rendered offline.

namespace {

std::vector<f32> Render(Mixer& m, u32 frames, u32 block = 256) {
    std::vector<f32> out(static_cast<usize>(frames) * 2);
    for (u32 at = 0; at < frames; at += block) m.Render(out.data() + static_cast<usize>(at) * 2, std::min(block, frames - at));
    return out;
}

// Rising zero crossings per second on one channel, over [from, to) frames.
f32 Frequency(const std::vector<f32>& s, u32 rate, usize from, usize to, int ch = 0) {
    usize count = 0;
    usize first = 0, last = 0;
    for (usize i = from + 1; i < to; ++i) {
        if (s[2 * (i - 1) + ch] < 0.0f && s[2 * i + ch] >= 0.0f) {
            if (count == 0) first = i;
            last = i;
            ++count;
        }
    }
    return count < 2 ? 0.0f : static_cast<f32>(count - 1) * rate / static_cast<f32>(last - first);
}

f32 Peak(const std::vector<f32>& s, usize from, usize to, int ch = 0) {
    f32 p = 0;
    for (usize i = from; i < to; ++i) p = std::max(p, std::fabs(s[2 * i + ch]));
    return p;
}

f32 Rms(const std::vector<f32>& s, usize from, usize to, int ch = 0) {
    f64 sum = 0;
    for (usize i = from; i < to; ++i) sum += static_cast<f64>(s[2 * i + ch]) * s[2 * i + ch];
    return static_cast<f32>(std::sqrt(sum / static_cast<f64>(to - from)));
}

bool Close(f32 a, f32 b, f32 eps) { return std::fabs(a - b) <= eps; }

} // namespace

AETHER_TEST(Audio_WavRoundTripsAndRejects) {
    SoundWave tone = GenerateTone(440, 0.1f, 22050, 0.5f, 2);
    tone.samples[1] = -0.25f; // the channels differ
    for (u32 bits : {16u, 32u}) {
        SoundWave back;
        std::string error;
        AETHER_CHECK(DecodeWav(EncodeWav(tone, bits), back, &error));
        AETHER_CHECK(back.sample_rate == 22050 && back.channels == 2 && back.samples.size() == tone.samples.size());
        f32 worst = 0;
        for (usize i = 0; i < tone.samples.size(); ++i) worst = std::max(worst, std::fabs(back.samples[i] - tone.samples[i]));
        AETHER_CHECK(worst < (bits == 32 ? 1e-7f : 1e-4f));
    }
    AETHER_CHECK(Close(tone.Duration(), 0.1f, 1e-4f) && tone.Frames() == 2205);

    // Hand-made 8-bit and 24-bit files, an extensible header and an odd chunk before the data.
    auto wav = [](u16 format, u16 channels, u32 rate, u16 bits, const std::vector<u8>& data, bool extensible, bool junk) {
        std::vector<u8> v;
        auto p16 = [&](u16 x) { v.push_back(static_cast<u8>(x)), v.push_back(static_cast<u8>(x >> 8)); };
        auto p32 = [&](u32 x) { for (int i = 0; i < 4; ++i) v.push_back(static_cast<u8>(x >> (8 * i))); };
        auto tag = [&](const char* t) { v.insert(v.end(), t, t + 4); };
        tag("RIFF");
        p32(0);
        tag("WAVE");
        if (junk) {
            tag("LIST");
            p32(3);
            v.insert(v.end(), {1, 2, 3, 0}); // 3 bytes + pad
        }
        tag("fmt ");
        p32(extensible ? 40 : 16);
        p16(extensible ? 0xFFFE : format);
        p16(channels);
        p32(rate);
        p32(rate * channels * bits / 8);
        p16(static_cast<u16>(channels * bits / 8));
        p16(bits);
        if (extensible) {
            p16(22);
            p16(bits);
            p32(channels == 1 ? 4 : 3);
            p16(format);
            for (int i = 0; i < 14; ++i) v.push_back(0);
        }
        tag("data");
        p32(static_cast<u32>(data.size()));
        v.insert(v.end(), data.begin(), data.end());
        return v;
    };
    SoundWave s;
    std::string error;
    AETHER_CHECK(DecodeWav(wav(1, 1, 8000, 8, {0, 128, 255}, false, true), s, &error));
    AETHER_CHECK(s.samples.size() == 3 && s.samples[0] == -1.0f && s.samples[1] == 0.0f && Close(s.samples[2], 127.0f / 128, 1e-6f));
    AETHER_CHECK(DecodeWav(wav(1, 1, 48000, 24, {0x00, 0x00, 0x80, 0xFF, 0xFF, 0x7F, 0x00, 0x00, 0x40}, true, false), s, &error));
    AETHER_CHECK(s.samples.size() == 3 && s.samples[0] == -1.0f && Close(s.samples[1], 1.0f, 1e-6f) && s.samples[2] == 0.5f);
    // A data chunk that claims more than the file has plays what's there (whole frames).
    std::vector<u8> cut = wav(1, 2, 8000, 16, {0, 0, 0, 0, 0, 0, 0, 0, 0x00}, false, false);
    AETHER_CHECK(DecodeWav(cut, s, &error) && s.Frames() == 2);
    // Refused: not a WAV, ADPCM, 3 channels, float 64, cut-short header, no data.
    AETHER_CHECK(!DecodeWav({1, 2, 3}, s, &error));
    AETHER_CHECK(!DecodeWav(wav(2, 1, 8000, 4, {0}, false, false), s, &error) && error.find("encoding") != std::string::npos);
    AETHER_CHECK(!DecodeWav(wav(1, 3, 8000, 16, {0, 0, 0, 0, 0, 0}, false, false), s, &error));
    AETHER_CHECK(!DecodeWav(wav(3, 1, 8000, 64, {0, 0, 0, 0, 0, 0, 0, 0}, false, false), s, &error));
    std::vector<u8> header = wav(1, 1, 8000, 16, {}, false, false);
    header.resize(24);
    AETHER_CHECK(!DecodeWav(header, s, &error));
    header = wav(1, 1, 8000, 16, {}, false, false);
    header.resize(36); // the format chunk, no data chunk
    AETHER_CHECK(!DecodeWav(header, s, &error) && error.find("no data") != std::string::npos);
}

AETHER_TEST(Audio_VoicesResamplePanAndFade) {
    Mixer m(48000);
    const SoundWave tone = GenerateTone(440, 1.0f, 44100, 0.5f); // a different rate from the mixer
    // Resampled to the mixer's rate: still 440 Hz, and it ends on time.
    VoiceId v = m.Play(&tone);
    AETHER_CHECK(v != 0 && m.IsPlaying(v) && m.VoiceCount() == 1);
    std::vector<f32> out = Render(m, 48000 + 1024);
    AETHER_CHECK(Close(Frequency(out, 48000, 1000, 47000), 440.0f, 1.0f));
    AETHER_CHECK(!m.IsPlaying(v) && Peak(out, 48100, 49000) == 0.0f);
    // Centered mono: equal power (0.707 each side).
    AETHER_CHECK(Close(Peak(out, 1000, 47000, 0), 0.5f * 0.7071f, 5e-3f) && Close(Peak(out, 1000, 47000, 1), 0.5f * 0.7071f, 5e-3f));
    // Pitch 2: an octave up, half as long.
    v = m.Play(&tone, {kMasterBus, 0.0f, 2.0f});
    out = Render(m, 26000);
    AETHER_CHECK(Close(Frequency(out, 48000, 500, 23500), 880.0f, 2.0f) && !m.IsPlaying(v));
    // -6 dB and hard left.
    v = m.Play(&tone, {kMasterBus, -6.0206f, 1.0f, -1.0f});
    out = Render(m, 4800);
    AETHER_CHECK(Close(Peak(out, 0, 4800, 0), 0.25f, 3e-3f) && Peak(out, 0, 4800, 1) < 1e-6f);
    // Volume changes glide instead of jumping.
    m.SetVolume(v, 0.0f);
    out = Render(m, 1500);
    AETHER_CHECK(Peak(out, 0, 20) < 0.3f && Close(Peak(out, 1300, 1500), 0.5f, 0.01f)); // settled after ~5 time constants (5 ms each)
    m.StopAll();
    // Looping keeps going past the end; the playback time wraps.
    const SoundWave blip = GenerateTone(1000, 0.01f, 48000);
    v = m.Play(&blip, {kMasterBus, 0.0f, 1.0f, 0.0f, true});
    out = Render(m, 4800);
    AETHER_CHECK(m.IsPlaying(v) && Peak(out, 4000, 4800) > 0.3f && m.PlaybackTime(v) < 0.01f);
    // Fading out over 50 ms, then it's gone.
    AETHER_CHECK(m.Stop(v, 0.05f) && m.IsPlaying(v));
    out = Render(m, 2400 + 256);
    AETHER_CHECK(!m.IsPlaying(v) && Peak(out, 0, 200) > 0.3f && Peak(out, 2300, 2656) < 0.02f);
    // Fading in over 100 ms: quiet at first, full at the end.
    v = m.Play(&tone, {kMasterBus, 0.0f, 1.0f, 0.0f, false, 0.0f, 0.1f});
    out = Render(m, 9600);
    AETHER_CHECK(Peak(out, 0, 480) < 0.06f && Peak(out, 2200, 2600) < 0.2f && Close(Peak(out, 4900, 9600), 0.3535f, 5e-3f));
    // Starting part way in, and the bad cases.
    m.StopAll();
    v = m.Play(&tone, {kMasterBus, 0.0f, 1.0f, 0.0f, false, 0.5f});
    AETHER_CHECK(Close(m.PlaybackTime(v), 0.5f, 1e-4f));
    AETHER_CHECK(m.Play(nullptr) == 0 && m.Play(&tone, {99}) == 0 && !m.Stop(9999) && !m.SetPan(9999, 0));
    SoundWave empty;
    AETHER_CHECK(m.Play(&empty) == 0);
    // Stereo sounds pan by balance.
    m.StopAll();
    const SoundWave wide = GenerateTone(300, 0.2f, 48000, 0.5f, 2);
    m.Play(&wide, {kMasterBus, 0.0f, 1.0f, 0.5f});
    out = Render(m, 4800);
    AETHER_CHECK(Close(Peak(out, 100, 4800, 0), 0.25f, 3e-3f) && Close(Peak(out, 100, 4800, 1), 0.5f, 3e-3f));
}

AETHER_TEST(Audio_BusesMixWithMetersAndMute) {
    Mixer m(48000);
    m.AddDefaultBuses();
    const BusId music = m.FindBus("Music"), sfx = m.FindBus("SFX");
    AETHER_CHECK(m.BusCount() == 5 && music != Mixer::kInvalidBus && m.BusParent(music) == kMasterBus && m.FindBus("Nope") == Mixer::kInvalidBus);
    const BusId footsteps = m.AddBus("Footsteps", sfx);
    AETHER_CHECK(m.BusParent(footsteps) == sfx && m.BusName(footsteps) == "Footsteps");
    const SoundWave tone = GenerateTone(200, 2.0f, 48000, 0.5f);
    m.Play(&tone, {footsteps, 0.0f, 1.0f, -1.0f});
    // Volumes multiply down the tree: -6 dB on SFX and -6 dB on Master is -12 dB.
    m.SetBusVolume(sfx, -6.0206f);
    m.SetBusVolume(kMasterBus, -6.0206f);
    AETHER_CHECK(m.BusVolume(sfx) == -6.0206f && !m.SetBusVolume(99, 0));
    std::vector<f32> out = Render(m, 4800);
    AETHER_CHECK(Close(Peak(out, 2400, 4800), 0.125f, 2e-3f));
    // Meters: each bus's own level after its gain.
    AETHER_CHECK(Close(m.Meter(footsteps).peak[0], 0.5f, 3e-3f) && Close(m.Meter(sfx).peak[0], 0.25f, 3e-3f));
    AETHER_CHECK(Close(m.Meter(kMasterBus).rms[0], 0.125f / std::sqrt(2.0f), 3e-3f) && m.Meter(music).peak[0] == 0.0f);
    AETHER_CHECK(m.Meter(kMasterBus).peak[1] < 1e-6f); // panned hard left
    // Muting ramps to silence over one block (no click), then stays silent.
    m.SetBusMuted(sfx, true);
    AETHER_CHECK(m.BusMuted(sfx));
    out = Render(m, 512, 512);
    AETHER_CHECK(Peak(out, 0, 50) > 0.05f && Peak(out, 500, 512) < 0.01f);
    out = Render(m, 512);
    AETHER_CHECK(Peak(out, 0, 512) == 0.0f);
    m.SetBusMuted(sfx, false);
    m.StopAll();
    out = Render(m, 256);
    AETHER_CHECK(Peak(out, 0, 256) == 0.0f); // nothing playing: silence
}

AETHER_TEST(Audio_Effects) {
    // Biquad responses against the cookbook: a 1 kHz low-pass is 0 dB low, -3 dB at the corner, deep down high.
    Biquad lp;
    lp.Configure(Biquad::Type::LowPass, 1000, 0.7071f, 0, 48000);
    AETHER_CHECK(Close(lp.MagnitudeAt(50, 48000), 1.0f, 0.01f) && Close(lp.MagnitudeAt(1000, 48000), 0.7071f, 0.01f) && lp.MagnitudeAt(10000, 48000) < 0.02f);
    Biquad hp;
    hp.Configure(Biquad::Type::HighPass, 1000, 0.7071f, 0, 48000);
    AETHER_CHECK(hp.MagnitudeAt(50, 48000) < 0.01f && Close(hp.MagnitudeAt(10000, 48000), 1.0f, 0.02f));
    Biquad peak;
    peak.Configure(Biquad::Type::Peak, 2000, 1.0f, 6.0f, 48000);
    AETHER_CHECK(Close(GainToDb(peak.MagnitudeAt(2000, 48000)), 6.0f, 0.05f) && Close(peak.MagnitudeAt(50, 48000), 1.0f, 0.02f));
    Biquad shelf;
    shelf.Configure(Biquad::Type::LowShelf, 200, 0.7071f, -12.0f, 48000);
    AETHER_CHECK(Close(GainToDb(shelf.MagnitudeAt(20, 48000)), -12.0f, 0.3f) && Close(shelf.MagnitudeAt(10000, 48000), 1.0f, 0.02f));
    Biquad notch, band, high_shelf;
    notch.Configure(Biquad::Type::Notch, 1000, 5.0f, 0, 48000);
    band.Configure(Biquad::Type::BandPass, 1000, 2.0f, 0, 48000);
    high_shelf.Configure(Biquad::Type::HighShelf, 4000, 0.7071f, 6.0f, 48000);
    AETHER_CHECK(notch.MagnitudeAt(1000, 48000) < 1e-3f && Close(band.MagnitudeAt(1000, 48000), 1.0f, 0.01f) &&
                 Close(GainToDb(high_shelf.MagnitudeAt(20000, 48000)), 6.0f, 0.3f));
    AETHER_CHECK(Close(DbToGain(-6.0206f), 0.5f, 1e-4f) && GainToDb(0.0f) == -144.0f && Close(GainToDb(0.1f), -20.0f, 1e-4f));

    // Measured through the mixer: a 5 kHz tone through a 1 kHz low-pass on its bus comes out at the analytic gain.
    Mixer m(48000);
    const BusId bus = m.AddBus("Filtered");
    auto* filter = static_cast<FilterEffect*>(m.AddEffect(bus, std::make_unique<FilterEffect>(Biquad::Type::LowPass, 1000.0f)));
    const SoundWave tone = GenerateTone(5000, 1.0f, 48000, 0.5f);
    m.Play(&tone, {bus});
    std::vector<f32> out = Render(m, 24000);
    const f32 expected = 0.5f * 0.7071f * filter->Filter().MagnitudeAt(5000, 48000);
    AETHER_CHECK(Close(Peak(out, 12000, 24000), expected, expected * 0.05f));
    filter->bypass = true;
    out = Render(m, 4800);
    AETHER_CHECK(Close(Peak(out, 2000, 4800), 0.3535f, 0.01f));
    AETHER_CHECK(m.AddEffect(99, std::make_unique<ReverbEffect>()) == nullptr);

    // Compressor: 12 dB over a -18 dB threshold at 4:1 comes down by 9 dB once settled.
    CompressorEffect comp;
    std::vector<f32> loud(48000 * 2);
    const f32 level = DbToGain(-6.0f);
    for (usize i = 0; i < loud.size(); ++i) loud[i] = (i / 2) % 2 ? level : -level; // a square wave at -6 dB
    comp.Process(loud.data(), 48000, 48000);
    AETHER_CHECK(Close(GainToDb(std::fabs(loud.back())), -6.0f - 9.0f, 0.3f) && Close(comp.GainReductionDb(), 9.0f, 0.3f));
    std::vector<f32> quiet(4800 * 2, DbToGain(-30.0f));
    comp.Reset();
    comp.Process(quiet.data(), 4800, 48000);
    AETHER_CHECK(Close(quiet.back(), DbToGain(-30.0f), 1e-5f) && comp.GainReductionDb() == 0.0f); // below threshold: untouched

    // Reverb: an impulse leaves a decaying tail; dry only passes straight through.
    ReverbEffect verb;
    verb.room_size = 0.8f;
    verb.dry = 0.0f;
    std::vector<f32> impulse(48000 * 2, 0.0f);
    impulse[0] = impulse[1] = 1.0f;
    verb.Process(impulse.data(), 48000, 48000);
    const f32 early = Rms(impulse, 2000, 6000), late = Rms(impulse, 40000, 48000);
    AETHER_CHECK(early > 1e-4f && late < early * 0.5f && late > 0.0f);
    AETHER_CHECK(std::fabs(impulse[0]) < 1e-6f); // nothing before the first echo
    ReverbEffect passthrough;
    passthrough.wet = 0.0f;
    std::vector<f32> signal = {0.1f, 0.2f, 0.3f, 0.4f};
    passthrough.Process(signal.data(), 2, 48000);
    AETHER_CHECK(signal == (std::vector<f32>{0.1f, 0.2f, 0.3f, 0.4f}));
}
