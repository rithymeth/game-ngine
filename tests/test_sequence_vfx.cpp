#include "test_framework.h"

#include "aether/ecs/world.h"
#include "aether/scene/components.h"
#include "aether/scene/entity_guid.h"
#include "aether/scene/gameplay.h"
#include "aether/sequencer/player.h"
#include "aether/sequencer/sequence.h"
#include "aether/sequencer/sequence_system.h"
#include "aether/vfx/particle_system.h"

#include <cmath>
#include <map>
#include <string>
#include <vector>

// M8.7: Vfx tracks (particle effects started and stopped by a sequence),
// their file format and validation, the player firing them once forward and
// stopping active ones on Stop/Skip, the system's events, and a host that
// drives a vfx::ParticleWorld in sync at fixed frame rates.

using namespace aether;
using namespace aether::seq;

#define CHECK(...) AETHER_CHECK((__VA_ARGS__))

namespace {

bool Near(f32 a, f32 b, f32 eps = 1e-4f) { return std::fabs(a - b) <= eps; }

VfxKey Fx(f32 time, const char* effect, VfxAction action = VfxAction::Play, Vec3 offset = {}) {
    VfxKey k;
    k.time = time;
    k.effect = effect;
    k.action = action;
    k.offset = offset;
    return k;
}

struct Scene {
    World world;
    GuidIndex guids;
    Entity actor;
    EntityGuid actor_guid;
    Scene() {
        (void)GetComponentId<Transform>();
        (void)GetComponentId<Active>();
        RegisterParticleComponents();
        actor = world.CreateEntity(Transform{Vec3(2, 0, 0), Quaternion::Identity()});
        actor_guid = EnsureGuid(world, actor, &guids);
    }
};

// Sparks on the actor from 1 s to 3 s, a world-space flash at 2 s.
LevelSequence Show(const EntityGuid& actor) {
    LevelSequence s;
    s.name = "Show";
    s.duration = 4.0f;
    Track sparks;
    sparks.id = "sparks";
    sparks.type = TrackType::Vfx;
    sparks.binding = actor;
    sparks.vfx = {Fx(1.0f, "Fx/Sparks.avfx", VfxAction::Play, {0, 1, 0}), Fx(3.0f, "Fx/Sparks.avfx", VfxAction::Stop)};
    Track flash;
    flash.id = "flash";
    flash.type = TrackType::Vfx;
    flash.vfx = {Fx(2.0f, "Fx/Flash.avfx", VfxAction::Play, {0, 0, 5})};
    s.tracks = {sparks, flash};
    return s;
}

bool Has(const LevelSequence& s, const char* code) {
    for (const std::string& m : ValidateSequence(s)) {
        if (m.rfind(code, 0) == 0) return true;
    }
    return false;
}

vfx::Emitter Looping(const char* name, f32 rate) {
    vfx::Emitter e;
    e.settings.name = name;
    e.init.push_back(vfx::InitLifetime{true, vfx::FloatRange::Constant(0.1f)});
    e.init.push_back(vfx::InitSize{true, vfx::FloatRange::Constant(0.5f)});
    e.init.push_back(vfx::InitColor{});
    e.render.push_back(vfx::SpriteRenderer{});
    e.spawn.push_back(vfx::SpawnRate{true, rate});
    return e;
}

} // namespace

AETHER_TEST(SequenceVfx_JsonRoundTripAndValidation) {
    Scene scene;
    const LevelSequence s = Show(scene.actor_guid);
    CHECK(ValidateSequence(s).empty()); // the flash track has no entity, and needs none
    const nlohmann::json j = SequenceToJson(s);
    CHECK(j["tracks"][0]["type"] == "vfx" && j["tracks"][0]["vfx"][1]["action"] == "stop");
    LevelSequence back;
    std::string error;
    CHECK(SequenceFromJson(j, back, &error));
    CHECK(back.tracks.size() == 2 && back.tracks[0].type == TrackType::Vfx && back.tracks[0].vfx.size() == 2);
    CHECK(back.tracks[0].binding == scene.actor_guid && back.tracks[1].binding.IsNull());
    CHECK(back.tracks[0].vfx[0].effect == "Fx/Sparks.avfx" && back.tracks[0].vfx[0].action == VfxAction::Play && Near(back.tracks[0].vfx[0].offset.y, 1.0f));
    CHECK(back.tracks[0].vfx[1].action == VfxAction::Stop && Near(back.tracks[1].vfx[0].offset.z, 5.0f));
    CHECK(Near(back.EffectiveDuration(), 4.0f));

    LevelSequence bad = s;
    bad.tracks[0].vfx[0].effect.clear();
    CHECK(Has(bad, "SQ026"));
    bad = s;
    bad.tracks[0].vfx = {Fx(2, "A"), Fx(1, "A")};
    CHECK(Has(bad, "SQ002"));
    bad = s;
    bad.tracks[1].vfx = {Fx(9, "A")};
    CHECK(Has(bad, "SQ003"));
    bad = s;
    bad.tracks[0].type = TrackType::Event; // vfx keys on another kind of track
    CHECK(Has(bad, "SQ013"));

    nlohmann::json broken = j;
    broken["tracks"][0]["vfx"][0]["action"] = "explode";
    LevelSequence untouched = s;
    CHECK(!SequenceFromJson(broken, untouched, &error) && error.find("vfx action") != std::string::npos);
    CHECK(untouched.tracks.size() == 2);
    broken = j;
    broken["tracks"][0]["vfx"][0]["offset"] = {1, 2};
    CHECK(!SequenceFromJson(broken, untouched, &error) && error.find("offset") != std::string::npos);
    broken = j;
    broken["tracks"][0]["vfx"][0].erase("time");
    CHECK(!SequenceFromJson(broken, untouched, &error));
}

AETHER_TEST(SequenceVfx_KeysFireOnceForwardAndStopOrSkipStopsActiveEffects) {
    Scene scene;
    const LevelSequence s = Show(scene.actor_guid);
    SequencePlayer player(s, scene.world, scene.guids);
    std::vector<std::string> log;
    player.on_vfx = [&](const Track& t, Entity e, const VfxKey& k) {
        CHECK(e == (t.id == "sparks" ? scene.actor : kNullEntity));
        log.push_back(k.effect + (k.action == VfxAction::Play ? ":play" : ":stop"));
    };
    player.SetTime(3.5f);
    player.Evaluate();
    CHECK(log.empty()); // scrubbing fires nothing
    player.SetTime(0.0f);
    player.Play();
    player.Update(1.5f);
    CHECK((log == std::vector<std::string>{"Fx/Sparks.avfx:play"}));
    player.Update(1.0f);
    player.Update(1.0f);
    CHECK((log == std::vector<std::string>{"Fx/Sparks.avfx:play", "Fx/Flash.avfx:play", "Fx/Sparks.avfx:stop"}));
    player.SetRate(-1.0f);
    player.Play();
    player.Update(2.0f); // backward: nothing
    CHECK(log.size() == 3);

    // Stop while both effects are alive: each gets one synthetic Stop, with its subject.
    log.clear();
    player.SetRate(1.0f);
    player.Stop();
    CHECK((log == std::vector<std::string>{"Fx/Flash.avfx:stop"})); // sparks were already stopped by their key
    log.clear();
    player.Play();
    player.Update(2.5f);
    CHECK(log.size() == 2);
    log.clear();
    player.Skip();
    // Skip fires no later key (sparks' own Stop at 3 s isn't replayed) and stops both.
    CHECK(log.size() == 2 && log[0] == "Fx/Sparks.avfx:stop" && log[1] == "Fx/Flash.avfx:stop");
    log.clear();
    player.Skip(); // completed: nothing more
    CHECK(log.empty());
}

AETHER_TEST(SequenceVfx_SystemReportsVfxEvents) {
    Scene scene;
    std::map<std::string, LevelSequence> sequences{{"Show.asequence", Show(scene.actor_guid)}};
    SequenceSystem system(scene.world, scene.guids, [&](const std::string& path) -> const LevelSequence* {
        const auto it = sequences.find(path);
        return it == sequences.end() ? nullptr : &it->second;
    });
    SequenceComponent c;
    c.sequence = "Show.asequence";
    c.auto_play = true;
    const Entity e = scene.world.CreateEntity(c);
    system.Update(1.1f);
    const std::vector<SequenceEvent>& events = system.Events();
    CHECK(events.size() == 1);
    CHECK(events[0].kind == SequenceEvent::Kind::Vfx && events[0].entity == e && events[0].name == "Fx/Sparks.avfx" &&
          events[0].payload == "play" && events[0].subject == scene.actor && Near(events[0].offset.y, 1.0f));
    system.Update(1.0f); // 2.1: the flash, in world space
    CHECK(system.Events().size() == 1 && system.Events()[0].subject.IsNull() && system.Events()[0].payload == "play" && Near(system.Events()[0].offset.z, 5.0f));
    scene.world.GetComponent<SequenceComponent>(e)->Skip();
    system.Update(0.01f);
    int stops = 0;
    for (const SequenceEvent& ev : system.Events()) stops += ev.kind == SequenceEvent::Kind::Vfx && ev.payload == "stop";
    CHECK(stops == 2);
}

// A host that plays a sequence's Vfx keys on a ParticleWorld, as a game with
// particles would: Play spawns the effect attached to the subject (or at the
// world offset), Stop deactivates it. At 30, 60 and 144 Hz the sparks start on
// the first frame at or after 1 s, follow the actor at their offset, spawn
// nothing after the frame that crosses 3 s and die out; a skip at 1.5 s stops
// them on the next frame and the flash never starts.
AETHER_TEST(SequenceVfx_ParticleWorldHostStartsAndStopsInSyncAt30_60_144Hz) {
    std::map<std::string, vfx::ParticleSystemAsset> assets;
    assets["Fx/Sparks.avfx"].emitters = {Looping("Sparks", 600.0f)};
    assets["Fx/Flash.avfx"].emitters = {Looping("Flash", 600.0f)};
    const auto lookup = [&](const std::string& name) -> const vfx::ParticleSystemAsset* {
        const auto it = assets.find(name);
        return it == assets.end() ? nullptr : &it->second;
    };
    for (const f32 hz : {30.0f, 60.0f, 144.0f}) {
        for (const bool skip : {false, true}) {
            Scene scene;
            const LevelSequence s = Show(scene.actor_guid);
            vfx::ParticleWorld particles(scene.world, lookup);
            SequencePlayer player(s, scene.world, scene.guids);
            std::map<std::string, Entity> live;
            player.on_vfx = [&](const Track&, Entity subject, const VfxKey& k) {
                if (k.action == VfxAction::Play) {
                    live[k.effect] = subject.IsNull() ? particles.SpawnAtLocation(k.effect, k.offset) : particles.SpawnAttached(k.effect, subject, k.offset);
                } else if (const auto it = live.find(k.effect); it != live.end() && scene.world.IsAlive(it->second)) {
                    if (ParticleSystem* ps = scene.world.GetComponent<ParticleSystem>(it->second)) ps->Deactivate();
                }
            };
            const f32 dt = 1.0f / hz;
            f32 started = -1.0f, last_spawn = -1.0f, flash_started = -1.0f;
            u64 previous = 0;
            bool skipped = false;
            player.Play();
            for (int frame = 1; frame <= static_cast<int>(hz * 4.5f); ++frame) {
                const f32 now = frame * dt;
                if (skip && !skipped && now >= 1.5f) {
                    player.Skip();
                    skipped = true;
                }
                player.Update(dt);
                particles.Update(dt);
                const auto sparks = live.find("Fx/Sparks.avfx");
                if (sparks != live.end()) {
                    const vfx::ParticleSystemInstance* inst = particles.InstanceOf(sparks->second);
                    const usize count = inst ? inst->Count() : 0;
                    const u64 total = inst ? inst->Emitter(0).TotalSpawned() : 0;
                    if (started < 0.0f) {
                        started = now;
                        CHECK(count > 0); // particles on the frame the key fires
                        CHECK(inst && Near(inst->Emitter(0).Particles().position[0].x, 2.0f, 1e-3f) && Near(inst->Emitter(0).Particles().position[0].y, 1.0f, 0.5f));
                    }
                    if (total > previous) last_spawn = now;
                    previous = total;
                }
                if (live.count("Fx/Flash.avfx") && flash_started < 0.0f) flash_started = now;
            }
            CHECK(started >= 1.0f - 1e-4f && started < 1.0f + dt + 1e-4f);
            const auto sparks = live.find("Fx/Sparks.avfx");
            const vfx::ParticleSystemInstance* inst = sparks == live.end() ? nullptr : particles.InstanceOf(sparks->second);
            CHECK(!inst || inst->Count() == 0); // stopped spawning and died out
            if (skip) {
                CHECK(last_spawn < 1.5f + dt + 1e-4f && flash_started < 0.0f);
            } else {
                CHECK(last_spawn < 3.0f + dt + 1e-4f && last_spawn > 2.5f);
                CHECK(flash_started >= 2.0f - 1e-4f && flash_started < 2.0f + dt + 1e-4f);
            }
        }
    }
}
