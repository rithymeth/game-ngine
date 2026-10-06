#include "test_framework.h"

#include "aether/ecs/world.h"
#include "aether/scene/components.h"
#include "aether/scene/entity_guid.h"
#include "aether/scene/gameplay.h"
#include "aether/sequencer/player.h"
#include "aether/sequencer/sequence.h"
#include "aether/sprite2d/components.h"

#include <cmath>
#include <cstring>
#include <filesystem>

// Level sequences (Phase 27 step 1, §27.1): keyed channels and their
// interpolation, rotation, validation, the .asequence format, and the player
// applying Transform and Property tracks to a world.

using namespace aether;
using namespace aether::seq;

#define CHECK(...) AETHER_CHECK((__VA_ARGS__))

namespace seq_test {
enum class Mood : u8 { Calm, Tense = 3, Angry = 7 };
struct Rig {
    f32 gain = 1.0f;
    f64 precise = 0.0;
    i32 count = 0;
    u16 small = 0;
    bool lit = false;
    Mood mood = Mood::Calm;
    Vec3 offset{0, 0, 0};
    std::string name = "x"; // can't be keyed
};
} // namespace seq_test

AETHER_ENUM(seq_test::Mood, 1, AETHER_ENUM_VALUE(Calm), AETHER_ENUM_VALUE(Tense), AETHER_ENUM_VALUE(Angry))
AETHER_REFLECT(seq_test::Rig, 1, AETHER_FIELD(gain, Field_EditAnywhere), AETHER_FIELD(precise, Field_EditAnywhere),
               AETHER_FIELD(count, Field_EditAnywhere), AETHER_FIELD(small, Field_EditAnywhere), AETHER_FIELD(lit, Field_EditAnywhere),
               AETHER_FIELD(mood, Field_EditAnywhere), AETHER_FIELD(offset, Field_EditAnywhere), AETHER_FIELD(name, Field_EditAnywhere))

namespace {

bool Near(f32 a, f32 b, f32 eps = 1e-4f) { return std::fabs(a - b) <= eps; }

Key K(f32 t, f32 v, Interp i = Interp::Linear, f32 in = 0, f32 out = 0) { return Key{t, v, i, in, out}; }

Channel Ch(const char* name, std::vector<Key> keys) {
    Channel c;
    c.name = name;
    c.keys = std::move(keys);
    return c;
}

struct Scene {
    World world;
    GuidIndex guids;
    EntityGuid guid;
    Entity entity;
    Scene() {
        (void)GetComponentId<Transform>();
        (void)GetComponentId<Camera>();
        (void)GetComponentId<seq_test::Rig>();
        entity = world.CreateEntity();
        world.AddComponent<Transform>(entity, Transform{Vec3(0, 0, 0), Quaternion::Identity()});
        world.AddComponent<Camera>(entity, Camera{});
        world.AddComponent<seq_test::Rig>(entity, seq_test::Rig{});
        guid = EnsureGuid(world, entity, &guids);
    }
};

Track MoveTrack(const EntityGuid& guid) {
    Track t;
    t.id = "move";
    t.name = "Move";
    t.type = TrackType::Transform;
    t.binding = guid;
    t.channels = {Ch("position.x", {K(0, 0), K(2, 10)}), Ch("position.y", {}), Ch("position.z", {K(0, 5, Interp::Constant), K(1, 6)})};
    return t;
}

Track PropertyTrack(const EntityGuid& guid, const char* id, const char* component, const char* field, std::vector<Channel> channels) {
    Track t;
    t.id = id;
    t.name = id;
    t.type = TrackType::Property;
    t.binding = guid;
    t.component = component;
    t.field = field;
    t.channels = std::move(channels);
    return t;
}

} // namespace

AETHER_TEST(Sequence_ChannelEvaluationAndInterpolation) {
    Channel c = Ch("v", {K(1, 10), K(3, 30), K(4, 0, Interp::Constant), K(6, 100)});
    CHECK(c.Evaluate(0.0f) == 10.0f && c.Evaluate(1.0f) == 10.0f);             // before and on the first key
    CHECK(Near(c.Evaluate(2.0f), 20.0f) && Near(c.Evaluate(3.5f), 15.0f));     // linear between
    CHECK(c.Evaluate(4.0f) == 0.0f && c.Evaluate(5.0f) == 0.0f);               // a constant key holds until the next
    CHECK(Near(c.Evaluate(5.99f), 0.0f) && c.Evaluate(6.0f) == 100.0f && c.Evaluate(99.0f) == 100.0f); // and clamps after the last
    CHECK(Channel{}.Evaluate(1.0f) == 0.0f);

    // Bezier: a Hermite curve. Flat tangents ease in and out and stay between the values.
    Channel b = Ch("b", {K(0, 0, Interp::Bezier, 0, 0), K(1, 10, Interp::Bezier, 0, 0)});
    CHECK(Near(b.Evaluate(0.5f), 5.0f));
    f32 previous = -1.0f;
    for (int i = 0; i <= 20; ++i) {
        const f32 v = b.Evaluate(static_cast<f32>(i) / 20.0f);
        CHECK(v >= previous - 1e-5f && v >= 0.0f && v <= 10.0f);
        previous = v;
    }
    CHECK(b.Evaluate(0.1f) < 1.0f); // eased in: slower than the straight line
    // Tangents shape it: a steep out-tangent overshoots less than linear early on.
    Channel steep = Ch("s", {K(0, 0, Interp::Bezier, 0, 20), K(1, 10, Interp::Bezier, 20, 0)});
    CHECK(steep.Evaluate(0.1f) > 1.0f);
    // Normalize sorts.
    Channel unsorted = Ch("u", {K(3, 3), K(1, 1), K(2, 2)});
    unsorted.Normalize();
    CHECK(unsorted.keys[0].time == 1 && unsorted.keys[2].time == 3 && Near(unsorted.Evaluate(1.5f), 1.5f));
}

AETHER_TEST(Sequence_RotationSlerpTakesTheShortWay) {
    const Quaternion a = Quaternion::FromAxisAngle(Vec3(0, 0, 1), 0.0f);
    const Quaternion b = Quaternion::FromAxisAngle(Vec3(0, 0, 1), 3.14159265f / 2.0f); // a quarter turn
    const Quaternion mid = Slerp(a, b, 0.5f);
    const Quaternion eighth = Quaternion::FromAxisAngle(Vec3(0, 0, 1), 3.14159265f / 4.0f);
    CHECK(Near(mid.x, eighth.x) && Near(mid.z, eighth.z) && Near(mid.w, eighth.w) && Near(mid.Length(), 1.0f));
    // The negated quaternion is the same rotation: the blend doesn't spin the long way round.
    const Quaternion neg(-b.x, -b.y, -b.z, -b.w);
    const Quaternion mid2 = Slerp(a, neg, 0.5f);
    CHECK(Near(mid2.z, eighth.z) && Near(mid2.w, eighth.w));
    CHECK(Near(Slerp(a, b, 0.0f).w, a.w) && Near(Slerp(a, b, 1.0f).z, b.z));
    // Nearly equal rotations don't divide by ~0.
    const Quaternion close = Slerp(a, a, 0.3f);
    CHECK(Near(close.w, 1.0f) && !std::isnan(close.x));

    std::vector<RotationKey> keys = {RotationKey{1.0f, a, Interp::Linear}, RotationKey{3.0f, b, Interp::Constant}, RotationKey{4.0f, a, Interp::Linear}};
    CHECK(Near(EvaluateRotation(keys, 0.0f).w, a.w) && Near(EvaluateRotation(keys, 2.0f).w, mid.w));
    CHECK(Near(EvaluateRotation(keys, 3.5f).z, b.z)); // constant holds
    CHECK(Near(EvaluateRotation(keys, 9.0f).w, a.w));
    CHECK(Near(EvaluateRotation({}, 1.0f).w, 1.0f));
}

AETHER_TEST(Sequence_ValidationDiagnostics) {
    Scene scene;
    LevelSequence ok;
    ok.fps = 30;
    ok.duration = 5;
    ok.tracks = {MoveTrack(scene.guid), PropertyTrack(scene.guid, "fov", "Camera", "fov_degrees", {Ch("value", {K(0, 60), K(1, 90)})})};
    ok.tracks[0].rotation = {RotationKey{0, Quaternion::Identity(), Interp::Linear}};
    CHECK(ValidateSequence(ok).empty());

    const auto has = [](const std::vector<std::string>& d, const char* code) {
        for (const std::string& s : d) {
            if (s.rfind(code, 0) == 0) return true;
        }
        return false;
    };
    LevelSequence s;
    CHECK(has(ValidateSequence(s), "SQ001"));
    s = ok;
    s.fps = 0;
    CHECK(has(ValidateSequence(s), "SQ006"));
    s = ok;
    s.duration = -1;
    CHECK(has(ValidateSequence(s), "SQ006"));
    s = ok;
    s.tracks[1].id = "move";
    CHECK(has(ValidateSequence(s), "SQ005"));
    s = ok;
    s.tracks[1].id.clear();
    CHECK(has(ValidateSequence(s), "SQ007"));
    s = ok;
    s.tracks[1].binding = EntityGuid{};
    CHECK(has(ValidateSequence(s), "SQ008"));
    s = ok;
    s.tracks[0].channels.pop_back();
    CHECK(has(ValidateSequence(s), "SQ009"));
    s = ok;
    s.tracks[1].field.clear();
    CHECK(has(ValidateSequence(s), "SQ004"));
    s = ok;
    s.tracks[1].channels.clear();
    CHECK(has(ValidateSequence(s), "SQ010"));
    s = ok;
    s.tracks[1].channels[0].keys = {K(1, 0), K(1, 5)}; // a repeated time
    CHECK(has(ValidateSequence(s), "SQ002"));
    s = ok;
    s.tracks[0].rotation = {RotationKey{2, Quaternion::Identity()}, RotationKey{1, Quaternion::Identity()}};
    CHECK(has(ValidateSequence(s), "SQ002"));
    s = ok;
    s.tracks[1].channels[0].keys = {K(0, 0), K(9, 5)}; // past the 5 s duration
    CHECK(has(ValidateSequence(s), "SQ003"));
    s.duration = 0; // no fixed duration: nothing is "outside"
    CHECK(!has(ValidateSequence(s), "SQ003") && Near(s.EffectiveDuration(), 9.0f));
    CHECK(Near(ok.EffectiveDuration(), 5.0f)); // the duration, unless a key is later
}

AETHER_TEST(Sequence_JsonRoundTripAndRefusals) {
    Scene scene;
    LevelSequence s;
    s.name = "Intro";
    s.fps = 30;
    s.duration = 12;
    Track move = MoveTrack(scene.guid);
    move.channels[0].keys = {K(0, 0, Interp::Bezier, 1, 2), K(2, 10, Interp::Constant), K(4, -3)};
    move.rotation = {RotationKey{0, Quaternion::Identity(), Interp::Linear}, RotationKey{2, Quaternion(0, 0, 0.7071068f, 0.7071068f), Interp::Constant}};
    move.mute = true;
    move.locked = true;
    s.tracks = {move, PropertyTrack(scene.guid, "lit", "Rig", "lit", {Ch("value", {K(0, 0, Interp::Constant), K(1, 1, Interp::Constant)})})};

    const nlohmann::json j = SequenceToJson(s);
    LevelSequence back;
    std::string error;
    CHECK(SequenceFromJson(j, back, &error));
    CHECK(SequenceToJson(back) == j); // lossless
    CHECK(back.name == "Intro" && back.tracks.size() == 2 && back.tracks[0].mute && back.tracks[0].locked);
    CHECK(back.tracks[0].channels[0].keys[0].interp == Interp::Bezier && Near(back.tracks[0].channels[0].keys[0].out_tangent, 2.0f));
    CHECK(back.tracks[0].rotation.size() == 2 && back.tracks[0].rotation[1].interp == Interp::Constant);
    CHECK(back.tracks[1].component == "Rig" && back.tracks[1].binding == scene.guid);

    const std::string file = (std::filesystem::temp_directory_path() / "aether_seq_tests" / "intro.asequence").string();
    CHECK(SaveSequence(file, s, &error));
    LevelSequence loaded;
    CHECK(LoadSequence(file, loaded, &error) && SequenceToJson(loaded) == j);
    CHECK(!LoadSequence(file + ".missing", loaded, &error) && !error.empty());

    // Refusals say why, and leave the output alone.
    LevelSequence untouched;
    untouched.name = "keep";
    const auto refuses = [&](nlohmann::json bad, const char* what) {
        const bool ok = !SequenceFromJson(bad, untouched, &error) && untouched.name == "keep" && error.find(what) != std::string::npos;
        if (!ok) std::printf("    expected '%s', got '%s'\n", what, error.c_str());
        return ok;
    };
    nlohmann::json bad = j;
    bad["$type"] = "Other";
    CHECK(refuses(bad, "not a level sequence"));
    bad = j;
    bad["$version"] = 99;
    CHECK(refuses(bad, "version 99"));
    bad = j;
    bad.erase("tracks");
    CHECK(refuses(bad, "track list"));
    bad = j;
    bad["tracks"][0]["type"] = "spline";
    CHECK(refuses(bad, "unknown type"));
    bad = j;
    bad["tracks"][0]["binding"] = "not a guid";
    CHECK(refuses(bad, "bad entity GUID"));
    bad = j;
    bad["tracks"][0]["channels"][0]["keys"][0]["interp"] = "wobbly";
    CHECK(refuses(bad, "unknown interpolation"));
    bad = j;
    bad["tracks"][0]["channels"][0]["keys"][0].erase("value");
    CHECK(refuses(bad, "needs a time and a value"));
    bad = j;
    bad["tracks"][0]["rotation"][0]["value"] = {1, 2};
    CHECK(refuses(bad, "four-number"));
    CHECK(!SequenceFromJson(nlohmann::json::parse("[1]"), untouched, &error));
}

AETHER_TEST(SequencePlayer_DrivesATransformTrack) {
    Scene scene;
    LevelSequence s;
    s.duration = 2;
    s.tracks = {MoveTrack(scene.guid)};
    s.tracks[0].rotation = {RotationKey{0, Quaternion::Identity(), Interp::Linear},
                            RotationKey{2, Quaternion::FromAxisAngle(Vec3(0, 1, 0), 3.14159265f / 2.0f), Interp::Linear}};
    SequencePlayer player(s, scene.world, scene.guids);
    CHECK(player.Problems().empty() && Near(player.Duration(), 2.0f));
    Transform* t = scene.world.GetComponent<Transform>(scene.entity);
    t->position.y = 42.0f; // the y channel has no keys: left alone

    player.SetTime(0.0f);
    player.Evaluate();
    CHECK(Near(t->position.x, 0.0f) && Near(t->position.z, 5.0f) && Near(t->position.y, 42.0f));
    player.SetTime(1.0f);
    player.Evaluate();
    CHECK(Near(t->position.x, 5.0f) && Near(t->position.z, 6.0f)); // z: constant until t = 1, then 6
    CHECK(Near(t->rotation.y, std::sin(3.14159265f / 8.0f), 1e-3f) && Near(t->rotation.w, std::cos(3.14159265f / 8.0f), 1e-3f)); // halfway round
    player.SetTime(2.0f);
    player.Evaluate();
    CHECK(Near(t->position.x, 10.0f) && Near(t->rotation.y, std::sin(3.14159265f / 4.0f), 1e-3f));
    // Time is clamped to the duration, and evaluation depends only on the time.
    player.SetTime(99.0f);
    CHECK(Near(player.Time(), 2.0f));
    player.SetTime(0.5f);
    player.Evaluate();
    const f32 forward = t->position.x;
    player.SetTime(1.5f);
    player.Evaluate();
    player.SetTime(0.5f);
    player.Evaluate();
    CHECK(t->position.x == forward); // scrubbing there and back lands on the same value
}

AETHER_TEST(SequencePlayer_PropertyTracksWriteReflectedFields) {
    Scene scene;
    LevelSequence s;
    s.duration = 2;
    s.tracks = {PropertyTrack(scene.guid, "fov", "Camera", "fov_degrees", {Ch("v", {K(0, 40), K(2, 100)})}),
                PropertyTrack(scene.guid, "priority", "Camera", "priority", {Ch("v", {K(0, 0), K(2, 10)})}),
                PropertyTrack(scene.guid, "gain", "Rig", "gain", {Ch("v", {K(0, 0), K(2, 1)})}),
                PropertyTrack(scene.guid, "precise", "Rig", "precise", {Ch("v", {K(0, 0), K(2, 4)})}),
                PropertyTrack(scene.guid, "small", "Rig", "small", {Ch("v", {K(0, 0), K(2, 70000)})}),
                PropertyTrack(scene.guid, "lit", "Rig", "lit", {Ch("v", {K(0, 0, Interp::Constant), K(1, 1, Interp::Constant)})}),
                PropertyTrack(scene.guid, "mood", "Rig", "mood", {Ch("v", {K(0, 0, Interp::Constant), K(1, 3, Interp::Constant), K(2, 7)})}),
                PropertyTrack(scene.guid, "offset", "Rig", "offset", {Ch("x", {K(0, 0), K(2, 8)}), Ch("y", {}), Ch("z", {K(0, 1), K(2, 3)})})};
    scene.world.GetComponent<seq_test::Rig>(scene.entity)->offset = Vec3(0, 77, 0);
    SequencePlayer player(s, scene.world, scene.guids);
    CHECK(player.Problems().empty());
    seq_test::Rig* rig = scene.world.GetComponent<seq_test::Rig>(scene.entity);
    Camera* cam = scene.world.GetComponent<Camera>(scene.entity);

    player.SetTime(1.0f);
    player.Evaluate();
    CHECK(Near(cam->fov_degrees, 70.0f) && cam->priority == 5);             // a float and an int (rounded)
    CHECK(Near(rig->gain, 0.5f) && Near(static_cast<f32>(rig->precise), 2.0f)); // f32 and f64
    CHECK(rig->small == 35000 && rig->lit && rig->mood == seq_test::Mood::Tense);
    CHECK(Near(rig->offset.x, 4.0f) && Near(rig->offset.y, 77.0f) && Near(rig->offset.z, 2.0f)); // a Vec3, a member with no keys untouched
    player.SetTime(0.5f);
    player.Evaluate();
    CHECK(!rig->lit && rig->mood == seq_test::Mood::Calm);
    player.SetTime(2.0f);
    player.Evaluate();
    CHECK(rig->small == 65535 && rig->mood == seq_test::Mood::Angry); // an unsigned field clamps to its range
    CHECK(Near(cam->fov_degrees, 100.0f));
}

AETHER_TEST(SequencePlayer_ProblemsAreReportedOnceAndTheRestStillPlay) {
    Scene scene;
    LevelSequence s;
    s.duration = 1;
    Track missing = MoveTrack(NewEntityGuid()); // no entity has this GUID
    missing.id = "missing";
    Track no_component = PropertyTrack(scene.guid, "no_comp", "NoSuchComponent", "x", {Ch("v", {K(0, 1)})});
    Track no_field = PropertyTrack(scene.guid, "no_field", "Camera", "no_such_field", {Ch("v", {K(0, 1)})});
    Track not_keyable = PropertyTrack(scene.guid, "str", "Rig", "name", {Ch("v", {K(0, 1)})});
    Track absent = PropertyTrack(scene.guid, "absent", "SpriteAnimator", "speed", {Ch("v", {K(0, 1)})});
    sprite2d::RegisterSprite2DComponents(); // registered with the ECS, but this entity doesn't have it
    Track fine = PropertyTrack(scene.guid, "fine", "Camera", "fov_degrees", {Ch("v", {K(0, 33), K(1, 66)})});
    s.tracks = {missing, no_component, no_field, not_keyable, absent, fine};
    SequencePlayer player(s, scene.world, scene.guids);
    CHECK(player.Problems().size() == 5);
    const auto mentions = [&](const char* text) {
        for (const std::string& p : player.Problems()) {
            if (p.find(text) != std::string::npos) return true;
        }
        return false;
    };
    CHECK(mentions("Track 'missing'") && mentions("no entity has the GUID") && mentions("no component named 'NoSuchComponent'"));
    CHECK(mentions("no field 'no_such_field'") && mentions("can't be keyed") && mentions("no SpriteAnimator"));
    player.SetTime(0.5f);
    player.Evaluate();
    player.Evaluate();
    CHECK(player.Problems().size() == 5); // not reported again
    CHECK(Near(scene.world.GetComponent<Camera>(scene.entity)->fov_degrees, 49.5f)); // the track that works still applied

    // A muted track is skipped, and a missing entity never crashes.
    LevelSequence muted;
    muted.duration = 1;
    muted.tracks = {PropertyTrack(scene.guid, "fov", "Camera", "fov_degrees", {Ch("v", {K(0, 11)})})};
    muted.tracks[0].mute = true;
    scene.world.GetComponent<Camera>(scene.entity)->fov_degrees = 60.0f;
    SequencePlayer quiet(muted, scene.world, scene.guids);
    quiet.Evaluate();
    CHECK(Near(scene.world.GetComponent<Camera>(scene.entity)->fov_degrees, 60.0f));
}

AETHER_TEST(SequencePlayer_SurvivesADestroyedEntityAndRebinds) {
    Scene scene;
    LevelSequence s;
    s.duration = 1;
    s.tracks = {MoveTrack(scene.guid)};
    SequencePlayer player(s, scene.world, scene.guids);
    player.SetTime(1.0f);
    player.Evaluate();
    CHECK(Near(scene.world.GetComponent<Transform>(scene.entity)->position.x, 5.0f));

    scene.world.DestroyEntity(scene.entity);
    player.Evaluate(); // the entity is gone: no crash, one problem
    player.Evaluate();
    CHECK(player.Problems().size() == 1 && player.Problems()[0].find("no entity has the GUID") != std::string::npos);

    // The entity comes back with the same GUID (a reloaded scene): Bind() finds it.
    const Entity again = scene.world.CreateEntity();
    scene.world.AddComponent<Transform>(again, Transform{Vec3(0, 0, 0), Quaternion::Identity()});
    IdComponent id;
    id.guid = scene.guid;
    scene.world.AddComponent<IdComponent>(again, id);
    scene.guids.Rebuild(scene.world);
    player.Bind();
    CHECK(player.Problems().empty());
    player.Evaluate();
    CHECK(Near(scene.world.GetComponent<Transform>(again)->position.x, 5.0f));
}

AETHER_TEST(SequencePlayer_PlaysLoopsAndFiresOnFinished) {
    Scene scene;
    LevelSequence s;
    s.duration = 2;
    s.tracks = {MoveTrack(scene.guid)};
    SequencePlayer player(s, scene.world, scene.guids);
    int finished = 0;
    player.on_finished = [&] { ++finished; };
    Transform* t = scene.world.GetComponent<Transform>(scene.entity);

    player.Update(1.0f); // not playing: nothing moves
    CHECK(player.Time() == 0.0f && !player.Playing());
    player.Play();
    player.Update(1.0f);
    CHECK(Near(player.Time(), 1.0f) && Near(t->position.x, 5.0f) && finished == 0);
    player.Pause();
    player.Update(1.0f);
    CHECK(Near(player.Time(), 1.0f));
    player.Play();
    player.Update(1.5f); // past the end: stops at the end, once
    CHECK(Near(player.Time(), 2.0f) && !player.Playing() && finished == 1 && Near(t->position.x, 10.0f));
    player.Update(1.0f);
    CHECK(finished == 1); // stopped: it doesn't fire again

    player.Stop();
    CHECK(player.Time() == 0.0f && !player.Playing());
    player.loop = true;
    player.Play();
    player.Update(2.5f); // wraps
    CHECK(player.Playing() && Near(player.Time(), 0.5f) && finished == 2);
    player.Update(1.0f);
    CHECK(Near(player.Time(), 1.5f) && finished == 2);

    // Backward at a negative rate, to the start.
    player.loop = false;
    player.SetTime(1.0f);
    player.SetRate(-1.0f);
    player.Update(0.5f);
    CHECK(Near(player.Time(), 0.5f) && Near(t->position.x, 2.5f));
    player.Update(2.0f);
    CHECK(player.Time() == 0.0f && !player.Playing() && finished == 3);
    // Half speed.
    player.SetRate(0.5f);
    player.Play();
    player.Update(1.0f);
    CHECK(Near(player.Time(), 0.5f) && Near(player.Rate(), 0.5f));
}

AETHER_TEST(SequencePlayer_TwoPlayersAreDeterministic) {
    Scene a, b;
    LevelSequence sa, sb;
    sa.duration = sb.duration = 3;
    sa.tracks = {MoveTrack(a.guid), PropertyTrack(a.guid, "offset", "Rig", "offset", {Ch("x", {K(0, 0, Interp::Bezier, 0, 3), K(3, 9, Interp::Bezier, 1, 0)}), Ch("y", {}), Ch("z", {K(0, 2)})})};
    sb.tracks = {MoveTrack(b.guid), PropertyTrack(b.guid, "offset", "Rig", "offset", {Ch("x", {K(0, 0, Interp::Bezier, 0, 3), K(3, 9, Interp::Bezier, 1, 0)}), Ch("y", {}), Ch("z", {K(0, 2)})})};
    SequencePlayer pa(sa, a.world, a.guids), pb(sb, b.world, b.guids);
    pa.Play();
    pb.Play();
    for (int i = 0; i < 100; ++i) {
        pa.Update(1.0f / 30.0f);
        pb.Update(1.0f / 30.0f);
    }
    const Transform* ta = a.world.GetComponent<Transform>(a.entity);
    const Transform* tb = b.world.GetComponent<Transform>(b.entity);
    CHECK(std::memcmp(&ta->position, &tb->position, sizeof(Vec3)) == 0);
    const auto* ra = a.world.GetComponent<seq_test::Rig>(a.entity);
    const auto* rb = b.world.GetComponent<seq_test::Rig>(b.entity);
    CHECK(std::memcmp(&ra->offset, &rb->offset, sizeof(Vec3)) == 0);
    CHECK(Near(ra->offset.x, 9.0f)); // both reached the end value
}
