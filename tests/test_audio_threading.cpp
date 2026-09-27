#include "aether/audio/audio_system.h"
#include "aether/audio/backend.h"
#include "aether/scene/components.h"
#include "test_framework.h"

#include <chrono>
#include <cmath>
#include <thread>

using namespace aether;
using namespace aether::audio;

// Phase 17 step 5: the lock-free handoff between the game thread and the
// audio thread, the null backend and miniaudio's (null) device.

namespace {

constexpr u32 kRate = 48000;

SoundWave Dc(f32 level, usize frames) {
    SoundWave s;
    s.sample_rate = kRate;
    s.samples.assign(frames, level);
    return s;
}

// Waits (polling) until `done`, up to `seconds`.
template <typename F>
bool WaitFor(F done, f64 seconds) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::duration<f64>(seconds);
    while (!done()) {
        if (std::chrono::steady_clock::now() > until) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

} // namespace

AETHER_TEST(Audio_SpscQueueAcrossThreads) {
    SpscQueue<u32> q(1000);
    AETHER_CHECK(q.Capacity() == 1024);
    u32 x = 0;
    AETHER_CHECK(!q.Pop(x));
    for (u32 i = 0; i < 1024; ++i) AETHER_CHECK(q.Push(i));
    AETHER_CHECK(!q.Push(9999) && q.Size() == 1024); // full
    for (u32 i = 0; i < 1024; ++i) AETHER_CHECK(q.Pop(x) && x == i);
    // One thread pushes 200k numbers while another pops them: all arrive, in order.
    constexpr u32 kCount = 200000;
    bool in_order = true;
    std::thread consumer([&] {
        u32 expected = 0, v = 0;
        while (expected < kCount) {
            if (q.Pop(v)) {
                in_order &= v == expected;
                ++expected;
            }
        }
    });
    for (u32 i = 0; i < kCount;) {
        if (q.Push(i)) ++i;
    }
    consumer.join();
    AETHER_CHECK(in_order && q.Size() == 0);
}

AETHER_TEST(Audio_ThreadedMixerMatchesDirect) {
    const SoundWave tone = GenerateTone(440.0f, 0.5f, kRate, 0.5f);
    const SoundWave dc = Dc(0.25f, 2000);
    // The same changes, made between blocks, to a mixer on one thread and to a threaded one.
    struct Ids {
        VoiceId tone = 0, dc = 0, spatial = 0;
    };
    auto step = [&](Mixer& m, Ids& ids, int i) {
        switch (i) {
        case 0: {
            PlayParams p;
            p.loop = true;
            p.volume_db = -6.0f;
            ids.tone = m.Play(&tone, p);
            PlayParams d;
            d.bus = m.FindBus("SFX");
            d.delay = 0.005;
            ids.dc = m.Play(&dc, d);
            break;
        }
        case 1:
            m.SetVolume(ids.tone, -12.0f);
            m.SetBusVolume(m.FindBus("SFX"), -6.0f);
            break;
        case 2: {
            PlayParams p;
            p.loop = true;
            p.spatial = true;
            p.position = {3, 0, 0};
            p.attenuation.model = AttenuationModel::Inverse;
            ids.spatial = m.Play(&tone, p);
            Listener l;
            l.position = {1, 0, 0};
            m.SetListener(l);
            break;
        }
        case 3:
            m.SetPosition(ids.spatial, {0, 0, -2});
            m.SetPan(ids.tone, -0.5f);
            m.SetPitch(ids.tone, 1.5f);
            break;
        case 4:
            m.Stop(ids.tone, 0.01f);
            m.Stop(ids.dc);
            break;
        case 5:
            m.AddEffect(kMasterBus, std::make_unique<FilterEffect>(Biquad::Type::LowPass, 800.0f));
            m.SetBusMuted(m.FindBus("SFX"), true);
            break;
        case 6: m.StopAll(); break;
        case 7: ids.tone = m.Play(&tone); break;
        }
    };
    Mixer direct(kRate), threaded(kRate);
    direct.AddDefaultBuses();
    threaded.AddDefaultBuses();
    auto backend = std::make_unique<NullBackend>(false, true);
    NullBackend* null = backend.get();
    AudioOutput output(threaded, std::move(backend));
    AETHER_CHECK(output.Start(256) && threaded.Threaded() && output.Running());
    AETHER_CHECK(threaded.AddBus("Late") == Mixer::kInvalidBus); // buses come first
    std::vector<f32> expected;
    Ids a, b;
    for (int i = 0; i < 8; ++i) {
        step(direct, a, i);
        step(threaded, b, i);
        AETHER_CHECK(a.tone == b.tone && a.dc == b.dc && a.spatial == b.spatial);
        if (i == 0) {
            // Queued but not applied: the game thread already sees them.
            AETHER_CHECK(threaded.IsPlaying(b.tone) && threaded.VoiceCount() == 2 && threaded.PlaybackTime(b.tone) == 0.0f);
            VoiceInfo info;
            AETHER_CHECK(!threaded.GetVoiceInfo(b.tone, info) && threaded.FramesRendered() == 0);
        }
        if (i == 1) AETHER_CHECK(threaded.BusVolume(threaded.FindBus("SFX")) == -6.0f); // at once
        if (i == 4) AETHER_CHECK(!threaded.IsPlaying(b.dc) && threaded.IsPlaying(b.tone)); // hard stop at once; the fade plays on
        if (i == 6) AETHER_CHECK(threaded.VoiceCount() == 0 && !threaded.IsPlaying(b.spatial));
        std::vector<f32> block(1024 * 2);
        for (u32 at = 0; at < 1024; at += 256) direct.Render(block.data() + at * 2, 256);
        expected.insert(expected.end(), block.begin(), block.end());
        // The audio thread: pump from another thread.
        std::thread audio([&] { null->Pump(1024); });
        audio.join();
        AETHER_CHECK(threaded.FramesRendered() == direct.FramesRendered());
        AETHER_CHECK(threaded.VoiceCount() == direct.VoiceCount() && threaded.RealVoiceCount() == direct.RealVoiceCount());
        AETHER_CHECK(threaded.Meter(kMasterBus).peak[0] == direct.Meter(kMasterBus).peak[0]);
        VoiceInfo x, y;
        AETHER_CHECK(direct.GetVoiceInfo(a.spatial, x) == threaded.GetVoiceInfo(b.spatial, y) && x.distance == y.distance && x.pan == y.pan);
        AETHER_CHECK(direct.PlaybackTime(a.tone) == threaded.PlaybackTime(b.tone));
    }
    const std::vector<f32> got = null->Captured();
    AETHER_CHECK(got.size() == expected.size() && got == expected);
    AETHER_CHECK(null->FramesPulled() == 8 * 1024);
    // Posted edits run on the audio thread, in order with the rest.
    bool ran = false;
    threaded.Post([&](Mixer& m) { ran = m.Threaded(); });
    AETHER_CHECK(!ran);
    null->Pump(256);
    AETHER_CHECK(ran);
    output.Stop();
    AETHER_CHECK(!threaded.Threaded() && !output.Running() && !null->Pump(256));
}

AETHER_TEST(Audio_RealTimeThreadStressAndSeamlessCues) {
    // A real-time audio thread (2.7 ms periods) while the game thread plays, moves and stops voices.
    Mixer mixer(kRate);
    mixer.AddDefaultBuses();
    mixer.occlusion_query = [](const Vec3&, const Vec3& s) { return s.x > 0.0f ? 1.0f : 0.0f; };
    auto backend = std::make_unique<NullBackend>(true, true);
    NullBackend* null = backend.get();
    AudioOutput output(mixer, std::move(backend));
    AETHER_CHECK(output.Start(128));
    const SoundWave blip = Dc(0.1f, 240), hum = GenerateTone(220.0f, 0.2f, kRate, 0.2f);
    u32 seed = 1;
    auto rnd = [&] { return (seed = seed * 1664525u + 1013904223u) >> 8; };
    std::vector<VoiceId> live;
    for (int i = 0; i < 3000; ++i) {
        PlayParams p;
        p.spatial = true;
        p.occlusion = true;
        p.position = {static_cast<f32>(rnd() % 20) - 10.0f, 0, -2};
        p.loop = rnd() % 4 == 0;
        live.push_back(mixer.Play(rnd() % 2 ? &blip : &hum, p));
        const VoiceId some = live[rnd() % live.size()];
        switch (rnd() % 5) {
        case 0: mixer.Stop(some); break;
        case 1: mixer.Stop(some, 0.01f); break;
        case 2: mixer.SetPosition(some, {static_cast<f32>(rnd() % 20) - 10.0f, 1, 0}); break;
        case 3: mixer.SetVolume(some, -3.0f); break;
        default: mixer.UpdateOcclusion(); break;
        }
        (void)mixer.VoiceCount();
        (void)mixer.Meter(kMasterBus);
        if (i % 100 == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    // More commands than the queue holds, at once: the game thread waits for room rather than dropping any.
    for (int i = 0; i < 20000; ++i) mixer.SetBusVolume(kMasterBus, i % 2 ? -1.0f : 0.0f);
    mixer.StopAll();
    AETHER_CHECK(mixer.VoiceCount() == 0);
    AETHER_CHECK(WaitFor([&] { return mixer.FramesRendered() > 0 && mixer.VoiceCount() == 0 && mixer.RealVoiceCount() == 0; }, 2.0));
    AETHER_CHECK(null->FramesPulled() > 0);
    output.Stop();

    // Cue loops scheduled from the game thread while audio runs stay seamless: each repeat
    // is asked for a little ahead, and the time it spent queued comes off its delay.
    Mixer m(kRate);
    auto rt = std::make_unique<NullBackend>(true, true);
    NullBackend* capture = rt.get();
    AudioOutput out(m, std::move(rt));
    SoundBank bank;
    bank.Add("one", Dc(0.25f, 480));
    bank.Add("two", Dc(0.5f, 240));
    SoundCue cue;
    CueNode one, two, cat, loop;
    one.id = 1, one.sound = "one";
    two.id = 2, two.sound = "two";
    cat.id = 3, cat.type = CueNodeType::Concatenator, cat.children = {1, 2};
    loop.id = 4, loop.type = CueNodeType::Loop, loop.children = {3};
    cue.nodes = {one, two, cat, loop};
    cue.root = 4;
    CuePlayer player(m, bank);
    player.lookahead = 0.03f;
    AETHER_CHECK(out.Start(128));
    const CueHandle h = player.Play(cue);
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
    while (std::chrono::steady_clock::now() < until) {
        player.Update();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    player.Stop(h);
    out.Stop();
    const std::vector<f32> got = capture->Captured();
    usize first = 0;
    while (first * 2 < got.size() && got[first * 2] == 0.0f) ++first;
    const f32 k = std::sqrt(0.5f);
    usize checked = 0;
    f32 worst = 0.0f;
    for (usize i = first; (i + 1) * 2 <= got.size() && i < first + 12000; ++i, ++checked) {
        const f32 expected = ((i - first) % 720 < 480 ? 0.25f : 0.5f) * k;
        worst = std::max(worst, std::fabs(got[i * 2] - expected));
    }
    AETHER_CHECK(checked == 12000 && worst < 1e-6f); // a quarter second of repeats, sample-exact
}

AETHER_TEST(Audio_MiniaudioNullDevice) {
    Mixer mixer(kRate);
    auto device = std::make_unique<MiniaudioBackend>(true);
    MiniaudioBackend* ma = device.get();
    AudioOutput output(mixer, std::move(device));
    std::string error;
    AETHER_CHECK(output.Start(480, &error));
    if (!error.empty()) std::printf("    %s\n", error.c_str());
    AETHER_CHECK(std::string(ma->Name()) == "miniaudio" && !ma->DeviceName().empty() && ma->SampleRate() == kRate);
    AETHER_CHECK(!ma->Start({}, [](f32*, u32) {}, &error) && error == "already running");
    AETHER_CHECK(WaitFor([&] { return ma->FramesPulled() >= 4800; }, 2.0)); // its thread is asking for audio
    const SoundWave shot = Dc(0.5f, 2400);                               // 50 ms
    const VoiceId v = mixer.Play(&shot);
    AETHER_CHECK(mixer.IsPlaying(v));
    AETHER_CHECK(WaitFor([&] { return !mixer.IsPlaying(v); }, 2.0)); // played through and freed on the audio thread
    output.Stop();
    AETHER_CHECK(!mixer.Threaded() && !ma->Running());
    // The default backend: the device if one opens, otherwise the null one.
    std::unique_ptr<AudioBackend> fallback = CreateDefaultBackend();
    AETHER_CHECK(fallback != nullptr && !fallback->Running());
}
