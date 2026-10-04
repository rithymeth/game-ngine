#include "test_framework.h"

#include "aether/ecs/world.h"
#include "aether/scene/components.h"
#include "aether/scene/entity_guid.h"
#include "aether/scene/gameplay.h"
#include "aether/sequencer/movie.h"
#include "aether/sequencer/player.h"
#include "aether/sequencer/sequence.h"

#include <cmath>
#include <vector>

// Phase 27 step 6 (§27.7): the CineCamera component and rendering a sequence
// frame by frame.

using namespace aether;
using namespace aether::seq;

#define CHECK(...) AETHER_CHECK((__VA_ARGS__))

namespace {

bool Near(f32 a, f32 b, f32 eps = 1e-3f) { return std::fabs(a - b) <= eps; }

struct Scene {
    World world;
    GuidIndex guids;
    Entity actor, camera;
    EntityGuid actor_guid, camera_guid;
    Scene() {
        (void)GetComponentId<Transform>();
        (void)GetComponentId<Camera>();
        (void)GetComponentId<CineCamera>();
        actor = world.CreateEntity(Transform{Vec3(), Quaternion::Identity()});
        actor_guid = EnsureGuid(world, actor, &guids);
        camera = world.CreateEntity(Transform{Vec3(), Quaternion::Identity()}, Camera{}, CineCamera{});
        camera_guid = EnsureGuid(world, camera, &guids);
    }
};

Track MoveX(const EntityGuid& guid, f32 to, f32 over) {
    Track t;
    t.id = "move";
    t.type = TrackType::Transform;
    t.binding = guid;
    t.channels = {Channel{"x", {Key{0, 0, Interp::Linear, 0, 0}, Key{over, to, Interp::Linear, 0, 0}}}, Channel{"y", {}}, Channel{"z", {}}};
    return t;
}

} // namespace

AETHER_TEST(CineCamera_FovFromTheLens) {
    CineCamera c;
    c.focal_length_mm = 50.0f;
    c.sensor_height_mm = 24.0f;
    CHECK(Near(CineFovDegrees(c), 2.0f * std::atan(24.0f / 100.0f) * 180.0f / 3.14159265f, 1e-2f)); // about 27 degrees
    c.focal_length_mm = 12.0f;
    c.sensor_height_mm = 24.0f;
    CHECK(Near(CineFovDegrees(c), 90.0f, 1e-2f)); // 2 * atan(1)
    c.focal_length_mm = 200.0f;
    CHECK(CineFovDegrees(c) < 8.0f && CineFovDegrees(c) > 1.0f); // telephoto: narrow
    // Bad values stay in range instead of producing NaN or a flipped view.
    c.focal_length_mm = 0.0f;
    CHECK(Near(CineFovDegrees(c), 170.0f));
    c.focal_length_mm = -5.0f;
    CHECK(Near(CineFovDegrees(c), 170.0f));
    c.focal_length_mm = 1000.0f;
    c.sensor_height_mm = 0.0f;
    CHECK(Near(CineFovDegrees(c), 1.0f));
    c.focal_length_mm = 1.0f;
    c.sensor_height_mm = 1000.0f;
    CHECK(Near(CineFovDegrees(c), 170.0f));
}

AETHER_TEST(CineCamera_WritesTheCameraAndLeavesOthersAlone) {
    Scene scene;
    scene.world.GetComponent<Camera>(scene.camera)->projection = Projection::Orthographic;
    scene.world.GetComponent<Camera>(scene.camera)->fov_degrees = 99.0f; // a hand edit: the lens wins
    const Entity plain = scene.world.CreateEntity(Transform{Vec3(), Quaternion::Identity()}, Camera{});
    const Entity lens_only = scene.world.CreateEntity(Transform{Vec3(), Quaternion::Identity()}, CineCamera{});
    CHECK(ApplyCineCameras(scene.world) == 1); // the one entity with both
    const Camera* c = scene.world.GetComponent<Camera>(scene.camera);
    CHECK(c->projection == Projection::Perspective && Near(c->fov_degrees, CineFovDegrees(CineCamera{})));
    CHECK(Near(scene.world.GetComponent<Camera>(plain)->fov_degrees, 60.0f)); // a Camera without a lens is untouched
    CHECK(!scene.world.HasComponent<Camera>(lens_only));
}

AETHER_TEST(CineCamera_APropertyTrackZoomsTheLens) {
    Scene scene;
    LevelSequence s;
    s.duration = 2.0f;
    Track zoom;
    zoom.id = "zoom";
    zoom.type = TrackType::Property;
    zoom.binding = scene.camera_guid;
    zoom.component = "CineCamera";
    zoom.field = "focal_length_mm";
    zoom.channels = {Channel{"value", {Key{0, 24, Interp::Linear, 0, 0}, Key{2, 120, Interp::Linear, 0, 0}}}};
    s.tracks = {zoom};
    SequencePlayer player(s, scene.world, scene.guids);
    CHECK(player.Problems().empty());
    player.SetTime(0.0f);
    player.Evaluate();
    ApplyCineCameras(scene.world);
    const f32 wide = scene.world.GetComponent<Camera>(scene.camera)->fov_degrees;
    player.SetTime(2.0f);
    player.Evaluate();
    ApplyCineCameras(scene.world);
    const f32 tele = scene.world.GetComponent<Camera>(scene.camera)->fov_degrees;
    CHECK(Near(scene.world.GetComponent<CineCamera>(scene.camera)->focal_length_mm, 120.0f) && wide > 40.0f && tele < 10.0f);
}

AETHER_TEST(CineCamera_IsReflectedForScenesAndTracks) {
    (void)GetComponentId<CineCamera>();
    const reflect::TypeInfo* type = reflect::TypeRegistry::Find("CineCamera");
    CHECK(type != nullptr);
    for (const char* f : {"focal_length_mm", "sensor_width_mm", "sensor_height_mm", "aperture_f", "focus_distance"}) CHECK(type->FindField(f) != nullptr);
    CHECK(FindComponentIdByName("CineCamera") != kInvalidComponentId);
}

AETHER_TEST(Movie_FrameCountAndExactTimes) {
    CHECK(MovieFrameCount(2.0f, 24.0f) == 49 && MovieFrameCount(0.0f, 24.0f) == 1 && MovieFrameCount(1.0f, 30.0f) == 31);
    CHECK(MovieFrameCount(1.0f, 0.0f) == 0 && MovieFrameCount(-1.0f, 24.0f) == 0);
    CHECK(MovieFrameCount(0.1f, 10.0f) == 2); // 0.1 * 10 is a hair under 1 in floating point: still two frames
    Scene scene;
    LevelSequence s;
    s.fps = 24.0f;
    s.duration = 2.0f;
    s.tracks = {MoveX(scene.actor_guid, 48.0f, 2.0f)};
    std::vector<f32> times, xs;
    const MovieResult r = RenderMovie(s, scene.world, scene.guids, {}, [&](u32 frame, f32 t, World& w) {
        CHECK(frame == times.size());
        times.push_back(t);
        xs.push_back(w.GetComponent<Transform>(scene.actor)->position.x);
        return true;
    });
    CHECK(r.completed && r.frames == 49 && r.total_frames == 49 && r.problems.empty());
    CHECK(Near(times.back(), 2.0f, 1e-6f) && Near(times[24], 1.0f, 1e-6f));
    for (usize i = 0; i < xs.size(); ++i) CHECK(Near(xs[i], static_cast<f32>(i), 1e-3f)); // 48 units over 48 frames
}

AETHER_TEST(Movie_IsDeterministicAndHonoursTheRange) {
    Scene scene;
    LevelSequence s;
    s.fps = 10.0f;
    s.duration = 1.0f;
    s.tracks = {MoveX(scene.actor_guid, 10.0f, 1.0f)};
    const auto run = [&](MovieOptions o) {
        std::vector<f32> xs;
        RenderMovie(s, scene.world, scene.guids, o, [&](u32, f32, World& w) {
            xs.push_back(w.GetComponent<Transform>(scene.actor)->position.x);
            return true;
        });
        return xs;
    };
    const std::vector<f32> a = run({}), b = run({});
    CHECK(a.size() == 11 && a == b);
    MovieOptions range;
    range.start_frame = 3;
    range.end_frame = 5;
    const std::vector<f32> part = run(range);
    CHECK(part.size() == 3 && Near(part[0], 3.0f) && Near(part[2], 5.0f));
    MovieOptions half_rate;
    half_rate.fps = 5.0f; // a different frame rate samples the same curve
    const std::vector<f32> coarse = run(half_rate);
    CHECK(coarse.size() == 6 && Near(coarse[1], 2.0f) && Near(coarse[5], 10.0f));
}

AETHER_TEST(Movie_SinkCanStopItAndBadOptionsAreReported) {
    Scene scene;
    LevelSequence s;
    s.fps = 10.0f;
    s.duration = 1.0f;
    s.tracks = {MoveX(scene.actor_guid, 10.0f, 1.0f)};
    int seen = 0;
    MovieResult r = RenderMovie(s, scene.world, scene.guids, {}, [&](u32, f32, World&) { return ++seen < 4; });
    CHECK(!r.completed && r.frames == 4 && seen == 4);
    // Stopping on the very last frame still counts as complete.
    r = RenderMovie(s, scene.world, scene.guids, {}, [&](u32 f, f32, World&) { return f != 10; });
    CHECK(r.completed && r.frames == 11);

    MovieOptions bad;
    bad.fps = -1.0f;
    r = RenderMovie(s, scene.world, scene.guids, bad, [](u32, f32, World&) { return true; });
    CHECK(!r.completed && r.frames == 0 && !r.problems.empty() && r.problems[0].find("frame rate") != std::string::npos);
    LevelSequence empty;
    r = RenderMovie(empty, scene.world, scene.guids, {}, [](u32, f32, World&) { return true; });
    CHECK(!r.completed && !r.problems.empty());
    MovieOptions past;
    past.start_frame = 99;
    r = RenderMovie(s, scene.world, scene.guids, past, [](u32, f32, World&) { return true; });
    CHECK(!r.completed && r.frames == 0 && r.problems[0].find("start frame") != std::string::npos);
    // A track with no entity is reported, and the rest still renders.
    s.tracks.push_back(MoveX(EntityGuid{0xDEAD, 0xBEEF}, 1.0f, 1.0f));
    s.tracks.back().id = "orphan";
    r = RenderMovie(s, scene.world, scene.guids, {}, [](u32, f32, World&) { return true; });
    CHECK(r.completed && r.frames == 11 && r.problems.size() == 1 && r.problems[0].find("orphan") != std::string::npos);
}

AETHER_TEST(Movie_EventsFireOnceEachAcrossFrames) {
    Scene scene;
    LevelSequence s;
    s.fps = 10.0f;
    s.duration = 1.0f;
    Track events;
    events.id = "events";
    events.type = TrackType::Event;
    events.events = {EventKey{0.0f, "Start", ""}, EventKey{0.3f, "Third", ""}, EventKey{0.55f, "Between", ""}, EventKey{1.0f, "End", ""}};
    s.tracks = {events};
    std::vector<std::pair<u32, std::string>> fired;
    u32 current = 0;
    const MovieResult r = RenderMovie(
        s, scene.world, scene.guids, {},
        [&](u32 frame, f32, World&) {
            current = frame;
            return true;
        },
        [&](SequencePlayer& p) { p.on_event = [&](const Track&, const EventKey& e) { fired.push_back({current, e.name}); }; });
    CHECK(r.completed);
    // `current` is the previous frame while a frame's events fire (they fire before its sink call).
    CHECK(fired.size() == 4 && fired[0].second == "Start" && fired[1].second == "Third" && fired[2].second == "Between" && fired[3].second == "End");
    // Starting later skips the earlier events.
    fired.clear();
    MovieOptions from;
    from.start_frame = 5;
    RenderMovie(s, scene.world, scene.guids, from, [](u32, f32, World&) { return true; },
                [&](SequencePlayer& p) { p.on_event = [&](const Track&, const EventKey& e) { fired.push_back({0, e.name}); }; });
    CHECK(fired.size() == 2 && fired[0].second == "Between" && fired[1].second == "End");
    // fire_events = false silences them.
    fired.clear();
    MovieOptions quiet;
    quiet.fire_events = false;
    RenderMovie(s, scene.world, scene.guids, quiet, [](u32, f32, World&) { return true; },
                [&](SequencePlayer& p) { p.on_event = [&](const Track&, const EventKey& e) { fired.push_back({0, e.name}); }; });
    CHECK(fired.empty());
}

AETHER_TEST(Movie_CameraCutsAndLensAreInForceOnTheRightFrame) {
    Scene scene;
    const Entity second = scene.world.CreateEntity(Transform{Vec3(), Quaternion::Identity()}, Camera{});
    const EntityGuid second_guid = EnsureGuid(scene.world, second, &scene.guids);
    scene.world.GetComponent<Camera>(scene.camera)->priority = 1;
    LevelSequence s;
    s.fps = 10.0f;
    s.duration = 1.0f;
    Track cuts;
    cuts.id = "cuts";
    cuts.type = TrackType::CameraCut;
    cuts.cuts = {CutKey{0.5f, second_guid}};
    Track zoom;
    zoom.id = "zoom";
    zoom.type = TrackType::Property;
    zoom.binding = scene.camera_guid;
    zoom.component = "CineCamera";
    zoom.field = "focal_length_mm";
    zoom.channels = {Channel{"value", {Key{0, 20, Interp::Linear, 0, 0}, Key{1, 100, Interp::Linear, 0, 0}}}};
    s.tracks = {cuts, zoom};
    std::vector<Entity> active;
    std::vector<f32> fov;
    RenderMovie(s, scene.world, scene.guids, {}, [&](u32, f32, World& w) {
        active.push_back(FindActiveCamera(w, scene.guids));
        fov.push_back(w.GetComponent<Camera>(scene.camera)->fov_degrees);
        return true;
    });
    CHECK(active.size() == 11 && active[0] == scene.camera && active[4] == scene.camera);
    CHECK(active[5] == second && active[10] == second); // the cut at 0.5 s is in force on frame 5
    CHECK(fov[0] > fov[10] + 20.0f); // the lens zoomed in
    // The render's player is gone: the cut is undone.
    CHECK(FindActiveCamera(scene.world, scene.guids) == scene.camera);
    CHECK(scene.world.GetComponent<Camera>(second)->priority == 0);
}
