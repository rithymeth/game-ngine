#include "test_framework.h"

#include "aether/ecs/world.h"
#include "aether/scene/components.h"
#include "aether/scene/entity_guid.h"
#include "aether/scene/gameplay.h"
#include "aether/sequencer/movie.h"
#include "aether/sequencer/player.h"
#include "aether/sequencer/sequence.h"
#include "aether/sequencer/sequence_system.h"

#include <cmath>
#include <map>

// Phase 27 step 7 (§27.8): Fade and Subsequence tracks.

using namespace aether;
using namespace aether::seq;

#define CHECK(...) AETHER_CHECK((__VA_ARGS__))

namespace {

bool Near(f32 a, f32 b, f32 eps = 1e-4f) { return std::fabs(a - b) <= eps; }

Key K(f32 t, f32 v) { return Key{t, v, Interp::Linear, 0, 0}; }

struct Scene {
    World world;
    GuidIndex guids;
    Entity actor;
    EntityGuid guid;
    Scene() {
        (void)GetComponentId<Transform>();
        (void)GetComponentId<Camera>();
        actor = world.CreateEntity(Transform{Vec3(), Quaternion::Identity()});
        guid = EnsureGuid(world, actor, &guids);
    }
};

Track FadeTrack(std::vector<Key> keys, Vec4 color = Vec4(0, 0, 0, 1)) {
    Track t;
    t.id = "fade";
    t.type = TrackType::Fade;
    t.channels = {Channel{"amount", std::move(keys)}};
    t.fade_color = color;
    return t;
}

Track SubTrack(std::vector<SubKey> keys) {
    Track t;
    t.id = "subs";
    t.type = TrackType::Subsequence;
    t.subs = std::move(keys);
    return t;
}

SubKey Sub(f32 time, f32 duration, const char* sequence, f32 offset = 0.0f, f32 scale = 1.0f) {
    SubKey k;
    k.time = time;
    k.duration = duration;
    k.sequence = sequence;
    k.offset = offset;
    k.scale = scale;
    return k;
}

Track MoveX(const EntityGuid& guid, f32 to, f32 over) {
    Track t;
    t.id = "move";
    t.type = TrackType::Transform;
    t.binding = guid;
    t.channels = {Channel{"x", {K(0, 0), K(over, to)}}, Channel{"y", {}}, Channel{"z", {}}};
    return t;
}

struct Library {
    std::map<std::string, LevelSequence> sequences;
    SequenceResolver Resolver() {
        return [this](const std::string& path) -> const LevelSequence* {
            const auto it = sequences.find(path);
            return it == sequences.end() ? nullptr : &it->second;
        };
    }
};

bool HasProblem(const SequencePlayer& p, const char* text) {
    for (const std::string& m : p.Problems()) {
        if (m.find(text) != std::string::npos) return true;
    }
    return false;
}

} // namespace

AETHER_TEST(Fade_TrackEvaluatesAmountAndColor) {
    Scene scene;
    LevelSequence s;
    s.duration = 4.0f;
    s.tracks = {FadeTrack({K(0, 0), K(2, 1), K(4, 0)}, Vec4(1, 0, 0, 1))};
    SequencePlayer player(s, scene.world, scene.guids);
    std::vector<f32> seen;
    player.on_fade = [&](const FadeState& f) { seen.push_back(f.amount); };
    player.SetTime(1.0f);
    player.Evaluate();
    CHECK(Near(player.Fade().amount, 0.5f) && Near(player.Fade().color.x, 1.0f) && Near(player.Fade().color.y, 0.0f));
    player.Evaluate(); // unchanged: no second call
    CHECK(seen.size() == 1);
    player.SetTime(2.0f);
    player.Evaluate();
    player.SetTime(4.0f);
    player.Evaluate();
    CHECK(seen.size() == 3 && Near(seen[1], 1.0f) && player.Fade().amount == 0.0f);
    // Stop clears it, and tells the host.
    player.SetTime(2.0f);
    player.Evaluate();
    player.Stop();
    CHECK(player.Fade().amount == 0.0f && seen.back() == 0.0f);
}

AETHER_TEST(Fade_TheStrongestUnmutedTrackWins) {
    Scene scene;
    LevelSequence s;
    s.duration = 2.0f;
    Track weak = FadeTrack({K(0, 0.3f), K(2, 0.3f)}, Vec4(0, 0, 1, 1));
    weak.id = "weak";
    Track strong = FadeTrack({K(0, 0.0f), K(2, 1.0f)}, Vec4(1, 1, 1, 1));
    strong.id = "strong";
    Track muted = FadeTrack({K(0, 1.0f), K(2, 1.0f)}, Vec4(1, 0, 0, 1));
    muted.id = "muted";
    muted.mute = true;
    s.tracks = {weak, strong, muted};
    SequencePlayer player(s, scene.world, scene.guids);
    player.SetTime(0.0f);
    player.Evaluate();
    CHECK(Near(player.Fade().amount, 0.3f) && Near(player.Fade().color.z, 1.0f));
    player.SetTime(2.0f);
    player.Evaluate();
    CHECK(Near(player.Fade().amount, 1.0f) && Near(player.Fade().color.x, 1.0f) && Near(player.Fade().color.y, 1.0f));
}

AETHER_TEST(Fade_JsonRoundTripAndDiagnostics) {
    Scene scene;
    LevelSequence s;
    s.duration = 2.0f;
    s.tracks = {FadeTrack({K(0, 0), K(2, 1)}, Vec4(0.1f, 0.2f, 0.3f, 1.0f)), SubTrack({Sub(0, 1, "Child.asequence", 0.5f, 2.0f)})};
    CHECK(ValidateSequence(s).empty()); // neither track needs an entity
    LevelSequence back;
    std::string error;
    CHECK(SequenceFromJson(SequenceToJson(s), back, &error));
    CHECK(back.tracks.size() == 2 && back.tracks[0].type == TrackType::Fade && Near(back.tracks[0].fade_color.y, 0.2f));
    CHECK(back.tracks[0].channels[0].keys.size() == 2);
    const SubKey& k = back.tracks[1].subs[0];
    CHECK(back.tracks[1].type == TrackType::Subsequence && k.sequence == "Child.asequence" && Near(k.offset, 0.5f) && Near(k.scale, 2.0f) && Near(k.duration, 1.0f));

    const auto has = [](const LevelSequence& seq, const char* code) {
        for (const std::string& m : ValidateSequence(seq)) {
            if (m.rfind(code, 0) == 0) return true;
        }
        return false;
    };
    LevelSequence bad;
    bad.duration = 4.0f;
    Track no_channel = FadeTrack({});
    no_channel.channels.clear();
    bad.tracks = {no_channel};
    CHECK(has(bad, "SQ019"));
    bad.tracks = {FadeTrack({K(0, 1.5f)})};
    CHECK(has(bad, "SQ020"));
    bad.tracks = {SubTrack({Sub(0, 1, "")})};
    CHECK(has(bad, "SQ021"));
    bad.tracks = {SubTrack({Sub(0, 0, "A")})};
    CHECK(has(bad, "SQ022"));
    bad.tracks = {SubTrack({Sub(0, 1, "A", 0, 0)})};
    CHECK(has(bad, "SQ023"));
    bad.tracks = {SubTrack({Sub(2, 1, "A"), Sub(1, 1, "B")})};
    CHECK(has(bad, "SQ002"));
    Track wrong = MoveX(scene.guid, 1, 1);
    wrong.subs = {Sub(0, 1, "A")};
    bad.tracks = {wrong};
    CHECK(has(bad, "SQ013"));
    // A bad fade colour is refused.
    nlohmann::json j = SequenceToJson(s);
    j["tracks"][0]["fade_color"] = {1, 2};
    LevelSequence untouched;
    untouched.name = "keep";
    CHECK(!SequenceFromJson(j, untouched, &error) && untouched.name == "keep");
    // A sequence is as long as its subsequences.
    LevelSequence long_one;
    long_one.tracks = {SubTrack({Sub(1, 3, "A")})};
    CHECK(Near(long_one.EffectiveDuration(), 4.0f));
}

AETHER_TEST(Subsequence_PlaysTheChildAtItsLocalTime) {
    Scene scene;
    Library lib;
    LevelSequence child;
    child.duration = 2.0f;
    child.tracks = {MoveX(scene.guid, 20.0f, 2.0f)}; // 10 units a second
    lib.sequences["Child.asequence"] = child;
    LevelSequence parent;
    parent.duration = 6.0f;
    parent.tracks = {SubTrack({Sub(2, 2, "Child.asequence")})};
    SequencePlayer player(parent, scene.world, scene.guids, lib.Resolver());
    CHECK(player.Problems().empty());
    const auto x = [&] { return scene.world.GetComponent<Transform>(scene.actor)->position.x; };
    player.SetTime(1.0f);
    player.Evaluate();
    CHECK(Near(x(), 0.0f)); // before the range: nothing yet
    player.SetTime(3.0f);
    player.Evaluate();
    CHECK(Near(x(), 10.0f)); // one second into the child
    player.SetTime(2.0f);
    player.Evaluate();
    CHECK(Near(x(), 0.0f));
    player.SetTime(3.5f);
    player.Evaluate();
    player.SetTime(3.0f);
    player.Evaluate();
    CHECK(Near(x(), 10.0f)); // deterministic back and forth
    player.SetTime(5.0f);
    player.Evaluate();
    CHECK(Near(x(), 20.0f)); // past the range: the child's last pose is left behind
    player.SetTime(3.0f);
    player.Evaluate();
    player.SetTime(0.0f);
    player.Evaluate();
    CHECK(Near(x(), 0.0f)); // scrubbing back out of it, to before: its first pose
}

AETHER_TEST(Subsequence_ScaleAndOffset) {
    Scene scene;
    Library lib;
    LevelSequence child;
    child.duration = 10.0f;
    child.tracks = {MoveX(scene.guid, 100.0f, 10.0f)}; // x = child time * 10
    lib.sequences["C"] = child;
    LevelSequence parent;
    parent.duration = 4.0f;
    parent.tracks = {SubTrack({Sub(1, 2, "C", 3.0f, 2.0f)})}; // starts 3 s in, at double speed
    SequencePlayer player(parent, scene.world, scene.guids, lib.Resolver());
    player.SetTime(2.0f); // one second in: child time 3 + 1 * 2 = 5
    player.Evaluate();
    CHECK(Near(scene.world.GetComponent<Transform>(scene.actor)->position.x, 50.0f));
}

AETHER_TEST(Subsequence_EventsAreForwardedOnceAndSpawnsRemovedOnLeaving) {
    Scene scene;
    Library lib;
    LevelSequence child;
    child.duration = 2.0f;
    Track events;
    events.id = "events";
    events.type = TrackType::Event;
    events.events = {EventKey{0.0f, "ChildStart", ""}, EventKey{1.0f, "ChildMid", ""}};
    SpawnKey crate;
    crate.time = 0.0f;
    crate.prefab = "Crate";
    Track spawn;
    spawn.id = "spawn";
    spawn.type = TrackType::Spawn;
    spawn.spawns = {crate};
    child.tracks = {events, spawn};
    lib.sequences["Child"] = child;
    LevelSequence parent;
    parent.duration = 6.0f;
    parent.tracks = {SubTrack({Sub(2, 2, "Child")})};
    SequencePlayer player(parent, scene.world, scene.guids, lib.Resolver());
    std::vector<std::string> fired;
    player.on_event = [&](const Track&, const EventKey& e) { fired.push_back(e.name); };
    int alive = 0, made = 0;
    player.on_spawn = [&](const Track&, Entity, const SpawnKey&) {
        ++alive;
        ++made;
        return scene.world.CreateEntity(Transform{Vec3(), Quaternion::Identity()});
    };
    player.on_despawn = [&](Entity e) {
        --alive;
        scene.world.DestroyEntity(e);
    };
    player.Play();
    for (int i = 0; i < 12; ++i) player.Update(0.25f); // 3 s: inside the range from 2 to 4
    CHECK((fired == std::vector<std::string>{"ChildStart", "ChildMid"}) && alive == 1 && made == 1);
    for (int i = 0; i < 8; ++i) player.Update(0.25f); // to 4 s and past: events don't repeat, the spawn is removed
    CHECK(fired.size() == 2 && alive == 0);
    // Scrubbing into the range spawns again; Stop removes it.
    player.SetTime(3.0f);
    player.Evaluate();
    CHECK(alive == 1 && made == 2);
    player.Stop();
    CHECK(alive == 0);
}

AETHER_TEST(Subsequence_DestroyingThePlayerCleansUpTheChild) {
    Scene scene;
    Library lib;
    scene.world.GetComponent<Transform>(scene.actor);
    const Entity camera = scene.world.CreateEntity(Transform{Vec3(), Quaternion::Identity()}, Camera{});
    const EntityGuid camera_guid = EnsureGuid(scene.world, camera, &scene.guids);
    scene.world.GetComponent<Camera>(camera)->priority = 3;
    LevelSequence child;
    child.duration = 2.0f;
    Track cuts;
    cuts.id = "cuts";
    cuts.type = TrackType::CameraCut;
    cuts.cuts = {CutKey{0.0f, camera_guid}};
    SpawnKey crate;
    crate.prefab = "Crate";
    Track spawn;
    spawn.id = "spawn";
    spawn.type = TrackType::Spawn;
    spawn.spawns = {crate};
    child.tracks = {cuts, spawn, FadeTrack({K(0, 1), K(2, 1)})};
    child.tracks[2].id = "fade";
    lib.sequences["Child"] = child;
    LevelSequence parent;
    parent.duration = 4.0f;
    parent.tracks = {SubTrack({Sub(0, 4, "Child")})};
    int alive = 0;
    {
        SequencePlayer player(parent, scene.world, scene.guids, lib.Resolver());
        player.on_spawn = [&](const Track&, Entity, const SpawnKey&) {
            ++alive;
            return scene.world.CreateEntity(Transform{Vec3(), Quaternion::Identity()});
        };
        player.on_despawn = [&](Entity e) {
            --alive;
            scene.world.DestroyEntity(e);
        };
        player.SetTime(1.0f);
        player.Evaluate();
        CHECK(alive == 1 && scene.world.GetComponent<Camera>(camera)->priority == SequencePlayer::kCutPriority);
        CHECK(Near(player.Fade().amount, 1.0f)); // the child's fade reaches the parent
    }
    CHECK(alive == 0 && scene.world.GetComponent<Camera>(camera)->priority == 3);
}

AETHER_TEST(Subsequence_CyclesDepthAndMissingThingsAreReported) {
    Scene scene;
    Library lib;
    LevelSequence a, b, deep;
    a.duration = b.duration = 2.0f;
    a.tracks = {SubTrack({Sub(0, 2, "A")})}; // plays itself
    lib.sequences["A"] = a;
    SequencePlayer self(a, scene.world, scene.guids, lib.Resolver(), "A");
    CHECK(HasProblem(self, "plays itself"));
    self.SetTime(1.0f);
    self.Evaluate(); // doesn't recurse forever

    LevelSequence x, y;
    x.duration = y.duration = 2.0f;
    x.tracks = {SubTrack({Sub(0, 2, "Y")})};
    y.tracks = {SubTrack({Sub(0, 2, "X")})};
    lib.sequences["X"] = x;
    lib.sequences["Y"] = y;
    SequencePlayer mutual(x, scene.world, scene.guids, lib.Resolver(), "X");
    CHECK(HasProblem(mutual, "plays itself"));
    SequencePlayer anonymous(x, scene.world, scene.guids, lib.Resolver()); // the root's own path unknown: caught one level later
    CHECK(HasProblem(anonymous, "plays itself"));

    // A chain longer than the limit stops at it.
    for (int i = 0; i < 8; ++i) {
        LevelSequence level;
        level.duration = 2.0f;
        level.tracks = {SubTrack({Sub(0, 2, ("L" + std::to_string(i + 1)).c_str())})};
        lib.sequences["L" + std::to_string(i)] = level;
    }
    SequencePlayer chain(lib.sequences["L0"], scene.world, scene.guids, lib.Resolver(), "L0");
    CHECK(HasProblem(chain, "nested too deep"));

    LevelSequence missing;
    missing.duration = 2.0f;
    missing.tracks = {SubTrack({Sub(0, 2, "Nope")})};
    SequencePlayer lost(missing, scene.world, scene.guids, lib.Resolver());
    CHECK(HasProblem(lost, "wasn't found"));
    SequencePlayer none(missing, scene.world, scene.guids); // no resolver
    CHECK(HasProblem(none, "no way to find"));
    none.SetTime(1.0f);
    none.Evaluate(); // plays nothing, harmlessly
}

AETHER_TEST(Subsequence_ProblemsInsideAChildAreSurfaced) {
    Scene scene;
    Library lib;
    LevelSequence child;
    child.duration = 2.0f;
    child.tracks = {MoveX(EntityGuid{0x1, 0x2}, 1.0f, 1.0f)}; // no such entity
    lib.sequences["Child"] = child;
    LevelSequence parent;
    parent.duration = 2.0f;
    parent.tracks = {SubTrack({Sub(0, 2, "Child")})};
    SequencePlayer player(parent, scene.world, scene.guids, lib.Resolver());
    CHECK(HasProblem(player, "Child") && HasProblem(player, "no entity has the GUID"));
}

AETHER_TEST(SequenceSystem_ReportsTheStrongestFadeAndResolvesSubsequences) {
    Scene scene;
    Lifecycle lifecycle(scene.world, scene.guids);
    Library lib;
    LevelSequence child;
    child.duration = 2.0f;
    child.tracks = {MoveX(scene.guid, 20.0f, 2.0f)};
    lib.sequences["Child.asequence"] = child;
    LevelSequence fade;
    fade.duration = 2.0f;
    fade.tracks = {FadeTrack({K(0, 0), K(2, 1)}, Vec4(0, 0, 0, 1))};
    lib.sequences["Fade.asequence"] = fade;
    LevelSequence parent;
    parent.duration = 4.0f;
    parent.tracks = {SubTrack({Sub(0, 2, "Child.asequence")})};
    lib.sequences["Parent.asequence"] = parent;
    SequenceSystem system(scene.world, scene.guids, lib.Resolver(), &lifecycle);
    SequenceComponent a;
    a.sequence = "Parent.asequence";
    a.auto_play = true;
    SequenceComponent b;
    b.sequence = "Fade.asequence";
    b.auto_play = true;
    scene.world.CreateEntity(a);
    scene.world.CreateEntity(b);
    system.Update(1.0f);
    CHECK(system.Problems().empty());
    CHECK(Near(scene.world.GetComponent<Transform>(scene.actor)->position.x, 10.0f)); // the nested child ran
    CHECK(Near(system.Fade().amount, 0.5f));
    system.Update(2.0f);
    CHECK(Near(system.Fade().amount, 1.0f)); // the finished fade holds its last value
}

AETHER_TEST(Movie_RendersSubsequencesAndFades) {
    Scene scene;
    Library lib;
    LevelSequence child;
    child.duration = 1.0f;
    child.tracks = {MoveX(scene.guid, 10.0f, 1.0f)};
    lib.sequences["Child"] = child;
    LevelSequence parent;
    parent.fps = 10.0f;
    parent.duration = 3.0f;
    parent.tracks = {SubTrack({Sub(1, 1, "Child")}), FadeTrack({K(0, 0), K(3, 1)})};
    MovieOptions options;
    options.resolver = lib.Resolver();
    std::vector<f32> xs;
    SequencePlayer* seen = nullptr;
    std::vector<f32> fades;
    const MovieResult r = RenderMovie(
        parent, scene.world, scene.guids, options,
        [&](u32, f32, World& w) {
            xs.push_back(w.GetComponent<Transform>(scene.actor)->position.x);
            if (seen) fades.push_back(seen->Fade().amount);
            return true;
        },
        [&](SequencePlayer& p) { seen = &p; });
    CHECK(r.completed && r.frames == 31 && r.problems.empty());
    CHECK(Near(xs[5], 0.0f) && Near(xs[15], 5.0f, 1e-3f) && Near(xs[19], 9.0f, 1e-3f) && Near(xs[20], 10.0f, 1e-3f) && Near(xs[30], 10.0f, 1e-3f));
    CHECK(Near(fades[0], 0.0f) && Near(fades[15], 0.5f, 1e-3f) && Near(fades[30], 1.0f, 1e-3f));
    // Without a resolver the render reports it.
    const MovieResult no = RenderMovie(parent, scene.world, scene.guids, {}, [](u32, f32, World&) { return true; });
    CHECK(no.completed && !no.problems.empty() && no.problems[0].find("no way to find") != std::string::npos);
}
