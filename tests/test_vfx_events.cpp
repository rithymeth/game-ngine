#include "aether/math/math.h"
#include "aether/vfx/simulation.h"
#include "test_framework.h"

#include <algorithm>
#include <cmath>

// Phase 19 step 3: particle events, EmitAt, sub-emitters (chains, caps,
// probabilities), parameters bound to module fields, the system's checks
// and file format, and collisions with the scene (a raycast and a depth
// buffer).

using namespace aether;
using namespace aether::vfx;

#define CHECK(...) AETHER_CHECK((__VA_ARGS__))

namespace {

bool Near(f32 a, f32 b, f32 tol = 1e-3f) { return std::fabs(a - b) <= tol; }
bool Near(const Vec3& a, const Vec3& b, f32 tol = 1e-3f) { return Near(a.x, b.x, tol) && Near(a.y, b.y, tol) && Near(a.z, b.z, tol); }

Emitter Lived(f32 lifetime, const std::string& name = "Emitter") {
    Emitter e;
    e.settings.name = name;
    e.settings.duration = 1000.0f;
    e.init.push_back(InitLifetime{true, FloatRange::Constant(lifetime)});
    e.init.push_back(InitSize{true, FloatRange::Constant(0.2f)});
    return e;
}

usize CountOf(const std::vector<ParticleEvent>& events, ParticleEventKind kind) {
    return static_cast<usize>(std::count_if(events.begin(), events.end(), [&](const ParticleEvent& e) { return e.kind == kind; }));
}

constexpr u8 kAll = EventBit(ParticleEventKind::Birth) | EventBit(ParticleEventKind::Death) | EventBit(ParticleEventKind::Collision);

} // namespace

AETHER_TEST(VFX_EventsAndEmitAt) {
    // Births and deaths of old age, in world space.
    Emitter e = Lived(0.1f);
    e.settings.space = SimSpace::Local;
    e.spawn.push_back(SpawnBurst{true, 0.0f, FloatRange::Constant(3), 1, 1});
    EmitterInstance burst(e);
    burst.Teleport({{4, 0, 0}, {}});
    burst.RecordEvents(kAll);
    burst.Update(0.05f);
    CHECK(burst.Events().size() == 3 && CountOf(burst.Events(), ParticleEventKind::Birth) == 3 && Near(burst.Events()[0].position, {4, 0, 0}));
    CHECK(Near(burst.Events()[0].size, 0.2f) && burst.Events()[2].id == 2);
    burst.Update(0.1f); // the last frame's events are gone; these are the deaths
    CHECK(burst.Events().size() == 3 && CountOf(burst.Events(), ParticleEventKind::Death) == 3 && burst.Events()[0].expired && Near(burst.Events()[1].position, {4, 0, 0}));
    // Only what's asked for is kept.
    EmitterInstance quiet(e);
    quiet.RecordEvents(EventBit(ParticleEventKind::Death));
    quiet.Update(0.05f);
    CHECK(quiet.Events().empty() && quiet.EventMask() == EventBit(ParticleEventKind::Death));
    // Killed (not expired) deaths, and collisions with their normals.
    Emitter killed = Lived(10.0f);
    killed.init.push_back(InitVelocity{true, VelocityMode::Direction, {0, -1, 0}, 0.0f, FloatRange::Constant(5)});
    killed.update.push_back(CollisionPlane{true, true, {0, 1, 0}, -1.0f, 0.5f, 0.0f, 0.0f, 0.0f});
    killed.update.push_back(KillVolume{true, true, VolumeShape::Sphere, {0, -1, 0}, 0.5f, {}, true});
    EmitterInstance hits(killed);
    hits.RecordEvents(kAll);
    hits.Emit(1);
    hits.Update(0.25f);
    CHECK(CountOf(hits.Events(), ParticleEventKind::Collision) == 1 && CountOf(hits.Events(), ParticleEventKind::Death) == 1);
    const auto hit = std::find_if(hits.Events().begin(), hits.Events().end(), [](const ParticleEvent& x) { return x.kind == ParticleEventKind::Collision; });
    const auto death = std::find_if(hits.Events().begin(), hits.Events().end(), [](const ParticleEvent& x) { return x.kind == ParticleEventKind::Death; });
    CHECK(Near(hit->normal, {0, 1, 0}) && Near(hit->position.y, -1.0f) && !death->expired && hits.Count() == 0);

    // EmitAt: at a world point, with extra velocity and the event's colour and size.
    Emitter spray = Lived(10.0f);
    spray.init.push_back(InitVelocity{true, VelocityMode::Direction, {0, 1, 0}, 0.0f, FloatRange::Constant(1)});
    EmitterInstance at(spray);
    at.Teleport({{100, 0, 0}, {}});
    const LinearColor red{1, 0, 0, 1};
    const f32 big = 3.0f;
    CHECK(at.EmitAt(4, {5, 6, 7}, {2, 0, 0}, &red, &big) == 4);
    CHECK(Near(at.Particles().position[0], {5, 6, 7}) && Near(at.Particles().velocity[0], {2, 1, 0}) && at.Particles().color[3] == red &&
          at.Particles().size[2] == 3.0f && Near(at.Pose().position, {100, 0, 0}));
    at.Update(0.1f); // the emitter's own pose is back: no motion was made up
    CHECK(Near(at.Particles().position[0], {5.2f, 6.1f, 7}));
    // In a local simulation, it's still that world point.
    Emitter carried = spray;
    carried.settings.space = SimSpace::Local;
    carried.init.push_back(InitShape{true, ShapeKind::Point});
    EmitterInstance local(carried);
    local.Teleport({{10, 0, 0}, Quaternion::FromAxisAngle({0, 1, 0}, Radians(90.0f))});
    local.EmitAt(1, {3, 0, 0});
    CHECK(Near(local.Pose().ToWorld(local.Particles().position[0]), {3, 0, 0}));
}

AETHER_TEST(VFX_SubEmittersAndParameters) {
    // A rocket that bursts into sparks when it dies.
    Emitter rocket = Lived(1.0f, "Rocket");
    rocket.settings.looping = false;
    rocket.settings.duration = 1.0f;
    rocket.spawn.push_back(SpawnBurst{true, 0.0f, FloatRange::Constant(1), 1, 1});
    rocket.init.push_back(InitVelocity{true, VelocityMode::Direction, {0, 1, 0}, 0.0f, FloatRange::Constant(10)});
    rocket.init.push_back(InitColor{true, ColorGradient::Constant({1, 0.5f, 0, 1})});
    rocket.update.push_back(Gravity{});
    SubEmitter burst;
    burst.event = ParticleEventKind::Death;
    burst.emitter = "Sparks";
    burst.count = FloatRange::Constant(20);
    burst.inherit_color = true;
    rocket.sub_emitters.push_back(burst);
    Emitter sparks = Lived(0.5f, "Sparks");
    sparks.init.push_back(InitVelocity{true, VelocityMode::Cone, {0, 1, 0}, 3.14f, FloatRange::Constant(5)});
    ParticleSystemAsset firework;
    firework.emitters = {rocket, sparks};
    const auto firework_checks = ValidateParticleSystem(firework);
    CHECK(firework_checks.size() == 3); // two FX008s (nothing draws) and the Sparks' FX001: all warnings
    CHECK(std::none_of(firework_checks.begin(), firework_checks.end(), [](const EmitterDiagnostic& d) { return d.error; }));
    ParticleSystemInstance show(firework, 3);
    for (int i = 0; i < 59; ++i) show.Update(1.0f / 60.0f);
    CHECK(show.Emitter(0).Count() == 1 && show.Emitter(1).Count() == 0 && show.SubEmitterSpawns() == 0);
    const Vec3 top = show.Emitter(0).Particles().position[0];
    for (int i = 0; i < 2; ++i) show.Update(1.0f / 60.0f);
    CHECK(show.Emitter(0).Count() == 0 && show.Emitter(1).Count() == 20 && show.SubEmitterSpawns() == 20);
    CHECK(Near(show.Emitter(1).Particles().position[0], top, 0.4f) && show.Emitter(1).Particles().color[0] == (LinearColor{1, 0.5f, 0, 1}) && !show.Finished());
    for (int i = 0; i < 40; ++i) show.Update(1.0f / 60.0f);
    CHECK(show.Count() == 0 && show.Finished()); // Sparks only spawns from events, so it's done when empty

    // Probability, caps, chains, and loops that stop.
    Emitter many = Lived(0.05f, "Many");
    many.settings.looping = false;
    many.settings.duration = 1.0f;
    many.spawn.push_back(SpawnBurst{true, 0.0f, FloatRange::Constant(50), 1, 1});
    SubEmitter capped;
    capped.emitter = "Child";
    capped.count = FloatRange::Constant(2);
    capped.max_per_frame = 10;
    many.sub_emitters.push_back(capped);
    Emitter child = Lived(10.0f, "Child");
    SubEmitter on_birth;
    on_birth.event = ParticleEventKind::Birth;
    on_birth.emitter = "Grandchild";
    child.sub_emitters.push_back(on_birth);
    Emitter grandchild = Lived(10.0f, "Grandchild");
    ParticleSystemAsset chain;
    chain.emitters = {many, child, grandchild};
    ParticleSystemInstance chained(chain);
    for (int i = 0; i < 6; ++i) chained.Update(0.02f);
    CHECK(chained.Emitter(1).Count() == 20 && chained.Emitter(2).Count() == 20 && chained.SubEmitterSpawns() == 40);
    ParticleSystemAsset never = chain;
    never.emitters[0].sub_emitters[0].probability = 0.0f;
    ParticleSystemInstance none(never);
    for (int i = 0; i < 6; ++i) none.Update(0.02f);
    CHECK(none.SubEmitterSpawns() == 0);
    // A loops back to itself through B: each sub-emitter's cap ends it.
    Emitter a = Lived(10.0f, "A"), b = Lived(10.0f, "B");
    a.spawn.push_back(SpawnBurst{true, 0.0f, FloatRange::Constant(1), 1, 1});
    SubEmitter to_b, to_a;
    to_b.event = to_a.event = ParticleEventKind::Birth;
    to_b.emitter = "B", to_a.emitter = "A";
    to_b.max_per_frame = to_a.max_per_frame = 5;
    a.sub_emitters.push_back(to_b);
    b.sub_emitters.push_back(to_a);
    ParticleSystemAsset loop;
    loop.emitters = {a, b};
    auto loop_checks = ValidateParticleSystem(loop);
    CHECK(std::any_of(loop_checks.begin(), loop_checks.end(), [](const EmitterDiagnostic& d) { return d.code == "FX012" && !d.error; }));
    ParticleSystemInstance looping(loop);
    looping.Update(0.02f);
    CHECK(looping.Emitter(0).Count() == 6 && looping.Emitter(1).Count() == 5);

    // Collisions feed sub-emitters at the hit.
    Emitter faller = Lived(10.0f, "Faller");
    faller.init.push_back(InitVelocity{true, VelocityMode::Direction, {0, -1, 0}, 0.0f, FloatRange::Constant(4)});
    faller.update.push_back(CollisionPlane{true, true, {0, 1, 0}, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f});
    SubEmitter splash;
    splash.event = ParticleEventKind::Collision;
    splash.emitter = "Splash";
    splash.count = FloatRange::Constant(3);
    splash.inherit_size = true;
    faller.sub_emitters.push_back(splash);
    ParticleSystemAsset rain;
    rain.emitters = {faller, Lived(1.0f, "Splash")};
    ParticleSystemInstance drops(rain);
    drops.Teleport({{2, 1, 0}, {}});
    drops.Emitter(0).Emit(1);
    for (int i = 0; i < 20; ++i) drops.Update(0.02f);
    CHECK(drops.Emitter(0).Count() == 0 && drops.Emitter(1).Count() == 3 && Near(drops.Emitter(1).Particles().position[0], {2, 0, 0}, 0.01f) &&
          drops.Emitter(1).Particles().size[0] == 0.2f);
    // Gameplay can listen too.
    drops.RecordEvents(EventBit(ParticleEventKind::Birth));
    CHECK(drops.Emitter(0).EventMask() == (EventBit(ParticleEventKind::Birth) | EventBit(ParticleEventKind::Collision)));

    // Parameters: declared on the system, bound to fields, set at runtime.
    Emitter fountain = Lived(100.0f, "Fountain");
    fountain.spawn.push_back(SpawnRate{true, 1.0f});
    fountain.init.push_back(InitColor{});
    fountain.update.push_back(Gravity{});
    fountain.bindings = {{"Rate", "spawn[0].rate"}, {"Wind", "update[0].acceleration"}, {"Tint", "init[2].color"}, {"Size", "init[1].size"}};
    ParticleSystemAsset park;
    park.emitters = {fountain};
    park.parameters = {{"Rate", ParameterValue::Float(5)}, {"Wind", ParameterValue::Vector({1, 0, 0})}, {"Tint", ParameterValue::Color({0, 1, 0, 1})},
                       {"Size", ParameterValue::Float(0.5f)}};
    CHECK(ValidateParticleSystem(park).size() == 1); // only the FX008 warning
    ParticleSystemInstance water(park);
    for (int i = 0; i < 10; ++i) water.Update(0.1f);
    CHECK(water.Count() == 5 && water.Emitter(0).Particles().color[0] == (LinearColor{0, 1, 0, 1}) && water.Emitter(0).Particles().size[0] == 0.5f);
    CHECK(water.Emitter(0).Particles().velocity[0].x > 0.5f && Near(water.Emitter(0).Particles().velocity[0].y, 0.0f));
    std::string error;
    CHECK(water.SetParameter("Rate", ParameterValue::Float(20)) && water.GetParameter("Rate")->f == 20.0f);
    for (int i = 0; i < 10; ++i) water.Update(0.1f);
    CHECK(water.Count() == 25);
    CHECK(!water.SetParameter("Rate", ParameterValue::Vector({1, 2, 3}), &error) && error.find("different type") != std::string::npos);
    CHECK(!water.SetParameter("Nope", ParameterValue::Float(1), &error) && error.find("no parameter") != std::string::npos && water.GetParameter("Nope") == nullptr);

    // Fields by path, and what they refuse.
    Emitter f = fountain;
    CHECK(SetModuleField(f, "spawn[0].rate", ParameterValue::Float(7)) && std::get<SpawnRate>(f.spawn[0]).rate == 7.0f);
    CHECK(SetModuleField(f, "init[0].seconds", ParameterValue::Float(3)) && std::get<InitLifetime>(f.init[0]).seconds == FloatRange::Constant(3));
    CHECK(!SetModuleField(f, "spawn0.rate", ParameterValue::Float(1), &error) && error.find("stage[index].field") != std::string::npos);
    CHECK(!SetModuleField(f, "spawn[9].rate", ParameterValue::Float(1), &error) && error.find("no module 9") != std::string::npos);
    CHECK(!SetModuleField(f, "update[0].nope", ParameterValue::Float(1), &error) && error.find("Gravity has no field 'nope'") != std::string::npos);
    CHECK(!SetModuleField(f, "spawn[0].rate", ParameterValue::Vector({}), &error) && error.find("takes a number") != std::string::npos);
    CHECK(!SetModuleField(f, "draw[0].x", ParameterValue::Float(1), &error) && error.find("unknown stage") != std::string::npos);
    Emitter shaped = f;
    shaped.init.push_back(InitShape{});
    CHECK(!SetModuleField(shaped, "init[3].shape", ParameterValue::Float(1), &error) && error.find("can't set") != std::string::npos);

    // The system's checks.
    ParticleSystemAsset broken = park;
    broken.emitters.push_back(Lived(1.0f, "Fountain"));                      // FX014
    broken.emitters[0].sub_emitters.push_back({ParticleEventKind::Death, "Nowhere"}); // FX011
    broken.emitters[0].sub_emitters.push_back({ParticleEventKind::Death, "Fountain"}); // FX011: itself (and the other has the same name)
    broken.emitters[0].bindings.push_back({"Ghost", "spawn[0].rate"});       // FX013
    broken.emitters[0].bindings.push_back({"Rate", "update[0].acceleration"}); // FX013: a float into a vector
    const auto checks = ValidateParticleSystem(broken);
    auto count = [&](const char* code) { return std::count_if(checks.begin(), checks.end(), [&](const EmitterDiagnostic& d) { return d.code == code; }); };
    CHECK(count("FX014") == 1 && count("FX011") == 2 && count("FX013") == 2);
    const auto prefixed = std::find_if(checks.begin(), checks.end(), [](const EmitterDiagnostic& d) { return d.code == "FX008"; });
    CHECK(prefixed->message.rfind("emitter 'Fountain': ", 0) == 0);

    // Saved and loaded.
    ParticleSystemAsset saved = park;
    saved.emitters[0].sub_emitters.push_back(splash);
    saved.emitters.push_back(Lived(1.0f, "Splash"));
    const std::string text = SaveParticleSystem(saved);
    ParticleSystemAsset loaded;
    CHECK(LoadParticleSystem(text, loaded, &error) && SaveParticleSystem(loaded) == text);
    CHECK(loaded.parameters.size() == 4 && loaded.parameters[1].value.type == ParameterType::Vector && Near(loaded.parameters[1].value.v, {1, 0, 0}));
    CHECK(loaded.emitters[0].bindings.size() == 4 && loaded.emitters[0].bindings[2].field == "init[2].color" && loaded.FindParameter("Tint") != nullptr);
    CHECK(loaded.emitters[0].sub_emitters.size() == 1 && loaded.emitters[0].sub_emitters[0].event == ParticleEventKind::Collision &&
          loaded.emitters[0].sub_emitters[0].count == FloatRange::Constant(3) && loaded.emitters[0].sub_emitters[0].inherit_size && loaded.FindEmitter("Splash") == 1);
    CHECK(!LoadParticleSystem(R"({"emitters":[{"name":"A","sub_emitters":[{"event":"Explode","emitter":"B"}]}]})", loaded, &error) &&
          error == "emitter 'A': sub_emitters[0]: unknown event 'Explode'");
    CHECK(!LoadParticleSystem(R"({"parameters":[{"name":"P","type":"Matrix"}]})", loaded, &error) && error.find("Matrix") != std::string::npos);
    CHECK(!LoadParticleSystem(R"({"parameters":[{"name":"P","type":"Vector","default":5}]})", loaded, &error) && error.find("parameter 'P': default") != std::string::npos);
    CHECK(!LoadParticleSystem(R"({"emitters":[{"name":"A","bindings":[{"parameter":"P"}]}]})", loaded, &error) && error.find("a binding is") != std::string::npos);
}

AETHER_TEST(VFX_SceneCollision) {
    // Any raycast: here a ground plane at y = 0.
    const FunctionCollider ground([](const Vec3& from, const Vec3& to, Vec3& hit, Vec3& normal) {
        if (from.y < 0.0f || to.y >= 0.0f) return false;
        const f32 t = from.y / (from.y - to.y);
        hit = from + (to - from) * t;
        normal = {0, 1, 0};
        return true;
    });
    Emitter ball = Lived(10.0f);
    ball.update.push_back(Gravity{});
    ball.update.push_back(SceneCollision{true, 0.5f, 0.0f, 0.0f, 0.5f});
    EmitterInstance bouncing(ball);
    bouncing.SetCollider(&ground);
    bouncing.RecordEvents(EventBit(ParticleEventKind::Collision));
    bouncing.Teleport({{0, 2, 0}, {}});
    bouncing.Emit(1);
    f32 lowest = 10.0f;
    usize hits = 0;
    bool up = false;
    for (int i = 0; i < 240; ++i) {
        bouncing.Update(1.0f / 120.0f);
        lowest = std::min(lowest, bouncing.Particles().position[0].y);
        hits += bouncing.Events().size();
        up |= bouncing.Particles().velocity[0].y > 0.0f;
    }
    CHECK(lowest >= 0.1f - 1e-4f && hits >= 1 && up);
    // Without a collider it falls through.
    EmitterInstance falling(ball);
    falling.Teleport({{0, 2, 0}, {}});
    falling.Emit(1);
    for (int i = 0; i < 120; ++i) falling.Update(1.0f / 120.0f);
    CHECK(falling.Particles().position[0].y < -1.0f);

    // A depth buffer: a floor at y = 0 as a camera above and in front saw it.
    const i32 w = 64, h = 48;
    const Mat4 view = Mat4::LookAtRH({0, 5, 10}, {0, 0, 0}, {0, 1, 0});
    const Mat4 proj = Mat4::PerspectiveRH(Radians(60.0f), static_cast<f32>(w) / static_cast<f32>(h), 0.1f, 100.0f);
    const Mat4 vp = proj * view;
    const Mat4 inv = Inverse(vp);
    const Vec4 check = inv * (vp * Vec4(1, 2, 3, 1));
    CHECK(Near(Vec3(check.x / check.w, check.y / check.w, check.z / check.w), {1, 2, 3}, 1e-3f));
    std::vector<f32> depth(static_cast<usize>(w * h), 1.0f);
    for (i32 y = 0; y < h; ++y) {
        for (i32 x = 0; x < w; ++x) {
            const f32 nx = (static_cast<f32>(x) + 0.5f) / static_cast<f32>(w) * 2.0f - 1.0f, ny = 1.0f - (static_cast<f32>(y) + 0.5f) / static_cast<f32>(h) * 2.0f;
            auto unproject = [&](f32 z) {
                const Vec4 p = inv * Vec4(nx, ny, z, 1.0f);
                return Vec3(p.x / p.w, p.y / p.w, p.z / p.w);
            };
            const Vec3 a = unproject(0.0f), b = unproject(1.0f);
            if (a.y <= 0.0f || b.y >= 0.0f) continue;
            const Vec3 p = a + (b - a) * (a.y / (a.y - b.y));
            const Vec4 c = vp * Vec4(p.x, p.y, p.z, 1.0f);
            depth[static_cast<usize>(y * w + x)] = c.z / c.w;
        }
    }
    const DepthBufferCollider screen(vp, w, h, depth, 0.5f);
    Vec3 surface;
    CHECK(screen.SurfaceAt(w / 2, h / 2, surface) && Near(surface.y, 0.0f, 1e-2f) && !screen.SurfaceAt(-1, 0, surface) && !screen.SurfaceAt(w / 2, 0, surface));
    Vec3 at, normal;
    CHECK(screen.Raycast({0, 1, 0}, {0, -0.2f, 0}, at, normal) && Near(at.y, 0.0f, 0.02f) && Near(normal, {0, 1, 0}, 0.05f) && Near(at.x, 0.0f, 0.2f));
    CHECK(!screen.Raycast({0, 1, 0}, {0, 0.5f, 0}, at, normal));    // still above
    CHECK(!screen.Raycast({0, -2, 0}, {0, -3, 0}, at, normal));     // well behind the surface
    CHECK(!screen.Raycast({50, 1, 0}, {50, -1, 0}, at, normal));    // off screen
    EmitterInstance on_screen(ball);
    on_screen.SetCollider(&screen);
    on_screen.Teleport({{0, 1, 0}, {}});
    on_screen.Emit(1);
    lowest = 10.0f;
    for (int i = 0; i < 120; ++i) {
        on_screen.Update(1.0f / 120.0f);
        lowest = std::min(lowest, on_screen.Particles().position[0].y);
    }
    CHECK(lowest > 0.0f && on_screen.Particles().position[0].y > -0.05f);
    // A system hands its collider to every emitter.
    ParticleSystemAsset one;
    one.emitters = {ball};
    ParticleSystemInstance sys(one);
    sys.SetCollider(&ground);
    sys.Teleport({{0, 1, 0}, {}});
    sys.Emitter(0).Emit(1);
    for (int i = 0; i < 120; ++i) sys.Update(1.0f / 120.0f);
    CHECK(sys.Emitter(0).Particles().position[0].y > 0.0f);
}
