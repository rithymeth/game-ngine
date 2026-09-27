#include "aether/audio/cue_player.h"
#include "test_framework.h"

#include <cmath>
#include <fstream>
#include <iterator>

using namespace aether;
using namespace aether::audio;

// Phase 17 step 3: Ogg Vorbis and FLAC decoding, streamed voices, delayed
// starts, sound cues (.acue) and the cue player.

namespace {

constexpr u32 kRate = 48000;
const std::string kOgg = std::string(AETHER_REPO_ASSETS_DIR) + "/audio/tone_440_mono.ogg";
const std::string kFlac = std::string(AETHER_REPO_ASSETS_DIR) + "/audio/tone_440_660_stereo.flac";

std::vector<u8> ReadFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

std::vector<f32> Render(Mixer& m, u32 frames, u32 block = 256, CuePlayer* player = nullptr) {
    std::vector<f32> out(static_cast<usize>(frames) * 2);
    for (u32 at = 0; at < frames; at += block) {
        if (player != nullptr) player->Update();
        m.Render(out.data() + static_cast<usize>(at) * 2, std::min(block, frames - at));
    }
    return out;
}

f32 MaxDiff(const std::vector<f32>& a, const std::vector<f32>& b, usize from_frame = 0) {
    f32 d = 0.0f;
    for (usize i = from_frame * 2; i < std::min(a.size(), b.size()); ++i) d = std::max(d, std::fabs(a[i] - b[i]));
    return d;
}

// Rising zero crossings per second over interleaved samples of `channels`.
f32 Frequency(const std::vector<f32>& s, u32 channels, u32 rate, int ch = 0) {
    usize count = 0, first = 0, last = 0;
    const usize frames = s.size() / channels;
    for (usize i = 1; i < frames; ++i) {
        if (s[(i - 1) * channels + ch] < 0.0f && s[i * channels + ch] >= 0.0f) {
            if (count == 0) first = i;
            last = i;
            ++count;
        }
    }
    return count < 2 ? 0.0f : static_cast<f32>(count - 1) * rate / static_cast<f32>(last - first);
}

SoundWave Dc(f32 level, usize frames) {
    SoundWave s;
    s.sample_rate = kRate;
    s.samples.assign(frames, level);
    return s;
}

CueNode Node(u32 id, CueNodeType type, std::vector<u32> children = {}, const std::string& sound = {}) {
    CueNode n;
    n.id = id;
    n.type = type;
    n.children = std::move(children);
    n.sound = sound;
    return n;
}

bool Has(const std::vector<CueDiagnostic>& d, const std::string& code) {
    return std::any_of(d.begin(), d.end(), [&](const CueDiagnostic& x) { return x.code == code; });
}

} // namespace

AETHER_TEST(Audio_DecodesOggAndFlacAndStreams) {
    const std::vector<u8> ogg = ReadFile(kOgg), flac = ReadFile(kFlac);
    AETHER_CHECK(!ogg.empty() && !flac.empty());
    AETHER_CHECK(DetectFormat(ogg) == SoundFormat::OggVorbis && DetectFormat(flac) == SoundFormat::Flac);
    AETHER_CHECK(DetectFormat(EncodeWav(GenerateTone(440.0f, 0.1f))) == SoundFormat::Wav);
    AETHER_CHECK(std::string(SoundFormatName(SoundFormat::Flac)) == "FLAC");

    // Ogg Vorbis: 1 s of 440 Hz at 22.05 kHz, mono (lossy: check its pitch and level).
    SoundWave v;
    std::string error;
    AETHER_CHECK(DecodeSound(ogg, v, &error));
    AETHER_CHECK(v.sample_rate == 22050 && v.channels == 1 && v.Frames() == 22050);
    AETHER_CHECK(std::fabs(Frequency(v.samples, 1, 22050) - 440.0f) < 1.0f);
    f64 sum = 0;
    for (f32 x : v.samples) sum += static_cast<f64>(x) * x;
    AETHER_CHECK(std::fabs(std::sqrt(sum / v.samples.size()) - 0.5 / std::sqrt(2.0)) < 0.02);

    // FLAC: 1 s of 440 Hz left and 660 Hz right at 16 kHz, 16-bit. Lossless: every sample matches.
    SoundWave f;
    AETHER_CHECK(DecodeFlac(flac, f, &error));
    AETHER_CHECK(f.sample_rate == 16000 && f.channels == 2 && f.Frames() == 16000);
    f32 worst = 0.0f;
    for (usize i = 0; i < f.Frames(); ++i) {
        for (u32 c = 0; c < 2; ++c) {
            const f64 x = 0.5 * std::sin(2.0 * 3.14159265358979323846 * (c == 0 ? 440.0 : 660.0) * i / 16000.0);
            const f32 expected = static_cast<f32>(std::round(x * 32767.0) / 32768.0);
            worst = std::max(worst, std::fabs(f.samples[i * 2 + c] - expected));
        }
    }
    AETHER_CHECK(worst < 1e-6f);

    // Bad files are refused with a reason.
    SoundWave bad;
    for (const std::vector<u8>& junk : {std::vector<u8>{'n', 'o', 'p', 'e', 0, 0, 0, 0, 0, 0, 0, 0}, std::vector<u8>(ogg.begin(), ogg.begin() + 100),
                                        std::vector<u8>(flac.begin(), flac.begin() + 20), std::vector<u8>{'O', 'g', 'g', 'S', 1, 2, 3}}) {
        error.clear();
        AETHER_CHECK(!DecodeSound(junk, bad, &error) && !error.empty());
    }
    AETHER_CHECK(!DecodeFlac(ogg, bad, &error) && !DecodeOggVorbis(flac, bad, &error));

    // Streams read in chunks and seek to exactly the same samples.
    auto flac_bytes = std::make_shared<const std::vector<u8>>(flac);
    auto s = OpenStream(flac_bytes, &error);
    AETHER_CHECK(s != nullptr && s->Frames() == 16000 && s->Channels() == 2 && s->SampleRate() == 16000);
    AETHER_CHECK(std::fabs(s->Duration() - 1.0f) < 1e-6f);
    std::vector<f32> chunked;
    std::vector<f32> buf(2 * 1000);
    while (const u32 got = s->Read(buf.data(), 1000)) chunked.insert(chunked.end(), buf.begin(), buf.begin() + got * 2);
    AETHER_CHECK(chunked == f.samples);
    AETHER_CHECK(s->Seek(8000) && s->Read(buf.data(), 10) == 10);
    AETHER_CHECK(std::equal(buf.begin(), buf.begin() + 20, f.samples.begin() + 16000));
    auto o = OpenStream(std::make_shared<const std::vector<u8>>(ogg));
    AETHER_CHECK(o != nullptr && o->Frames() == 22050 && o->Seek(11025) && o->Read(buf.data(), 64) == 64);
    f32 seek_error = 0.0f;
    for (usize i = 0; i < 64; ++i) seek_error = std::max(seek_error, std::fabs(buf[i] - v.samples[11025 + i]));
    AETHER_CHECK(seek_error < 1e-4f);
    // From disk, and from memory as WAV.
    auto disk = OpenStreamFile(kFlac, &error);
    AETHER_CHECK(disk != nullptr && disk->Frames() == 16000 && disk->Read(buf.data(), 1000) == 1000);
    AETHER_CHECK(std::equal(buf.begin(), buf.begin() + 2000, f.samples.begin()));
    AETHER_CHECK(OpenStreamFile(kOgg) != nullptr);
    AETHER_CHECK(OpenStreamFile(std::string(AETHER_REPO_ASSETS_DIR) + "/audio/missing.ogg", &error) == nullptr && !error.empty());
    auto wav = OpenStream(std::make_shared<const std::vector<u8>>(EncodeWav(GenerateTone(440.0f, 0.5f, 44100))));
    AETHER_CHECK(wav != nullptr && wav->Frames() == 22050 && wav->SampleRate() == 44100);
    AETHER_CHECK(OpenStream(std::make_shared<const std::vector<u8>>(std::vector<u8>{1, 2, 3}), &error) == nullptr);
    AETHER_CHECK(MakeWaveStream(std::make_shared<const SoundWave>(GenerateTone(440.0f, 0.1f))) != nullptr);
    AETHER_CHECK(MakeWaveStream(std::make_shared<const SoundWave>()) == nullptr);
}

AETHER_TEST(Audio_StreamedVoicesMatchDecodedOnes) {
    const std::vector<u8> flac = ReadFile(kFlac), ogg = ReadFile(kOgg);
    SoundWave f, v;
    DecodeSound(flac, f);
    DecodeSound(ogg, v);
    auto flac_bytes = std::make_shared<const std::vector<u8>>(flac);
    auto ogg_bytes = std::make_shared<const std::vector<u8>>(ogg);

    // The same sound, decoded up front or streamed, mixes to the same samples (resampled 16 kHz -> 48 kHz).
    for (bool loop : {false, true}) {
        PlayParams p;
        p.loop = loop;
        Mixer a(kRate), b(kRate);
        a.Play(&f, p);
        const VoiceId sv = b.PlayStream(OpenStream(flac_bytes), p);
        AETHER_CHECK(sv != 0);
        const u32 frames = loop ? 130000 : 50000; // looping wraps twice
        const auto da = Render(a, frames), db = Render(b, frames);
        AETHER_CHECK(MaxDiff(da, db, 8) < 1e-6f); // (the very start differs: a looped array wraps backwards)
        AETHER_CHECK(b.IsPlaying(sv) == loop);
    }
    {
        // Vorbis too, with a pitch; and streams keep their place.
        PlayParams p;
        p.pitch = 1.5f;
        p.start_time = 0.25f;
        Mixer a(kRate), b(kRate);
        a.Play(&v, p);
        const VoiceId sv = b.PlayStream(OpenStream(ogg_bytes), p);
        const auto da = Render(a, 9600), db = Render(b, 9600);
        AETHER_CHECK(MaxDiff(da, db, 8) < 1e-5f);
        AETHER_CHECK(std::fabs(b.PlaybackTime(sv) - (0.25f + 0.2f * 1.5f)) < 1e-3f);
    }
    {
        // Losing its channel and coming back (Continue): it seeks to where it would be.
        auto run = [&](bool streamed) {
            Mixer m(kRate);
            m.SetMaxVoices(1);
            PlayParams p;
            p.loop = true;
            p.priority = 10;
            if (streamed) m.PlayStream(OpenStream(flac_bytes), p);
            else m.Play(&f, p);
            std::vector<f32> out = Render(m, 12800);
            const SoundWave quiet = Dc(0.0f, 100);
            PlayParams hi;
            hi.priority = 200;
            hi.loop = true;
            const VoiceId blocker = m.Play(&quiet, hi);
            const auto mid = Render(m, 70000); // longer than the sound: wraps while virtual
            m.Stop(blocker);
            const auto back = Render(m, 12800);
            out.insert(out.end(), mid.begin(), mid.end());
            out.insert(out.end(), back.begin(), back.end());
            return out;
        };
        AETHER_CHECK(MaxDiff(run(false), run(true), 8) < 1e-6f);
    }
    {
        // Delayed starts are sample-accurate across blocks; the clock counts frames.
        Mixer m(kRate);
        const SoundWave dc = Dc(0.5f, 1000);
        PlayParams p;
        p.delay = 500.0 / kRate;
        p.pan = -1.0f;
        m.Play(&dc, p);
        const auto out = Render(m, 2000, 128);
        AETHER_CHECK(out[2 * 499] == 0.0f && out[2 * 500] > 0.49f && out[2 * 1499] > 0.49f && out[2 * 1500] == 0.0f);
        AETHER_CHECK(m.FramesRendered() == 2000);
        AETHER_CHECK(m.PlayStream(nullptr) == 0);
    }
}

AETHER_TEST(Audio_CueGraphsEvaluateValidateAndSave) {
    const std::map<std::string, f64> lengths = {{"a", 1.0}, {"b", 0.5}, {"c", 2.0}};
    const SoundDuration duration = [&](const std::string& n) {
        const auto it = lengths.find(n);
        return it == lengths.end() ? -1.0 : it->second;
    };
    auto cue_of = [](std::vector<CueNode> nodes, u32 root) {
        SoundCue c;
        c.nodes = std::move(nodes);
        c.root = root;
        return c;
    };
    const CueNode A = Node(1, CueNodeType::Wave, {}, "a"), B = Node(2, CueNodeType::Wave, {}, "b"), C = Node(3, CueNodeType::Wave, {}, "c");
    CueState state(42);
    {
        // Concatenator: back to back.
        const auto plan = EvaluateCue(cue_of({A, B, Node(10, CueNodeType::Concatenator, {1, 2, 1})}, 10), state, duration);
        AETHER_CHECK(plan.items.size() == 3 && plan.items[1].offset == 1.0 && plan.items[2].offset == 1.5 && plan.duration == 2.5);
        AETHER_CHECK(plan.items[1].sound == "b" && plan.items[1].node == 2 && plan.tails.empty());
    }
    {
        // A modulator's pitch shortens what's under it; Mix plays together with per-input volume; Delay waits.
        CueNode mod = Node(10, CueNodeType::Modulator, {11});
        mod.pitch_min = mod.pitch_max = 2.0f;
        const auto fast = EvaluateCue(cue_of({A, B, mod, Node(11, CueNodeType::Concatenator, {1, 2})}, 10), state, duration);
        AETHER_CHECK(fast.items.size() == 2 && fast.items[1].offset == 0.5 && fast.duration == 0.75 && fast.items[0].pitch == 2.0f);
        CueNode mix = Node(10, CueNodeType::Mix, {1, 3});
        mix.input_db = {-6.0f};
        const auto both = EvaluateCue(cue_of({A, C, mix}, 10), state, duration);
        AETHER_CHECK(both.items.size() == 2 && both.items[1].offset == 0.0 && both.duration == 2.0);
        AETHER_CHECK(both.items[0].volume_db == -6.0f && both.items[1].volume_db == 0.0f);
        CueNode delay = Node(10, CueNodeType::Delay, {1});
        delay.delay_min = delay.delay_max = 0.25f;
        const auto later = EvaluateCue(cue_of({A, delay}, 10), state, duration);
        AETHER_CHECK(later.items[0].offset == 0.25 && later.duration == 1.25);
    }
    {
        // Loops: counted, forever over a wave (one looping voice), forever over more (a tail).
        CueNode three = Node(10, CueNodeType::Loop, {2});
        three.count = 3;
        const auto counted = EvaluateCue(cue_of({B, three}, 10), state, duration);
        AETHER_CHECK(counted.items.size() == 3 && counted.items[2].offset == 1.0 && counted.duration == 1.5);
        const auto looped = EvaluateCue(cue_of({A, Node(10, CueNodeType::Loop, {1})}, 10), state, duration);
        AETHER_CHECK(looped.items.size() == 1 && looped.items[0].loop && std::isinf(looped.duration) && looped.tails.empty());
        const auto endless =
            EvaluateCue(cue_of({A, B, Node(10, CueNodeType::Loop, {11}), Node(11, CueNodeType::Concatenator, {1, 2})}, 10), state, duration);
        AETHER_CHECK(endless.items.size() == 2 && endless.tails.size() == 1 && endless.tails[0].node == 11 && endless.tails[0].offset == 1.5);
        AETHER_CHECK(std::isinf(endless.duration));
        // An intro, then a loop.
        const auto intro =
            EvaluateCue(cue_of({A, B, Node(10, CueNodeType::Concatenator, {2, 11}), Node(11, CueNodeType::Loop, {1})}, 10), state, duration);
        AETHER_CHECK(intro.items.size() == 2 && intro.items[1].offset == 0.5 && intro.items[1].loop && !intro.items[0].loop);
        // Unknown sounds play nothing.
        const auto nothing = EvaluateCue(cue_of({Node(1, CueNodeType::Wave, {}, "zzz")}, 1), state, duration);
        AETHER_CHECK(nothing.items.empty() && nothing.duration == 0.0);
    }
    {
        // Random: weights, no repeats; Sequence: in turn; Modulator: within its ranges; all repeatable from a seed.
        CueNode r = Node(10, CueNodeType::Random, {1, 2});
        r.weights = {1.0f, 0.0f};
        const SoundCue only_a = cue_of({A, B, r}, 10);
        for (int i = 0; i < 100; ++i) AETHER_CHECK(EvaluateCue(only_a, state, duration).items[0].sound == "a");
        r.weights = {1.0f, 3.0f};
        const SoundCue weighted = cue_of({A, B, r}, 10);
        int bs = 0;
        for (int i = 0; i < 4000; ++i) bs += EvaluateCue(weighted, state, duration).items[0].sound == "b";
        AETHER_CHECK(bs > 2880 && bs < 3120); // 75% +- 3%
        r.weights.clear();
        r.no_repeat = true;
        r.children = {1, 2, 3};
        const SoundCue fresh = cue_of({A, B, C, r}, 10);
        std::string last;
        for (int i = 0; i < 300; ++i) {
            const std::string now = EvaluateCue(fresh, state, duration).items[0].sound;
            AETHER_CHECK(now != last);
            last = now;
        }
        const SoundCue seq = cue_of({A, B, C, Node(10, CueNodeType::Sequence, {1, 2, 3})}, 10);
        std::string order;
        for (int i = 0; i < 4; ++i) order += EvaluateCue(seq, state, duration).items[0].sound;
        AETHER_CHECK(order == "abca");
        CueNode mod = Node(10, CueNodeType::Modulator, {1});
        mod.volume_min_db = -6.0f, mod.volume_max_db = -3.0f;
        mod.pitch_min = 0.9f, mod.pitch_max = 1.1f;
        const SoundCue varied = cue_of({A, mod}, 10);
        f32 lo = 1e9f, hi = -1e9f;
        for (int i = 0; i < 200; ++i) {
            const CueItem it = EvaluateCue(varied, state, duration).items[0];
            AETHER_CHECK(it.volume_db >= -6.0f && it.volume_db <= -3.0f && it.pitch >= 0.9f && it.pitch <= 1.1f);
            lo = std::min(lo, it.pitch), hi = std::max(hi, it.pitch);
        }
        AETHER_CHECK(hi - lo > 0.1f);
        CueState s1(7), s2(7);
        for (int i = 0; i < 50; ++i) AETHER_CHECK(EvaluateCue(weighted, s1, duration).items[0].sound == EvaluateCue(weighted, s2, duration).items[0].sound);
    }
    {
        // Validation.
        const auto exists = [&](const std::string& n) { return lengths.count(n) != 0; };
        const SoundCue good = cue_of({A, B, Node(10, CueNodeType::Random, {1, 2})}, 10);
        AETHER_CHECK(ValidateCue(good, exists).empty());
        AETHER_CHECK(Has(ValidateCue(cue_of({A}, 99)), "CU001"));
        AETHER_CHECK(Has(ValidateCue(cue_of({A, Node(1, CueNodeType::Wave, {}, "b")}, 1)), "CU002"));
        AETHER_CHECK(Has(ValidateCue(cue_of({Node(10, CueNodeType::Random, {5})}, 10)), "CU003"));
        AETHER_CHECK(Has(ValidateCue(cue_of({Node(10, CueNodeType::Concatenator, {11}), Node(11, CueNodeType::Delay, {10})}, 10)), "CU004"));
        AETHER_CHECK(Has(ValidateCue(cue_of({A, B, Node(10, CueNodeType::Modulator, {1, 2})}, 10)), "CU005"));
        AETHER_CHECK(Has(ValidateCue(cue_of({Node(1, CueNodeType::Wave, {2}, "a"), B}, 1)), "CU005"));
        AETHER_CHECK(Has(ValidateCue(cue_of({Node(10, CueNodeType::Mix)}, 10)), "CU005"));
        AETHER_CHECK(Has(ValidateCue(cue_of({Node(1, CueNodeType::Wave)}, 1)), "CU006"));
        AETHER_CHECK(Has(ValidateCue(cue_of({Node(1, CueNodeType::Wave, {}, "zzz")}, 1), exists), "CU007"));
        CueNode bad_mod = Node(10, CueNodeType::Modulator, {1});
        bad_mod.pitch_min = 1.2f, bad_mod.pitch_max = 1.1f;
        AETHER_CHECK(Has(ValidateCue(cue_of({A, bad_mod}, 10)), "CU008"));
        CueNode zero = Node(10, CueNodeType::Random, {1, 2});
        zero.weights = {0.0f, 0.0f};
        AETHER_CHECK(Has(ValidateCue(cue_of({A, B, zero}, 10)), "CU009"));
        const auto unreachable = ValidateCue(cue_of({A, B, Node(10, CueNodeType::Concatenator, {11, 2}), Node(11, CueNodeType::Loop, {1}), C}, 10));
        AETHER_CHECK(Has(unreachable, "CU010") && Has(unreachable, "CU011"));
        AETHER_CHECK(std::none_of(unreachable.begin(), unreachable.end(), [](const CueDiagnostic& d) { return d.error; })); // both warnings
    }
    {
        // .acue round trip.
        SoundCue cue = cue_of({A, B, C}, 10);
        CueNode r = Node(10, CueNodeType::Random, {11, 3});
        r.weights = {2.0f, 1.0f};
        r.no_repeat = true;
        r.x = 120.0f, r.y = -40.0f;
        CueNode mod = Node(11, CueNodeType::Modulator, {12});
        mod.volume_min_db = -3.0f, mod.pitch_min = 0.95f, mod.pitch_max = 1.05f;
        CueNode loop = Node(12, CueNodeType::Loop, {13});
        loop.count = 2;
        CueNode mix = Node(13, CueNodeType::Mix, {1, 14});
        mix.input_db = {-3.0f, 0.0f};
        CueNode delay = Node(14, CueNodeType::Delay, {2});
        delay.delay_min = 0.1f, delay.delay_max = 0.2f;
        cue.nodes.insert(cue.nodes.end(), {r, mod, loop, mix, delay});
        cue.nodes[0].looping = true;
        cue.name = "Footsteps";
        cue.volume_db = -4.0f, cue.pitch = 1.1f, cue.bus = "Voice", cue.priority = 200;
        cue.virtual_mode = VirtualMode::Restart;
        cue.spatial = true, cue.occlusion = true, cue.spatial_blend = 0.8f, cue.doppler = 0.5f;
        cue.attenuation.model = AttenuationModel::Custom;
        cue.attenuation.curve = {{0.0f, 1.0f}, {1.0f, 0.0f}};
        cue.attenuation.lowpass_at_max_hz = 800.0f;
        const std::string text = SaveCue(cue);
        SoundCue back;
        std::string error;
        AETHER_CHECK(LoadCue(text, back, &error));
        AETHER_CHECK(SaveCue(back) == text);
        AETHER_CHECK(back.name == "Footsteps" && back.bus == "Voice" && back.priority == 200 && back.virtual_mode == VirtualMode::Restart);
        AETHER_CHECK(back.attenuation.model == AttenuationModel::Custom && back.attenuation.curve.size() == 2 && back.nodes[0].looping);
        AETHER_CHECK(back.Find(10)->weights.size() == 2 && back.Find(10)->no_repeat && back.Find(10)->x == 120.0f && back.Find(14)->delay_max == 0.2f);
        AETHER_CHECK(back.NextId() == 15);
        AETHER_CHECK(!LoadCue("{not json", back, &error) && !error.empty());
        AETHER_CHECK(!LoadCue(R"({"version":1,"nodes":[{"id":1,"type":"Nope"}]})", back, &error) && error.find("Nope") != std::string::npos);
        AETHER_CHECK(!LoadCue(R"({"version":9})", back, &error) && error.find("newer") != std::string::npos);
        AETHER_CHECK(!LoadCue(R"({"output":{"attenuation":{"model":"Cubic"}}})", back, &error));
        AETHER_CHECK(!LoadCue(R"({"nodes":[{"type":"Wave"}]})", back, &error));
        AETHER_CHECK(!LoadCue(R"([1,2])", back, &error));
    }
}

AETHER_TEST(Audio_CuePlayerSchedulesSampleAccurately) {
    SoundBank bank;
    bank.Add("one", Dc(0.25f, 480));
    bank.Add("two", Dc(0.5f, 240));
    const f32 k = std::sqrt(0.5f); // equal-power centre
    auto cue_of = [](std::vector<CueNode> nodes, u32 root) {
        SoundCue c;
        c.nodes = std::move(nodes);
        c.root = root;
        return c;
    };
    const CueNode one = Node(1, CueNodeType::Wave, {}, "one"), two = Node(2, CueNodeType::Wave, {}, "two");
    {
        // Concatenated sounds butt up exactly, across render blocks.
        Mixer m(kRate);
        CuePlayer player(m, bank);
        const SoundCue cue = cue_of({one, two, Node(10, CueNodeType::Concatenator, {1, 2, 1})}, 10);
        const CueHandle h = player.Play(cue);
        AETHER_CHECK(h != 0 && player.IsPlaying(h) && player.Voices(h).size() == 3);
        const auto out = Render(m, 1500, 256, &player);
        f32 worst = 0.0f;
        for (usize i = 0; i < 1500; ++i) {
            const f32 expected = i < 480 ? 0.25f * k : i < 720 ? 0.5f * k : i < 1200 ? 0.25f * k : 0.0f;
            worst = std::max(worst, std::fabs(out[2 * i] - expected));
        }
        AETHER_CHECK(worst < 1e-6f);
        player.Update();
        AETHER_CHECK(!player.IsPlaying(h) && player.ActiveCount() == 0);
    }
    {
        // A loop forever over a concatenation repeats seamlessly and without drift for a second.
        Mixer m(kRate);
        CuePlayer player(m, bank);
        const SoundCue cue = cue_of({one, two, Node(10, CueNodeType::Loop, {11}), Node(11, CueNodeType::Concatenator, {1, 2})}, 10);
        const CueHandle h = player.Play(cue);
        const auto out = Render(m, kRate, 256, &player);
        f32 worst = 0.0f;
        for (usize i = 0; i < kRate; ++i) worst = std::max(worst, std::fabs(out[2 * i] - (i % 720 < 480 ? 0.25f : 0.5f) * k));
        AETHER_CHECK(worst < 1e-6f);
        // Scheduled a lookahead (0.25 s: ~17 repeats of 720 frames, 2 voices each) ahead, not all at once.
        AETHER_CHECK(player.IsPlaying(h) && m.VoiceCount() >= 30 && m.VoiceCount() <= 40);
        AETHER_CHECK(player.Stop(h));
        const auto after = Render(m, 1024, 256, &player);
        AETHER_CHECK(MaxDiff(after, std::vector<f32>(after.size(), 0.0f)) == 0.0f);
        AETHER_CHECK(!player.IsPlaying(h) && player.ActiveCount() == 0 && m.VoiceCount() == 0);
    }
    {
        // A streamed sound plays exactly as its decoded copy.
        const std::vector<u8> flac = ReadFile(kFlac);
        SoundWave f;
        DecodeSound(flac, f);
        SoundBank sb;
        sb.Add("decoded", f);
        AETHER_CHECK(sb.AddStreamed("streamed", std::make_shared<const std::vector<u8>>(flac)));
        AETHER_CHECK(!sb.AddStreamed("junk", std::make_shared<const std::vector<u8>>(std::vector<u8>{1, 2, 3})) && !sb.Contains("junk"));
        AETHER_CHECK(sb.IsStreamed("streamed") && !sb.IsStreamed("decoded") && sb.Find("streamed") == nullptr);
        AETHER_CHECK(std::fabs(sb.Duration("streamed") - 1.0) < 1e-9 && sb.Duration("nope") < 0.0);
        Mixer a(kRate), b(kRate);
        CuePlayer pa(a, sb), pb(b, sb);
        const SoundCue ca = cue_of({Node(1, CueNodeType::Wave, {}, "decoded")}, 1), cb = cue_of({Node(1, CueNodeType::Wave, {}, "streamed")}, 1);
        pa.Play(ca);
        pb.Play(cb);
        AETHER_CHECK(MaxDiff(Render(a, 20000, 256, &pa), Render(b, 20000, 256, &pb), 8) < 1e-6f);
    }
    {
        // Output settings: volume (cue and instance), bus, fade-in, and position for spatial cues.
        Mixer m(kRate);
        m.AddDefaultBuses();
        CuePlayer player(m, bank);
        SoundCue cue = cue_of({Node(1, CueNodeType::Wave, {}, "two"), Node(10, CueNodeType::Loop, {1})}, 10);
        cue.volume_db = -6.0206f;
        cue.bus = "Music";
        const CueHandle h = player.Play(cue, {.volume_db = -6.0206f});
        auto out = Render(m, 1024, 256, &player);
        AETHER_CHECK(std::fabs(out[2 * 1000] - 0.5f * 0.25f * k) < 1e-4f);
        AETHER_CHECK(player.SetVolume(h, 0.0f));
        out = Render(m, 2048, 256, &player);
        AETHER_CHECK(std::fabs(out[2 * 2000] - 0.5f * 0.5f * k) < 1e-4f);
        m.SetBusMuted(m.FindBus("Music"), true);
        out = Render(m, 2048, 256, &player);
        AETHER_CHECK(std::fabs(out[2 * 2000]) < 1e-6f);

        SoundCue spatial = cue_of({Node(1, CueNodeType::Wave, {}, "two"), Node(10, CueNodeType::Loop, {1})}, 10);
        spatial.spatial = true;
        spatial.attenuation.model = AttenuationModel::Inverse;
        const CueHandle s = player.Play(spatial, {.position = {4, 0, 0}});
        Render(m, 256, 256, &player);
        VoiceInfo info;
        AETHER_CHECK(m.GetVoiceInfo(player.Voices(s)[0], info) && std::fabs(info.distance - 4.0f) < 1e-5f && info.pan > 0.99f);
        AETHER_CHECK(player.SetPosition(s, {0, 0, -2}));
        Render(m, 256, 256, &player);
        m.GetVoiceInfo(player.Voices(s)[0], info);
        AETHER_CHECK(std::fabs(info.distance - 2.0f) < 1e-5f && std::fabs(info.pan) < 1e-5f);

        Mixer fm(kRate);
        CuePlayer fp(fm, bank);
        fp.Play(cue_of({Node(1, CueNodeType::Wave, {}, "two"), Node(10, CueNodeType::Loop, {1})}, 10), {.fade_in = 0.01f});
        out = Render(fm, 960, 256, &fp);
        AETHER_CHECK(out[2 * 10] < 0.1f * 0.5f * k && std::fabs(out[2 * 959] - 0.5f * k) < 1e-3f);
        fp.StopAll();
        AETHER_CHECK(fp.ActiveCount() == 0 && fm.VoiceCount() == 0);
    }
    {
        // A Sequence remembers where it is between plays; cues with nothing to play return 0.
        Mixer m(kRate);
        CuePlayer player(m, bank);
        const SoundCue seq = cue_of({one, two, Node(10, CueNodeType::Sequence, {1, 2})}, 10);
        auto length = [&] {
            player.Play(seq);
            const auto out = Render(m, 1000, 256, &player);
            usize n = 0;
            for (usize i = 0; i < 1000; ++i) n += out[2 * i] != 0.0f;
            return n;
        };
        AETHER_CHECK(length() == 480 && length() == 240 && length() == 480);
        AETHER_CHECK(player.Play(cue_of({Node(1, CueNodeType::Wave, {}, "missing")}, 1)) == 0);
        AETHER_CHECK(!player.Stop(12345) && !player.SetVolume(12345, 0.0f));
    }
}
