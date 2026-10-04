#include "test_framework.h"

#include "aether/ecs/world.h"
#include "aether/reflection/registry.h"
#include "aether/scene/components.h"
#include "aether/scene/entity_guid.h"
#include "aether/scene/gameplay.h"
#include "aether/scene/lifecycle.h"
#include "aether/sequencer/player.h"
#include "aether/sequencer/sequence.h"
#include "aether/sequencer/sequence_system.h"

#include <cmath>
#include <map>

// Phase 27 step 2 (§27.2): Event and Visibility tracks, the SequenceComponent
// and the SequenceSystem that runs it.

using namespace aether;
using namespace aether::seq;

#define CHECK(...) AETHER_CHECK((__VA_ARGS__))

namespace {

bool Near(f32 a, f32 b, f32 eps = 1e-4f) { return std::fabs(a - b) <= eps; }

Key K(f32 t, f32 v, Interp i = Interp::Linear) { return Key{t, v, i, 0, 0}; }

Track EventTrack(std::vector<EventKey> keys, const EntityGuid& binding = {}) {
    Track t;
    t.id = "events";
    t.type = TrackType::Event;
    t.binding = binding;
    t.events = std::move(keys);
    return t;
}

Track VisibilityTrack(const EntityGuid& guid, std::vector<Key> keys) {
    Track t;
    t.id = "vis";
    t.type = TrackType::Visibility;
    t.binding = guid;
    t.channels.push_back(Channel{"visible", std::move(keys)});
    return t;
}

Track MoveX(const EntityGuid& guid) {
    Track t;
    t.id = "move";
    t.type = TrackType::Transform;
    t.binding = guid;
    t.channels = {Channel{"x", {K(0, 0), K(2, 10)}}, Channel{"y", {}}, Channel{"z", {}}};
    return t;
}

struct Scene {
    World world;
    GuidIndex guids;
    Entity actor;
    EntityGuid actor_guid;
    Scene() {
        (void)GetComponentId<Transform>();
        (void)GetComponentId<Active>();
        actor = world.CreateEntity(Transform{Vec3(), Quaternion::Identity()});
        actor_guid = EnsureGuid(world, actor, &guids);
    }
};

EventKey E(f32 t, const char* name, const char* payload = "") { return EventKey{t, name, payload}; }

} // namespace

AETHER_TEST(SequenceTracks_EventAndVisibilityJsonRoundTrip) {
    Scene scene;
    LevelSequence s;
    s.name = "Intro";
    s.duration = 4;
    s.tracks = {EventTrack({E(1, "Boom", "loud"), E(3, "Done")}), VisibilityTrack(scene.actor_guid, {K(0, 0, Interp::Constant), K(2, 1, Interp::Constant)})};
    CHECK(ValidateSequence(s).empty());
    LevelSequence back;
    std::string error;
    CHECK(SequenceFromJson(SequenceToJson(s), back, &error));
    CHECK(back.tracks.size() == 2 && back.tracks[0].type == TrackType::Event && back.tracks[1].type == TrackType::Visibility);
    CHECK(back.tracks[0].events.size() == 2 && back.tracks[0].events[0].name == "Boom" && back.tracks[0].events[0].payload == "loud");
    CHECK(back.tracks[0].binding.IsNull() && back.tracks[1].channels[0].keys.size() == 2);
    CHECK(Near(back.EffectiveDuration(), 4.0f));
}

AETHER_TEST(SequenceTracks_ValidationDiagnostics) {
    Scene scene;
    const auto has = [](const LevelSequence& s, const char* code) {
        for (const std::string& m : ValidateSequence(s)) {
            if (m.rfind(code, 0) == 0) return true;
        }
        return false;
    };
    LevelSequence s;
    s.duration = 4;
    s.tracks = {EventTrack({E(1, "")})};
    CHECK(has(s, "SQ011"));
    CHECK(!has(s, "SQ008")); // an Event track needs no entity
    s.tracks = {EventTrack({E(2, "A"), E(1, "B")})};
    CHECK(has(s, "SQ002"));
    s.tracks = {EventTrack({E(9, "A")})};
    CHECK(has(s, "SQ003"));
    s.tracks = {VisibilityTrack(scene.actor_guid, {K(0, 0.5f)})};
    CHECK(has(s, "SQ012"));
    Track no_channel = VisibilityTrack(scene.actor_guid, {});
    no_channel.channels.clear();
    s.tracks = {no_channel};
    CHECK(has(s, "SQ012"));
    Track wrong = MoveX(scene.actor_guid);
    wrong.events = {E(0, "X")};
    s.tracks = {wrong};
    CHECK(has(s, "SQ013"));
    Track wrong_rot = VisibilityTrack(scene.actor_guid, {K(0, 1)});
    wrong_rot.rotation = {RotationKey{}};
    s.tracks = {wrong_rot};
    CHECK(has(s, "SQ013"));
    // A Visibility track still needs its entity.
    s.tracks = {VisibilityTrack(EntityGuid{}, {K(0, 1)})};
    CHECK(has(s, "SQ008"));
}

AETHER_TEST(SequencePlayer_EventsFireOnceWhenCrossed) {
    Scene scene;
    LevelSequence s;
    s.duration = 4;
    s.tracks = {EventTrack({E(0, "Start"), E(1, "One"), E(2, "Two"), E(4, "End")})};
    SequencePlayer player(s, scene.world, scene.guids);
    std::vector<std::string> fired;
    player.on_event = [&](const Track&, const EventKey& e) { fired.push_back(e.name); };
    player.Play();
    player.Update(0.5f);
    CHECK(fired == std::vector<std::string>{"Start"}); // an event at t = 0 fires on the first frame
    player.Update(0.4f);
    CHECK(fired.size() == 1);
    player.Update(0.2f); // 1.1: crossed One
    CHECK(fired.size() == 2 && fired.back() == "One");
    player.Update(3.0f); // past the end: Two and End, then finished
    CHECK((fired == std::vector<std::string>{"Start", "One", "Two", "End"}));
    CHECK(!player.Playing());
}

AETHER_TEST(SequencePlayer_EventsAreNotRefiredByScrubbingOrBackward) {
    Scene scene;
    LevelSequence s;
    s.duration = 4;
    s.tracks = {EventTrack({E(1, "One"), E(3, "Three")})};
    SequencePlayer player(s, scene.world, scene.guids);
    int count = 0;
    player.on_event = [&](const Track&, const EventKey&) { ++count; };
    player.SetTime(3.5f);
    player.Evaluate();
    CHECK(count == 0); // scrubbing and evaluating fire nothing
    player.SetRate(-1.0f);
    player.Play();
    player.Update(2.0f); // backward across One
    CHECK(count == 0 && Near(player.Time(), 1.5f));
    player.SetRate(1.0f);
    player.Update(0.1f); // 1.6: One is behind
    CHECK(count == 0);
    player.Update(2.0f); // crosses Three only
    CHECK(count == 1);
}

AETHER_TEST(SequencePlayer_EventsAcrossALoopWrap) {
    Scene scene;
    LevelSequence s;
    s.duration = 2;
    s.tracks = {EventTrack({E(0, "Head"), E(1.5f, "Tail")})};
    SequencePlayer player(s, scene.world, scene.guids);
    std::vector<std::string> fired;
    player.on_event = [&](const Track&, const EventKey& e) { fired.push_back(e.name); };
    int finished = 0;
    player.on_finished = [&] { ++finished; };
    player.loop = true;
    player.Play();
    player.Update(1.0f); // first frame: Head
    CHECK(fired == std::vector<std::string>{"Head"});
    player.Update(1.2f); // 2.2: Tail, then wraps to 0.2 and Head again
    CHECK((fired == std::vector<std::string>{"Head", "Tail", "Head"}));
    CHECK(finished == 1 && Near(player.Time(), 0.2f) && player.Playing());
}

AETHER_TEST(SequencePlayer_MutedEventTracksAndBoundEvents) {
    Scene scene;
    LevelSequence s;
    s.duration = 2;
    Track muted = EventTrack({E(0.5f, "Quiet")});
    muted.id = "muted";
    muted.mute = true;
    s.tracks = {muted, EventTrack({E(0.5f, "Loud")}, scene.actor_guid)};
    SequencePlayer player(s, scene.world, scene.guids);
    std::vector<std::string> fired;
    player.on_event = [&](const Track& t, const EventKey& e) {
        fired.push_back(e.name);
        CHECK(t.binding == scene.actor_guid);
    };
    player.Play();
    player.Update(1.0f);
    CHECK(fired == std::vector<std::string>{"Loud"});
    CHECK(player.Problems().empty());
}

AETHER_TEST(SequencePlayer_VisibilityTogglesActiveAndCallsTheHookOnlyOnChange) {
    Scene scene;
    LevelSequence s;
    s.duration = 4;
    s.tracks = {VisibilityTrack(scene.actor_guid, {K(0, 0, Interp::Constant), K(2, 1, Interp::Constant), K(3, 0, Interp::Constant)})};
    SequencePlayer player(s, scene.world, scene.guids);
    player.SetTime(0.0f);
    player.Evaluate();
    CHECK(scene.world.GetComponent<Active>(scene.actor) && !scene.world.GetComponent<Active>(scene.actor)->active);
    player.SetTime(2.5f);
    player.Evaluate();
    CHECK(scene.world.GetComponent<Active>(scene.actor)->active);
    player.SetTime(3.5f);
    player.Evaluate();
    CHECK(!scene.world.GetComponent<Active>(scene.actor)->active);

    // With a hook, it's called only when the value changes.
    std::vector<bool> calls;
    player.set_active = [&](Entity e, bool on) {
        CHECK(e == scene.actor);
        calls.push_back(on);
    };
    player.Bind(); // forgets what was applied
    for (f32 t : {0.0f, 0.5f, 1.0f, 2.0f, 2.5f, 3.0f, 3.9f}) {
        player.SetTime(t);
        player.Evaluate();
    }
    CHECK((calls == std::vector<bool>{false, true, false}));
}

namespace {

struct Host {
    Scene scene;
    Lifecycle lifecycle{scene.world, scene.guids};
    std::map<std::string, LevelSequence> sequences;
    SequenceSystem system{scene.world, scene.guids,
                          [this](const std::string& path) -> const LevelSequence* {
                              const auto it = sequences.find(path);
                              return it == sequences.end() ? nullptr : &it->second;
                          },
                          &lifecycle};
    Entity Add(const std::string& path, bool auto_play, bool loop = false, bool destroy = false) {
        SequenceComponent c;
        c.sequence = path;
        c.auto_play = auto_play;
        c.loop = loop;
        c.destroy_when_finished = destroy;
        return scene.world.CreateEntity(c);
    }
};

LevelSequence MoveSequence(const EntityGuid& guid, f32 duration = 2.0f) {
    LevelSequence s;
    s.duration = duration;
    s.tracks = {MoveX(guid)};
    return s;
}

} // namespace

AETHER_TEST(SequenceSystem_AutoPlayStartsAndWritesTimeBack) {
    Host h;
    h.sequences["Move.asequence"] = MoveSequence(h.scene.actor_guid);
    const Entity e = h.Add("Move.asequence", true);
    h.system.Update(1.0f);
    const SequenceComponent* c = h.scene.world.GetComponent<SequenceComponent>(e);
    CHECK(c->playing && Near(c->time, 1.0f));
    CHECK(Near(h.scene.world.GetComponent<Transform>(h.scene.actor)->position.x, 5.0f));
    CHECK(h.system.PlayerOf(e) != nullptr && h.system.Problems().empty());
    h.system.Update(1.5f);
    CHECK(!c->playing && Near(c->time, 2.0f));
    CHECK(Near(h.scene.world.GetComponent<Transform>(h.scene.actor)->position.x, 10.0f));
}

AETHER_TEST(SequenceSystem_CommandsPlayPauseStopSetTime) {
    Host h;
    h.sequences["Move.asequence"] = MoveSequence(h.scene.actor_guid);
    const Entity e = h.Add("Move.asequence", false);
    h.system.Update(1.0f);
    SequenceComponent* c = h.scene.world.GetComponent<SequenceComponent>(e);
    CHECK(!c->playing && Near(c->time, 0.0f)); // not auto-playing: nothing moves
    c->Play();
    h.system.Update(0.5f);
    CHECK(c->playing && Near(c->time, 0.5f));
    c->Pause();
    h.system.Update(1.0f);
    CHECK(!c->playing && Near(c->time, 0.5f));
    c->SetTime(1.5f);
    h.system.Update(0.0f);
    CHECK(Near(c->time, 1.5f) && Near(h.scene.world.GetComponent<Transform>(h.scene.actor)->position.x, 7.5f));
    c->SetRate(2.0f);
    c->Play();
    h.system.Update(0.1f); // 1.5 + 0.2
    CHECK(Near(c->time, 1.7f) && Near(c->rate, 2.0f));
    c->Stop();
    h.system.Update(0.5f);
    CHECK(!c->playing && Near(c->time, 0.0f));
    CHECK(Near(h.scene.world.GetComponent<Transform>(h.scene.actor)->position.x, 0.0f));
    CHECK(c->IsPlaying() == c->playing && Near(c->GetTime(), c->time));
}

AETHER_TEST(SequenceSystem_EventsAndFinishedAreCollected) {
    Host h;
    LevelSequence s;
    s.duration = 2;
    s.tracks = {EventTrack({E(0.5f, "Boom", "big")}, h.scene.actor_guid)};
    h.sequences["Cue.asequence"] = s;
    const Entity e = h.Add("Cue.asequence", true);
    h.system.Update(0.25f);
    CHECK(h.system.Events().empty());
    h.system.Update(0.5f);
    CHECK(h.system.Events().size() == 1);
    const SequenceEvent& m = h.system.Events()[0];
    CHECK(m.kind == SequenceEvent::Kind::Marker && m.entity == e && m.name == "Boom" && m.payload == "big" && m.subject == h.scene.actor);
    h.system.Update(5.0f);
    CHECK(h.system.Events().size() == 1 && h.system.Events()[0].kind == SequenceEvent::Kind::Finished);
    CHECK(h.system.Events()[0].name == "Cue.asequence" && h.system.Events()[0].entity == e);
    h.system.Update(0.1f);
    CHECK(h.system.Events().empty()); // events are for one update
}

AETHER_TEST(SequenceSystem_VisibilityGoesThroughLifecycle) {
    Host h;
    LevelSequence s;
    s.duration = 2;
    s.tracks = {VisibilityTrack(h.scene.actor_guid, {K(0, 1, Interp::Constant), K(1, 0, Interp::Constant)})};
    h.sequences["Hide.asequence"] = s;
    (void)h.Add("Hide.asequence", true);
    h.system.Update(0.5f);
    CHECK(h.scene.world.GetComponent<Active>(h.scene.actor) == nullptr || h.scene.world.GetComponent<Active>(h.scene.actor)->active);
    h.system.Update(1.0f);
    const Active* a = h.scene.world.GetComponent<Active>(h.scene.actor);
    CHECK(a != nullptr && !a->active);
}

AETHER_TEST(SequenceSystem_DestroyWhenFinishedAndSequencerLibrary) {
    Host h;
    h.sequences["Move.asequence"] = MoveSequence(h.scene.actor_guid);
    const Entity e = h.Add("Move.asequence", true, false, true);
    h.system.Update(0.5f);
    CHECK(h.scene.world.IsAlive(e));
    h.system.Update(2.0f);
    CHECK(!h.scene.world.IsAlive(e));
    h.system.Update(0.1f); // the slot is dropped without complaint
    CHECK(h.system.Problems().empty());

    // The Sequencer library makes a self-destroying entity.
    CHECK(SequenceSystem::Active() == &h.system);
    Sequencer::PlaySequence("Move.asequence", false);
    h.system.Update(0.4f);
    CHECK(Near(h.scene.world.GetComponent<Transform>(h.scene.actor)->position.x, 2.0f));
    Sequencer::StopAll();
    h.system.Update(0.1f);
    CHECK(Near(h.scene.world.GetComponent<Transform>(h.scene.actor)->position.x, 0.0f));
    CHECK(h.system.PlaySequence("Missing.asequence") == kNullEntity);
}

AETHER_TEST(SequenceSystem_MissingSequencesAndMissingEntitiesAreReportedOnce) {
    Host h;
    (void)h.Add("Nope.asequence", true);
    LevelSequence s = MoveSequence(EntityGuid{0x1234, 0x5678});
    h.sequences["Orphan.asequence"] = s;
    (void)h.Add("Orphan.asequence", true);
    for (int i = 0; i < 5; ++i) h.system.Update(0.1f);
    CHECK(h.system.Problems().size() == 2);
    CHECK(h.system.Problems()[0].find("Nope.asequence") != std::string::npos);
    CHECK(h.system.Problems()[1].find("0000000000001234") != std::string::npos || h.system.Problems()[1].find("no entity") != std::string::npos);
}

AETHER_TEST(SequenceSystem_RemovedEntityDropsItsPlayer) {
    Host h;
    h.sequences["Move.asequence"] = MoveSequence(h.scene.actor_guid);
    const Entity e = h.Add("Move.asequence", true);
    h.system.Update(0.1f);
    CHECK(h.system.PlayerOf(e) != nullptr);
    h.scene.world.DestroyEntity(e);
    h.system.Update(0.1f);
    CHECK(h.system.PlayerOf(e) == nullptr);
    // A new entity reusing the slot gets a fresh player.
    const Entity again = h.Add("Move.asequence", true);
    h.system.Update(0.1f);
    CHECK(h.system.PlayerOf(again) != nullptr && Near(h.scene.world.GetComponent<SequenceComponent>(again)->time, 0.1f));
}

AETHER_TEST(SequenceSystem_ComponentsAreReflectedForBlueprintsAndScripts) {
    RegisterSequenceComponents();
    const reflect::TypeInfo* type = reflect::TypeRegistry::Find("SequenceComponent");
    CHECK(type != nullptr);
    CHECK(type->FindField("sequence") && type->FindField("auto_play") && type->FindField("loop") && type->FindField("rate"));
    CHECK(type->FindField("time") == nullptr && type->FindField("commands") == nullptr); // not saved
    for (const char* fn : {"Play", "Pause", "Stop", "SetTime", "SetRate", "SetLoop", "IsPlaying", "GetTime"}) {
        bool found = false;
        for (const reflect::FunctionInfo& f : type->functions) found = found || std::string(f.name) == fn;
        CHECK(found);
    }
    const reflect::TypeInfo* library = reflect::TypeRegistry::Find("Sequencer");
    CHECK(library != nullptr && library->functions.size() == 2);
}
