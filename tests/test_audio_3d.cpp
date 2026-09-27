#include "aether/audio/mixer.h"
#include "test_framework.h"

#include <cmath>

using namespace aether;
using namespace aether::audio;

// Phase 17 step 2: 3D audio (attenuation, panning, doppler, air absorption,
// occlusion) and voice limiting with virtual voices, rendered offline.

namespace {

constexpr u32 kRate = 48000;

std::vector<f32> Render(Mixer& m, u32 frames, u32 block = 256) {
    std::vector<f32> out(static_cast<usize>(frames) * 2);
    for (u32 at = 0; at < frames; at += block) m.Render(out.data() + static_cast<usize>(at) * 2, std::min(block, frames - at));
    return out;
}

f32 Rms(const std::vector<f32>& s, usize from, usize to, int ch = 0) {
    f64 sum = 0;
    for (usize i = from; i < to; ++i) sum += static_cast<f64>(s[2 * i + ch]) * s[2 * i + ch];
    return to > from ? static_cast<f32>(std::sqrt(sum / static_cast<f64>(to - from))) : 0.0f;
}

f32 Frequency(const std::vector<f32>& s, usize from, usize to, int ch = 0) {
    usize count = 0, first = 0, last = 0;
    for (usize i = from + 1; i < to; ++i) {
        if (s[2 * (i - 1) + ch] < 0.0f && s[2 * i + ch] >= 0.0f) {
            if (count == 0) first = i;
            last = i;
            ++count;
        }
    }
    return count < 2 ? 0.0f : static_cast<f32>(count - 1) * kRate / static_cast<f32>(last - first);
}

bool Near(f32 a, f32 b, f32 tolerance) { return std::fabs(a - b) <= tolerance; }

PlayParams At(const Vec3& position, AttenuationModel model = AttenuationModel::None) {
    PlayParams p;
    p.loop = true;
    p.spatial = true;
    p.position = position;
    p.attenuation.model = model;
    return p;
}

} // namespace

AETHER_TEST(Audio_AttenuationPanAndDopplerMaths) {
    AttenuationSettings s;
    s.model = AttenuationModel::Inverse;
    s.min_distance = 1.0f, s.max_distance = 50.0f;
    AETHER_CHECK(Near(Attenuate(s, 0.5f), 1.0f, 1e-6f)); // inside min_distance
    AETHER_CHECK(Near(Attenuate(s, 2.0f), 0.5f, 1e-6f));
    AETHER_CHECK(Near(Attenuate(s, 4.0f), 0.25f, 1e-6f));
    AETHER_CHECK(Near(Attenuate(s, 500.0f), 1.0f / 50.0f, 1e-6f)); // no further falloff past max
    s.rolloff = 2.0f;
    AETHER_CHECK(Near(Attenuate(s, 2.0f), 1.0f / 3.0f, 1e-6f));

    s.model = AttenuationModel::Linear;
    s.min_distance = 1.0f, s.max_distance = 11.0f;
    AETHER_CHECK(Near(Attenuate(s, 6.0f), 0.5f, 1e-6f));
    AETHER_CHECK(Attenuate(s, 20.0f) == 0.0f);

    s.model = AttenuationModel::Logarithmic;
    s.min_distance = 1.0f, s.max_distance = 100.0f;
    AETHER_CHECK(Near(Attenuate(s, 10.0f), 0.5f, 1e-5f));
    AETHER_CHECK(Near(Attenuate(s, 100.0f), 0.0f, 1e-6f));

    s.model = AttenuationModel::Custom;
    s.min_distance = 10.0f, s.max_distance = 20.0f;
    s.curve = {{0.0f, 1.0f}, {0.5f, 0.2f}, {1.0f, 0.0f}};
    AETHER_CHECK(Near(Attenuate(s, 12.5f), 0.6f, 1e-6f));
    AETHER_CHECK(Near(Attenuate(s, 17.5f), 0.1f, 1e-6f));
    AETHER_CHECK(Near(Attenuate(s, 5.0f), 1.0f, 1e-6f));

    s.model = AttenuationModel::None;
    AETHER_CHECK(Attenuate(s, 1000.0f) == 1.0f);

    // Every model falls (or holds) with distance and stays in 0..1.
    for (AttenuationModel model : {AttenuationModel::Inverse, AttenuationModel::Linear, AttenuationModel::Logarithmic, AttenuationModel::Custom}) {
        s.model = model;
        f32 last = 1.0f;
        for (f32 d = 0.0f; d < 30.0f; d += 0.25f) {
            const f32 g = Attenuate(s, d);
            AETHER_CHECK(g <= last + 1e-6f && g >= 0.0f && g <= 1.0f);
            last = g;
        }
    }

    // Air absorption sweeps in octaves from open to the far cutoff.
    s.model = AttenuationModel::Linear;
    s.min_distance = 10.0f, s.max_distance = 20.0f;
    AETHER_CHECK(AirAbsorptionCutoff(s, 15.0f) == kOpenCutoff); // off by default
    s.lowpass_at_max_hz = 500.0f;
    AETHER_CHECK(Near(AirAbsorptionCutoff(s, 5.0f), kOpenCutoff, 1e-2f));
    AETHER_CHECK(Near(AirAbsorptionCutoff(s, 15.0f), std::sqrt(kOpenCutoff * 500.0f), 0.5f));
    AETHER_CHECK(Near(AirAbsorptionCutoff(s, 40.0f), 500.0f, 1e-2f));

    // Pan is the source's direction along the listener's right.
    Listener l; // at the origin, facing -Z
    AETHER_CHECK(Near(PanFromListener(l, {5, 0, 0}), 1.0f, 1e-6f));
    AETHER_CHECK(Near(PanFromListener(l, {-5, 0, 0}), -1.0f, 1e-6f));
    AETHER_CHECK(Near(PanFromListener(l, {0, 0, -5}), 0.0f, 1e-6f));
    AETHER_CHECK(Near(PanFromListener(l, {3, 0, -3}), std::sqrt(0.5f), 1e-5f));
    AETHER_CHECK(PanFromListener(l, {0, 0, 0}) == 0.0f);
    l.forward = {1, 0, 0}; // turned right: +Z is now on the right
    AETHER_CHECK(Near(PanFromListener(l, {0, 0, 5}), 1.0f, 1e-6f));

    // Doppler: 10% of the speed of sound toward, away, and a moving listener.
    Listener ear;
    const Vec3 source{0, 0, -10};
    AETHER_CHECK(Near(DopplerRatio(ear, source, {0, 0, 34.3f}), 343.0f / (343.0f - 34.3f), 1e-5f));
    AETHER_CHECK(Near(DopplerRatio(ear, source, {0, 0, -34.3f}), 343.0f / (343.0f + 34.3f), 1e-5f));
    AETHER_CHECK(Near(DopplerRatio(ear, source, {34.3f, 0, 0}), 1.0f, 1e-6f)); // passing sideways
    AETHER_CHECK(Near(DopplerRatio(ear, source, {0, 0, 34.3f}, 0.0f), 1.0f, 1e-6f)); // factor 0: off
    ear.velocity = {0, 0, -34.3f};                                                  // listener toward the source
    AETHER_CHECK(Near(DopplerRatio(ear, source, {}), (343.0f + 34.3f) / 343.0f, 1e-5f));
    ear.velocity = {};
    const f32 fast = DopplerRatio(ear, source, {0, 0, 1000.0f}); // faster than sound: clamped
    AETHER_CHECK(fast <= 4.0f && fast > 1.0f && std::isfinite(fast));
}

AETHER_TEST(Audio_3DVoicesAttenuatePanAndShiftPitch) {
    const SoundWave tone = GenerateTone(440.0f, 1.0f, kRate, 0.5f);
    const f32 full_rms = 0.5f / std::sqrt(2.0f);
    {
        // Inverse falloff, 4 m to the right: a quarter of the level, all in the right ear.
        Mixer m(kRate);
        const VoiceId v = m.Play(&tone, At({4, 0, 0}, AttenuationModel::Inverse));
        const auto out = Render(m, 9600);
        AETHER_CHECK(Near(Rms(out, 0, 9600, 1), full_rms * 0.25f, 2e-3f));
        AETHER_CHECK(Rms(out, 0, 9600, 0) < 1e-3f);
        VoiceInfo info;
        AETHER_CHECK(m.GetVoiceInfo(v, info));
        AETHER_CHECK(Near(info.distance, 4.0f, 1e-5f) && Near(info.attenuation, 0.25f, 1e-5f) && Near(info.pan, 1.0f, 1e-5f));
        AETHER_CHECK(!info.is_virtual && Near(info.audibility, 0.25f, 1e-5f));

        // Walking the listener onto the source: full level, centred (equal power), smoothed.
        Listener l;
        l.position = {4, 0, 0};
        m.SetListener(l);
        const auto near = Render(m, 9600);
        AETHER_CHECK(Near(Rms(near, 4800, 9600, 0), full_rms * std::sqrt(0.5f), 2e-3f));
        AETHER_CHECK(Near(Rms(near, 4800, 9600, 1), full_rms * std::sqrt(0.5f), 2e-3f));
        AETHER_CHECK(std::fabs(near[1] - out[2 * 9599 + 1]) < 0.05f); // no jump at the change
    }
    {
        // Spatial blend halfway: halfway to the 3D gain and pan.
        Mixer m(kRate);
        PlayParams p = At({4, 0, 0}, AttenuationModel::Inverse);
        p.spatial_blend = 0.5f;
        const VoiceId v = m.Play(&tone, p);
        Render(m, 256);
        VoiceInfo info;
        m.GetVoiceInfo(v, info);
        AETHER_CHECK(Near(info.attenuation, 0.625f, 1e-5f) && Near(info.pan, 0.5f, 1e-5f));
        // Blend 0 is a plain 2D voice: no falloff, its own pan.
        Mixer flat(kRate);
        p.spatial_blend = 0.0f;
        p.pan = -1.0f;
        const VoiceId w = flat.Play(&tone, p);
        const auto out = Render(flat, 4800);
        flat.GetVoiceInfo(w, info);
        AETHER_CHECK(info.attenuation == 1.0f && info.pan == -1.0f);
        AETHER_CHECK(Near(Rms(out, 0, 4800, 0), full_rms, 2e-3f) && Rms(out, 0, 4800, 1) < 1e-3f);
    }
    {
        // Doppler: a source closing at a tenth of the speed of sound sounds 11% higher.
        Mixer m(kRate);
        PlayParams p = At({0, 0, -10});
        p.velocity = {0, 0, 34.3f};
        const VoiceId v = m.Play(&tone, p);
        const auto out = Render(m, 24000);
        AETHER_CHECK(Near(Frequency(out, 0, 24000), 440.0f * 343.0f / (343.0f - 34.3f), 2.0f));
        // Going away: lower. Doppler 0: unchanged.
        m.SetPosition(v, {0, 0, -10}, {0, 0, -34.3f});
        const auto away = Render(m, 24000);
        AETHER_CHECK(Near(Frequency(away, 2400, 24000), 440.0f * 343.0f / (343.0f + 34.3f), 2.0f));
        Mixer off(kRate);
        p.doppler = 0.0f;
        off.Play(&tone, p);
        AETHER_CHECK(Near(Frequency(Render(off, 24000), 0, 24000), 440.0f, 1.0f));
        // Pitch multiplies with doppler.
        VoiceInfo info;
        m.SetPitch(v, 2.0f);
        Render(m, 256);
        m.GetVoiceInfo(v, info);
        AETHER_CHECK(Near(info.pitch, 2.0f * 343.0f / (343.0f + 34.3f), 1e-4f));
    }
    {
        // Air absorption: far away, a high tone is filtered hard and a low one barely.
        const SoundWave high = GenerateTone(8000.0f, 1.0f, kRate, 0.5f);
        const SoundWave low = GenerateTone(100.0f, 1.0f, kRate, 0.5f);
        auto level = [&](const SoundWave& s, f32 distance) {
            Mixer m(kRate);
            PlayParams p = At({0, 0, -distance});
            p.attenuation.min_distance = 1.0f, p.attenuation.max_distance = 100.0f;
            p.attenuation.lowpass_at_max_hz = 300.0f;
            m.Play(&s, p);
            const auto out = Render(m, 9600);
            return Rms(out, 4800, 9600, 0);
        };
        const f32 high_near = level(high, 1.0f), high_far = level(high, 100.0f);
        const f32 low_near = level(low, 1.0f), low_far = level(low, 100.0f);
        AETHER_CHECK(Near(high_near, full_rms * std::sqrt(0.5f), 2e-3f)); // open at min_distance
        AETHER_CHECK(high_far < 0.1f * high_near);
        AETHER_CHECK(low_far > 0.9f * low_near);
    }
}

AETHER_TEST(Audio_OcclusionHookFiltersAndDucks) {
    const SoundWave tone = GenerateTone(440.0f, 1.0f, kRate, 0.5f);
    Mixer m(kRate);
    // A wall along x = 1: anything past it is blocked.
    usize queries = 0;
    m.occlusion_query = [&](const Vec3& listener, const Vec3& source) {
        ++queries;
        return (listener.x < 1.0f) != (source.x < 1.0f) ? 1.0f : 0.0f;
    };
    PlayParams p = At({2, 0, 0});
    p.occlusion = true;
    const VoiceId behind = m.Play(&tone, p);
    const VoiceId ignores = m.Play(&tone, At({2, 0, 0})); // doesn't ask for occlusion
    Render(m, 256);
    m.UpdateOcclusion();
    AETHER_CHECK(queries == 1);
    Render(m, 256);
    VoiceInfo info;
    m.GetVoiceInfo(behind, info);
    // Smoothed over 0.1 s (after a voice's first block): partly there after one block, all there after half a second.
    AETHER_CHECK(info.occlusion > 0.0f && info.occlusion < 0.2f);
    Render(m, 24000);
    m.GetVoiceInfo(behind, info);
    AETHER_CHECK(info.occlusion > 0.99f);
    AETHER_CHECK(info.lowpass_hz < 1300.0f);
    m.GetVoiceInfo(ignores, info);
    AETHER_CHECK(info.occlusion == 0.0f && info.lowpass_hz == kOpenCutoff);
    // Occluded: -12 dB and slightly dulled, compared to the one that ignores it.
    m.Stop(ignores);
    const auto blocked = Render(m, 4800);
    const f32 ratio = Rms(blocked, 0, 4800, 1) / (0.5f / std::sqrt(2.0f));
    AETHER_CHECK(ratio > 0.2f && ratio < DbToGain(-12.0f));
    // Stepping through the wall clears it.
    Listener l;
    l.position = {3, 0, 0};
    m.SetListener(l);
    m.UpdateOcclusion();
    Render(m, 24000);
    m.GetVoiceInfo(behind, info);
    AETHER_CHECK(info.occlusion < 0.01f);
    // Hosts can set it directly, on 2D voices too.
    const VoiceId flat = m.Play(&tone);
    AETHER_CHECK(m.SetOcclusion(flat, 0.5f) && !m.SetOcclusion(9999, 1.0f));
    Render(m, 256);
    m.GetVoiceInfo(flat, info);
    AETHER_CHECK(Near(info.occlusion, 0.5f, 1e-6f)); // the first block starts where it's set
}

AETHER_TEST(Audio_VoiceLimitingAndVirtualVoices) {
    const SoundWave tone = GenerateTone(440.0f, 1.0f, kRate, 0.5f);
    const SoundWave silent = [] {
        SoundWave s;
        s.sample_rate = kRate;
        s.samples.assign(kRate, 0.0f);
        return s;
    }();
    auto play = [](Mixer& m, const SoundWave& s, u8 priority, VirtualMode mode = VirtualMode::Continue, f32 db = 0.0f, bool loop = true) {
        PlayParams p;
        p.priority = priority;
        p.virtual_mode = mode;
        p.volume_db = db;
        p.loop = loop;
        return m.Play(&s, p);
    };
    {
        // Priorities decide who keeps a channel.
        Mixer m(kRate);
        m.SetMaxVoices(2);
        const VoiceId high = play(m, tone, 200), mid = play(m, tone, 100), low = play(m, tone, 50);
        Render(m, 4800);
        AETHER_CHECK(m.RealVoiceCount() == 2 && m.VirtualVoiceCount() == 1);
        VoiceInfo info;
        m.GetVoiceInfo(low, info);
        AETHER_CHECK(info.is_virtual);
        m.GetVoiceInfo(mid, info);
        AETHER_CHECK(!info.is_virtual);
        // Freeing a channel brings it back, at the time it would have reached (Continue).
        m.Stop(high);
        Render(m, 4800);
        m.GetVoiceInfo(low, info);
        AETHER_CHECK(!info.is_virtual);
        AETHER_CHECK(Near(m.PlaybackTime(low), 0.2f, 1e-3f));
    }
    {
        // Equal priority: the louder voice wins.
        Mixer m(kRate);
        m.SetMaxVoices(1);
        const VoiceId quiet = play(m, tone, 128, VirtualMode::Continue, -20.0f);
        const VoiceId loud = play(m, tone, 128);
        Render(m, 256);
        VoiceInfo a, b;
        m.GetVoiceInfo(quiet, a);
        m.GetVoiceInfo(loud, b);
        AETHER_CHECK(a.is_virtual && !b.is_virtual);
    }
    {
        // Restart comes back from the top; Stop just ends; a Continue one-shot that runs out while virtual is freed.
        Mixer m(kRate);
        m.SetMaxVoices(1);
        const VoiceId blocker = play(m, silent, 255);
        const VoiceId restart = play(m, tone, 10, VirtualMode::Restart);
        const VoiceId stop = play(m, tone, 10, VirtualMode::Stop);
        const VoiceId shot = play(m, tone, 10, VirtualMode::Continue, 0.0f, false);
        Render(m, 256);
        AETHER_CHECK(!m.IsPlaying(stop) && m.IsPlaying(restart) && m.IsPlaying(shot));
        Render(m, kRate + 256); // the one-shot's second runs out while it's virtual
        AETHER_CHECK(!m.IsPlaying(shot) && m.IsPlaying(restart));
        m.Stop(blocker);
        Render(m, 480);
        AETHER_CHECK(Near(m.PlaybackTime(restart), 0.01f, 1e-3f));
    }
    {
        // Losing and regaining a channel ramps over a block instead of clicking.
        const SoundWave dc = [] {
            SoundWave s;
            s.sample_rate = kRate;
            s.samples.assign(kRate, 0.5f);
            return s;
        }();
        Mixer m(kRate);
        m.SetMaxVoices(1);
        play(m, dc, 100);
        Render(m, 2560);
        const VoiceId blocker = play(m, silent, 200);
        const auto down = Render(m, 512);
        AETHER_CHECK(down[0] > 0.3f && std::fabs(down[2 * 255]) < 1e-6f && down[2 * 128] < down[0]);
        AETHER_CHECK(Rms(down, 256, 512) == 0.0f); // virtual: silent
        m.Stop(blocker);
        const auto up = Render(m, 512);
        AETHER_CHECK(up[0] < 0.01f && Near(up[2 * 255], down[0], 0.01f) && up[2 * 128] > up[0]);
    }
    {
        // Voices below the audibility threshold go virtual even with channels free, and come back when they're near.
        Mixer m(kRate);
        PlayParams p = At({0, 0, -100}, AttenuationModel::Linear);
        p.attenuation.max_distance = 50.0f;
        const VoiceId far = m.Play(&tone, p);
        Render(m, 256);
        VoiceInfo info;
        m.GetVoiceInfo(far, info);
        AETHER_CHECK(info.is_virtual && info.audibility == 0.0f && m.RealVoiceCount() == 0);
        m.SetPosition(far, {0, 0, -10});
        Render(m, 256);
        m.GetVoiceInfo(far, info);
        AETHER_CHECK(!info.is_virtual && Near(info.attenuation, 1.0f - 9.0f / 49.0f, 1e-5f));
        // Muting its bus makes it inaudible too.
        m.SetBusMuted(kMasterBus, true);
        Render(m, 256);
        m.GetVoiceInfo(far, info);
        AETHER_CHECK(info.is_virtual && m.IsPlaying(far));
    }
}
