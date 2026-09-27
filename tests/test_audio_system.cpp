#include "aether/audio/audio_system.h"
#include "aether/blueprint/compiler.h"
#include "aether/blueprint/vm.h"
#include "aether/scene/components.h"
#include "aether/scene/hierarchy.h"
#include "test_framework.h"

#include <cmath>
#include <map>

using namespace aether;
using namespace aether::audio;

// Phase 17 step 4: AudioSource, AudioListener and ReverbZone components,
// the AudioSystem, and the Audio Blueprint nodes.

namespace {

constexpr u32 kRate = 48000;

SoundWave Dc(f32 level, f32 seconds) {
    SoundWave s;
    s.sample_rate = kRate;
    s.samples.assign(static_cast<usize>(seconds * kRate), level);
    return s;
}

SoundCue WaveCue(const std::string& sound, bool loop, bool spatial) {
    SoundCue c;
    CueNode w;
    w.id = 1;
    w.sound = sound;
    w.looping = loop;
    c.nodes.push_back(w);
    c.root = 1;
    c.spatial = spatial;
    c.attenuation.model = AttenuationModel::Inverse;
    return c;
}

// A mixer, a bank and cues, shared by the tests.
struct Rig {
    Mixer mixer{kRate};
    SoundBank bank;
    std::map<std::string, SoundCue> cues;
    Rig() {
        mixer.AddDefaultBuses();
        bank.Add("hum", Dc(0.5f, 1.0f));
        bank.Add("blip", Dc(0.25f, 0.1f));
        cues["Hum"] = WaveCue("hum", true, true);
        cues["Blip"] = WaveCue("blip", false, false);
        cues["Broken"] = WaveCue("missing", false, false);
    }
    std::function<const SoundCue*(const std::string&)> Lookup() {
        return [this](const std::string& n) {
            const auto it = cues.find(n);
            return it == cues.end() ? nullptr : &it->second;
        };
    }
    std::vector<f32> Render(f32 seconds) {
        std::vector<f32> out(static_cast<usize>(seconds * kRate) * 2);
        const u32 frames = static_cast<u32>(out.size() / 2);
        for (u32 at = 0; at < frames; at += 256) mixer.Render(out.data() + static_cast<usize>(at) * 2, std::min(256u, frames - at));
        return out;
    }
    VoiceInfo Info(CuePlayer& player, CueHandle h) {
        VoiceInfo info;
        const auto voices = player.Voices(h);
        if (!voices.empty()) mixer.GetVoiceInfo(voices[0], info);
        return info;
    }
};

bool Near(f32 a, f32 b, f32 tolerance = 1e-4f) { return std::fabs(a - b) <= tolerance; }

Quaternion Yaw(f32 degrees) { return Quaternion::FromAxisAngle({0, 1, 0}, degrees * 3.14159265f / 180.0f); }

} // namespace

AETHER_TEST(Audio_SourcesFollowEntitiesAndReportEnds) {
    Rig rig;
    World world;
    AudioSystem sys(world, rig.mixer, rig.bank, rig.Lookup());
    AETHER_CHECK(AudioSystem::Active() == &sys);
    const Entity ear = world.CreateEntity(Transform{Vec3(), Quaternion::Identity()}, AudioListener{});
    AudioSource hum;
    hum.cue = "Hum";
    const Entity src = world.CreateEntity(Transform{Vec3(4, 0, 0), Quaternion::Identity()}, hum);
    sys.Update(1.0f / 60.0f);
    const CueHandle h = sys.HandleOf(src);
    AETHER_CHECK(h != 0 && world.GetComponent<AudioSource>(src)->playing && sys.ListenerEntity() == ear);
    rig.Render(0.02f);
    VoiceInfo info = rig.Info(sys.Player(), h);
    AETHER_CHECK(Near(info.distance, 4.0f) && Near(info.pan, 1.0f) && Near(info.attenuation, 0.25f));

    // Turning the listener round puts the source on the left; moving it follows, with doppler from the motion.
    world.GetComponent<Transform>(ear)->rotation = Yaw(180.0f);
    sys.Update(0.5f);
    rig.Render(0.02f);
    info = rig.Info(sys.Player(), h);
    AETHER_CHECK(Near(info.pan, -1.0f) && Near(sys.GetListener().forward.z, 1.0f));
    world.GetComponent<Transform>(src)->position = Vec3(0, 0, 6); // now in front of the turned listener
    sys.Update(0.5f);
    rig.Render(0.02f);
    info = rig.Info(sys.Player(), h);
    AETHER_CHECK(Near(info.distance, 6.0f) && Near(info.pan, 0.0f, 1e-3f));
    // Its velocity was (-8, 0, 12) m/s: 12 m/s away along the line to the listener.
    AETHER_CHECK(Near(info.pitch, 343.0f / 355.0f, 1e-4f));
    world.GetComponent<Transform>(ear)->position = Vec3(0, 0, 1);
    sys.Update(0.5f);
    AETHER_CHECK(Near(sys.GetListener().velocity.z, 2.0f) && Near(sys.GetListener().position.z, 1.0f));

    // Blueprint-style commands.
    AudioSource& a = *world.GetComponent<AudioSource>(src);
    a.SetVolume(-6.0f);
    sys.Update(0.01f);
    AETHER_CHECK(sys.HandleOf(src) == h && a.volume_db == -6.0f);
    a.Stop();
    sys.Update(0.01f);
    AETHER_CHECK(sys.HandleOf(src) == 0 && !a.IsPlaying() && sys.Events().empty()); // stopping isn't finishing
    a.FadeIn(0.1f);
    sys.Update(0.01f);
    const CueHandle again = sys.HandleOf(src);
    AETHER_CHECK(again != 0 && a.IsPlaying());
    const auto faded = rig.Render(0.2f);
    AETHER_CHECK(faded[0] < faded[2 * 9000]); // rising over the fade
    a.FadeOut(0.05f);
    sys.Update(0.01f);
    AETHER_CHECK(sys.HandleOf(src) == 0 && rig.mixer.VoiceCount() == 1); // still fading
    rig.Render(0.1f);
    AETHER_CHECK(rig.mixer.VoiceCount() == 0);

    // A one-shot finishing by itself is reported (and its component told).
    AudioSource blip;
    blip.cue = "Blip";
    const Entity shot = world.CreateEntity(Transform{Vec3(), Quaternion::Identity()}, blip);
    sys.Update(0.01f);
    AETHER_CHECK(world.GetComponent<AudioSource>(shot)->playing);
    rig.Render(0.15f);
    sys.Update(0.15f);
    AETHER_CHECK(sys.Events().size() == 1 && sys.Events()[0].entity == shot && sys.Events()[0].cue == "Blip");
    AETHER_CHECK(!world.GetComponent<AudioSource>(shot)->playing);
    sys.Update(0.01f);
    AETHER_CHECK(sys.Events().empty()); // once

    // Switching cues while playing plays the new one; auto_play off waits for Play.
    a.Play();
    sys.Update(0.01f);
    a.SetCue("Blip");
    sys.Update(0.01f);
    AETHER_CHECK(rig.Info(sys.Player(), sys.HandleOf(src)).distance == 0.0f); // Blip isn't spatial
    AudioSource quiet;
    quiet.cue = "Hum";
    quiet.auto_play = false;
    const Entity waiting = world.CreateEntity(Transform{Vec3(), Quaternion::Identity()}, quiet);
    sys.Update(0.01f);
    AETHER_CHECK(sys.HandleOf(waiting) == 0);
    world.GetComponent<AudioSource>(waiting)->Play();
    sys.Update(0.01f);
    AETHER_CHECK(sys.HandleOf(waiting) != 0);

    // Destroying an entity stops its sound.
    const usize before = rig.mixer.VoiceCount();
    world.DestroyEntity(waiting);
    sys.Update(0.01f);
    rig.Render(0.1f);
    AETHER_CHECK(rig.mixer.VoiceCount() < before && sys.HandleOf(waiting) == 0);

    // Missing and broken cues are reported once each.
    AudioSource bad;
    bad.cue = "Nope";
    world.CreateEntity(Transform{Vec3(), Quaternion::Identity()}, bad);
    bad.cue = "Broken";
    world.CreateEntity(Transform{Vec3(), Quaternion::Identity()}, bad);
    for (int i = 0; i < 3; ++i) sys.Update(0.01f);
    AETHER_CHECK(sys.Problems().size() == 2);
    AETHER_CHECK(sys.Problems()[0].find("Nope") != std::string::npos && sys.Problems()[1].find("CU007") != std::string::npos);

    // With the scene hierarchy, sources follow their parents.
    GuidIndex guids;
    World scene;
    AudioSystem nested(scene, rig.mixer, rig.bank, rig.Lookup(), &guids);
    AETHER_CHECK(AudioSystem::Active() == &nested);
    scene.CreateEntity(Transform{Vec3(), Quaternion::Identity()}, AudioListener{});
    const Entity parent = scene.CreateEntity(Transform{Vec3(10, 0, 0), Yaw(90.0f)});
    EnsureGuid(scene, parent, &guids);
    const Entity child = scene.CreateEntity(Transform{Vec3(1, 0, 0), Quaternion::Identity()}, hum);
    EnsureGuid(scene, child, &guids);
    scene.AddComponent(child, Parent{scene.GetComponent<IdComponent>(parent)->guid});
    nested.Update(0.01f);
    rig.Render(0.01f);
    info = rig.Info(nested.Player(), nested.HandleOf(child));
    AETHER_CHECK(Near(info.distance, std::sqrt(101.0f), 1e-3f)); // at (10, 0, -1)
    AETHER_CHECK(Near(WorldPosition(scene, guids, child).z, -1.0f));
}

AETHER_TEST(Audio_LibraryBusFadesAndReverbZones) {
    Rig rig;
    World world;
    {
        auto sys = std::make_unique<AudioSystem>(world, rig.mixer, rig.bank, rig.Lookup());
        // Play Sound 2D ignores a cue's 3D; at a location uses it.
        world.CreateEntity(Transform{Vec3(), Quaternion::Identity()}, AudioListener{});
        sys->Update(0.01f);
        const CueHandle flat = sys->PlaySound2D("Hum", -6.0f);
        const CueHandle placed = sys->PlaySoundAtLocation("Hum", Vec3(0, 0, -3));
        rig.Render(0.01f);
        AETHER_CHECK(rig.Info(sys->Player(), flat).distance == 0.0f && rig.Info(sys->Player(), flat).attenuation == 1.0f);
        AETHER_CHECK(Near(rig.Info(sys->Player(), placed).distance, 3.0f));
        AETHER_CHECK(sys->PlaySound2D("Nope") == 0);

        // Spawn Sound Attached follows its entity (and its offset turns with it), and stops with it.
        const Entity car = world.CreateEntity(Transform{Vec3(5, 0, 0), Yaw(90.0f)});
        const CueHandle engine = sys->SpawnSoundAttached("Hum", car, Vec3(1, 0, 0));
        rig.Render(0.01f);
        AETHER_CHECK(Near(rig.Info(sys->Player(), engine).distance, std::sqrt(26.0f), 1e-3f)); // at (5, 0, -1)
        world.GetComponent<Transform>(car)->position = Vec3(0, 0, 0);
        sys->Update(0.01f);
        rig.Render(0.01f);
        AETHER_CHECK(Near(rig.Info(sys->Player(), engine).distance, 1.0f, 1e-3f));
        world.DestroyEntity(car);
        sys->Update(0.01f);
        rig.Render(0.1f);
        sys->Update(0.01f);
        AETHER_CHECK(!sys->Player().IsPlaying(engine));
        AETHER_CHECK(sys->SpawnSoundAttached("Hum", car) == 0 && !sys->Problems().empty());

        // Bus volume: at once, or faded linearly in dB; unknown buses are refused.
        const BusId music = rig.mixer.FindBus("Music");
        AETHER_CHECK(sys->SetBusVolume("Music", -3.0f) && rig.mixer.BusVolume(music) == -3.0f);
        AETHER_CHECK(sys->SetBusVolume("Music", -13.0f, 1.0f));
        sys->Update(0.5f);
        AETHER_CHECK(Near(rig.mixer.BusVolume(music), -8.0f));
        sys->Update(0.5f);
        AETHER_CHECK(Near(rig.mixer.BusVolume(music), -13.0f));
        sys->Update(0.5f);
        AETHER_CHECK(Near(rig.mixer.BusVolume(music), -13.0f));
        AETHER_CHECK(!sys->SetBusVolume("Nope", 0.0f));

        // The library's static functions act on the active system.
        const usize voices = rig.mixer.VoiceCount();
        Audio::PlaySound2D("Hum", 0.0f, 1.0f);
        Audio::PlaySoundAtLocation("Hum", Vec3(1, 2, 3), 0.0f, 1.0f);
        AETHER_CHECK(rig.mixer.VoiceCount() == voices + 2);
        Audio::SetBusVolume("Music", 0.0f, 0.0f);
        AETHER_CHECK(rig.mixer.BusVolume(music) == 0.0f);
        Audio::StopAllSounds();
        AETHER_CHECK(rig.mixer.VoiceCount() == 0);
    }
    // Without an active system they do nothing.
    AETHER_CHECK(AudioSystem::Active() == nullptr);
    Audio::PlaySound2D("Hum", 0.0f, 1.0f);
    AETHER_CHECK(rig.mixer.VoiceCount() == 0);

    // Reverb zones.
    Mixer mixer(kRate);
    mixer.AddDefaultBuses();
    World w;
    AudioSystem sys(w, mixer, rig.bank, rig.Lookup());
    const Entity ear = w.CreateEntity(Transform{Vec3(), Quaternion::Identity()}, AudioListener{});
    sys.Update(0.01f);
    AETHER_CHECK(!sys.HasReverb()); // no zones: no effect at all
    ReverbZone hall;
    hall.radius = 5.0f, hall.blend_distance = 5.0f, hall.wet = 0.4f, hall.room_size = 0.9f;
    const Entity zone = w.CreateEntity(Transform{Vec3(20, 0, 0), Quaternion::Identity()}, hall);
    sys.Update(0.01f);
    AETHER_CHECK(sys.HasReverb() && sys.ReverbStrength() == 0.0f && sys.ReverbWet() == 0.0f);
    auto tail = [&] {
        // A blip on SFX; what's left 0.1 s after it ends is reverb.
        PlayParams p;
        p.bus = mixer.FindBus("SFX");
        const SoundWave* blip = rig.bank.Find("blip");
        mixer.Play(blip, p);
        std::vector<f32> out(static_cast<usize>(0.3f * kRate) * 2);
        const u32 frames = static_cast<u32>(out.size() / 2);
        for (u32 at = 0; at < frames; at += 256) mixer.Render(out.data() + static_cast<usize>(at) * 2, std::min(256u, frames - at));
        f32 peak = 0.0f;
        for (usize i = static_cast<usize>(0.15f * kRate); i < out.size() / 2; ++i) peak = std::max(peak, std::fabs(out[2 * i]));
        return peak;
    };
    AETHER_CHECK(tail() == 0.0f); // outside: dry
    w.GetComponent<Transform>(ear)->position = Vec3(18, 0, 0);
    sys.Update(0.01f);
    AETHER_CHECK(sys.ReverbStrength() == 1.0f && Near(sys.ReverbWet(), 0.4f) && Near(sys.ReverbRoomSize(), 0.9f));
    AETHER_CHECK(tail() > 1e-3f); // inside: a tail
    w.GetComponent<Transform>(ear)->position = Vec3(27.5f, 0, 0);
    sys.Update(0.01f);
    AETHER_CHECK(Near(sys.ReverbStrength(), 0.5f) && Near(sys.ReverbWet(), 0.2f));
    // A higher-priority zone wins where they overlap, even when weaker.
    ReverbZone cave;
    cave.radius = 1.0f, cave.blend_distance = 10.0f, cave.priority = 1, cave.wet = 0.8f, cave.room_size = 0.3f;
    w.CreateEntity(Transform{Vec3(30, 0, 0), Quaternion::Identity()}, cave);
    sys.Update(0.01f);
    AETHER_CHECK(Near(sys.ReverbRoomSize(), 0.3f) && Near(sys.ReverbStrength(), 0.85f) && Near(sys.ReverbWet(), 0.68f));
    w.DestroyEntity(zone);
    w.GetComponent<Transform>(ear)->position = Vec3(100, 0, 0);
    sys.Update(0.01f);
    AETHER_CHECK(sys.ReverbStrength() == 0.0f && sys.ReverbWet() == 0.0f);
}

AETHER_TEST(Audio_BlueprintNodes) {
    Rig rig;
    RegisterAudioComponents();
    bp::Blueprint blueprint;
    bp::Graph events;
    events.name = "EventGraph";
    blueprint.graphs.push_back(events);
    bp::GraphBuilder b(*blueprint.FindGraph("EventGraph"));
    const bp::NodeId begin = b.Add("Event.BeginPlay");
    const bp::NodeId play2d = b.Add("Call.Native:Audio.PlaySound2D");
    const bp::NodeId attach = b.Add("Call.Native:Audio.SpawnSoundAttached");
    const bp::NodeId self = b.Add("Entity.Self");
    const bp::NodeId bus = b.Add("Call.Native:Audio.SetBusVolume");
    const bp::NodeId fade = b.Add("Call.Native:AudioSource.FadeIn");
    b.Default(play2d, "cue", "Blip").Default(play2d, "volume_db", 0.0).Default(play2d, "pitch", 1.0);
    b.Default(attach, "cue", "Hum").Default(attach, "offset", nlohmann::json::array({0, 2, 0}));
    b.Default(bus, "bus", "SFX").Default(bus, "volume_db", -6.0);
    b.Default(fade, "seconds", 0.25);
    b.Connect(begin, "then", play2d, "exec").Connect(play2d, "then", attach, "exec").Connect(self, "self", attach, "target");
    b.Connect(attach, "then", bus, "exec").Connect(bus, "then", fade, "exec");
    const bp::NodeId finished = b.Add("Event.OnAudioFinished"), print = b.Add("Debug.Print");
    b.Connect(finished, "then", print, "exec").Connect(finished, "cue", print, "text");
    bp::CompileResult compiled = bp::CompileBlueprint(blueprint);
    for (const bp::Diagnostic& d : compiled.diagnostics.diagnostics) std::printf("    %s: %s\n", d.code.c_str(), d.message.c_str());
    AETHER_CHECK(compiled.Ok());

    World world;
    AudioSystem sys(world, rig.mixer, rig.bank, rig.Lookup());
    bp::BlueprintVM vm(world);
    std::vector<std::string> printed;
    vm.SetPrintHandler([&](Entity, const std::string& text) { printed.push_back(text); });
    AudioSource source;
    source.cue = "Blip";
    source.auto_play = false;
    const Entity e = world.CreateEntity(Transform{Vec3(0, 0, -2), Quaternion::Identity()}, source);
    world.CreateEntity(Transform{Vec3(), Quaternion::Identity()}, AudioListener{});
    AETHER_CHECK(vm.Attach(e, compiled.blueprint));
    vm.BeginPlay();
    // Play Sound 2D and Spawn Sound Attached (2 m above the entity) started, the bus changed, and a fade-in queued.
    AETHER_CHECK(rig.mixer.VoiceCount() == 2 && rig.mixer.BusVolume(rig.mixer.FindBus("SFX")) == -6.0f);
    AETHER_CHECK(world.GetComponent<AudioSource>(e)->commands.size() == 1);
    sys.Update(0.01f);
    AETHER_CHECK(sys.HandleOf(e) != 0 && rig.mixer.VoiceCount() == 3);
    for (int frame = 0; frame < 20; ++frame) {
        rig.Render(1.0f / 60.0f);
        sys.Update(1.0f / 60.0f);
        for (const AudioFinishedEvent& ev : sys.Events()) {
            const bp::VmValue args[] = {ev.cue};
            vm.Dispatch(ev.entity, AudioSystem::kFinishedEvent, args);
        }
    }
    AETHER_CHECK(printed == std::vector<std::string>{"Blip"}); // the source's one-shot (Play Sound 2D's isn't an entity's)
    AETHER_CHECK(rig.mixer.VoiceCount() == 1); // the looping attached hum
}
