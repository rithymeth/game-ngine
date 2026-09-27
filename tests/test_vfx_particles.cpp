#include "aether/math/math.h"
#include "aether/vfx/simulation.h"
#include "test_framework.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

// Phase 19 step 1: curves, gradients, randomness and noise; emitter assets
// (modules, JSON, checks); and the CPU simulation (spawning, shapes,
// velocities, forces, collisions, kill volumes, over-life curves, spaces,
// determinism).

using namespace aether;
using namespace aether::vfx;

#define CHECK(...) AETHER_CHECK((__VA_ARGS__))

namespace {

bool Near(f32 a, f32 b, f32 tol = 1e-3f) { return std::fabs(a - b) <= tol; }
bool Near(const Vec3& a, const Vec3& b, f32 tol = 1e-3f) { return Near(a.x, b.x, tol) && Near(a.y, b.y, tol) && Near(a.z, b.z, tol); }

// An emitter that lives long and doesn't move particles unless told to.
Emitter Basic(f32 lifetime = 100.0f) {
    Emitter e;
    e.settings.duration = 1000.0f;
    e.init.push_back(InitLifetime{true, FloatRange::Constant(lifetime)});
    e.init.push_back(InitSize{true, FloatRange::Constant(0.1f)});
    return e;
}

void Run(EmitterInstance& e, f32 seconds, f32 dt = 1.0f / 60.0f) {
    const long steps = std::lround(seconds / dt);
    for (long i = 0; i < steps; ++i) e.Update(dt);
}

bool Same(const Vec3& a, const Vec3& b) { return a.x == b.x && a.y == b.y && a.z == b.z; }

} // namespace

AETHER_TEST(VFX_CurvesRandomAndNoise) {
    // Curves: linear, held, smooth through their keys, clamped outside.
    const FloatCurve line = FloatCurve::Line(0.0f, 2.0f);
    CHECK(Near(line.Evaluate(0.25f), 0.5f) && line.Evaluate(-1.0f) == 0.0f && line.Evaluate(5.0f) == 2.0f && FloatCurve{}.Evaluate(0.3f) == 0.0f);
    FloatCurve steps{{{0.0f, 1.0f}, {0.5f, 3.0f}, {1.0f, 0.0f}}, CurveInterp::Constant};
    CHECK(steps.Evaluate(0.49f) == 1.0f && steps.Evaluate(0.5f) == 3.0f && steps.Evaluate(0.99f) == 3.0f && steps.Sorted());
    FloatCurve smooth = steps;
    smooth.interp = CurveInterp::Smooth;
    CHECK(Near(smooth.Evaluate(0.5f), 3.0f) && Near(smooth.Evaluate(0.0f), 1.0f) && smooth.Evaluate(0.25f) > 1.0f && smooth.Evaluate(0.25f) < 3.2f);
    CHECK(std::fabs(smooth.Evaluate(0.01f) - 1.0f) < 0.02f); // flat at the ends
    CHECK(!FloatCurve{{{1.0f, 0.0f}, {0.0f, 1.0f}}, CurveInterp::Linear}.Sorted());
    // Gradients: colour and alpha keys apart.
    const ColorGradient fade = ColorGradient::Fade({1, 1, 1, 1}, {0, 0, 0, 0});
    const LinearColor mid = fade.Evaluate(0.5f);
    CHECK(Near(mid.r, 0.5f) && Near(mid.a, 0.5f) && fade.Evaluate(2.0f).r == 0.0f);
    ColorGradient split{{{0.0f, 1, 0, 0}, {1.0f, 0, 0, 1}}, {{0.0f, 1.0f}, {0.2f, 0.0f}}};
    CHECK(Near(split.Evaluate(0.1f).a, 0.5f) && Near(split.Evaluate(0.1f).r, 0.9f) && split.Evaluate(0.5f).a == 0.0f && ColorGradient{}.Evaluate(0.5f) == LinearColor{});

    // Random: repeatable per seed, uniform enough, shapes of directions.
    Random a(42), b(42), c(43);
    bool same = true, differ = false;
    for (int i = 0; i < 100; ++i) {
        const u32 x = a.NextU32(), y = b.NextU32();
        same &= x == y;
        differ |= x != c.NextU32();
    }
    CHECK(same && differ);
    f32 mean = 0.0f, lo = 1.0f, hi = 0.0f, radius = 0.0f;
    bool cone_ok = true, sphere_ok = true;
    for (int i = 0; i < 10000; ++i) {
        const f32 u = a.Next01();
        mean += u, lo = std::min(lo, u), hi = std::max(hi, u);
        cone_ok &= a.InCone(0.3f).y >= std::cos(0.3f) - 1e-5f;
        sphere_ok &= Near(a.OnUnitSphere().Length(), 1.0f);
        radius += a.InUnitSphere().Length();
    }
    CHECK(Near(mean / 10000.0f, 0.5f, 0.02f) && lo >= 0.0f && hi < 1.0f && cone_ok && sphere_ok && Near(radius / 10000.0f, 0.75f, 0.02f));
    CHECK(a.Range(FloatRange::Constant(3.0f)) == 3.0f);

    // Noise is smooth and bounded; its curl has no divergence.
    f32 biggest = 0.0f, jump = 0.0f, divergence = 0.0f, magnitude = 0.0f;
    for (int i = 0; i < 200; ++i) {
        const Vec3 p(static_cast<f32>(i) * 0.137f, static_cast<f32>(i % 7) * 0.3f, static_cast<f32>(i % 13) * 0.21f);
        biggest = std::max(biggest, std::fabs(Noise3(p)));
        jump = std::max(jump, std::fabs(Noise3(p) - Noise3(p + Vec3(0.001f, 0, 0))));
        constexpr f32 h = 1e-2f;
        const Vec3 cx0 = CurlNoise(p - Vec3(h, 0, 0)), cx1 = CurlNoise(p + Vec3(h, 0, 0));
        const Vec3 cy0 = CurlNoise(p - Vec3(0, h, 0)), cy1 = CurlNoise(p + Vec3(0, h, 0));
        const Vec3 cz0 = CurlNoise(p - Vec3(0, 0, h)), cz1 = CurlNoise(p + Vec3(0, 0, h));
        divergence += std::fabs((cx1.x - cx0.x + cy1.y - cy0.y + cz1.z - cz0.z) / (2 * h));
        magnitude += CurlNoise(p).Length();
    }
    CHECK(biggest < 1.2f && biggest > 0.2f && jump < 0.01f && divergence / 200.0f < 0.05f * magnitude / 200.0f + 0.05f && magnitude > 10.0f);

    // JSON.
    FloatRange r;
    FloatCurve fc;
    ColorGradient g;
    LinearColor col;
    Vec3 v;
    std::string error;
    CHECK(FromJson(ToJson(FloatRange{1, 2}), r) && r == FloatRange{1, 2} && ToJson(FloatRange::Constant(4)) == 4.0f);
    CHECK(FromJson(ToJson(smooth), fc) && fc == smooth && ToJson(FloatCurve::Constant(2)) == 2.0f && FromJson(nlohmann::json(2.0f), fc) && fc.Evaluate(0.7f) == 2.0f);
    CHECK(FromJson(ToJson(split), g) && g == split && FromJson(nlohmann::json({1, 0, 0}), g) && g.Evaluate(0.3f) == LinearColor{1, 0, 0, 1});
    CHECK(FromJson(nlohmann::json({1, 2, 3}), v) && Near(v, {1, 2, 3}) && FromJson(nlohmann::json({0.5, 0.5, 0.5}), col) && col.a == 1.0f);
    CHECK(!FromJson(nlohmann::json("x"), r, &error) && error.find("range") != std::string::npos);
    CHECK(!FromJson(nlohmann::json::parse(R"({"keys":[[0,1]],"interp":"Wobbly"})"), fc, &error) && error.find("Wobbly") != std::string::npos);
    CHECK(!FromJson(nlohmann::json::parse(R"({"keys":[[0]]})"), fc, &error) && !FromJson(nlohmann::json({1, 2}), v, &error));
    CHECK(FromJson(nlohmann::json::parse(R"({"keys":[[1,0],[0,1]]})"), fc) && fc.Sorted() && fc.keys[0].time == 0.0f); // sorted on load
}

AETHER_TEST(VFX_EmitterAssets) {
    // Every module, saved and loaded back to the same text.
    Emitter e;
    e.settings.name = "Sparks";
    e.settings.space = SimSpace::Local;
    e.settings.warmup = 0.5f;
    e.spawn = {SpawnRate{true, 50}, SpawnBurst{true, 0.2f, {5, 10}, 0, 0.5f}, SpawnPerDistance{false, 3}};
    InitShape shape;
    shape.shape = ShapeKind::Box;
    shape.thickness = 0.0f;
    InitVelocity velocity;
    velocity.mode = VelocityMode::Radial;
    e.init = {InitLifetime{}, shape, velocity, InitSize{}, InitColor{true, ColorGradient::Fade({1, 0, 0, 1}, {1, 1, 0, 1})}, InitRotation{true, {0, 6.28f}, {-1, 1}},
              InheritVelocity{true, 0.5f}};
    KillVolume box;
    box.shape = VolumeShape::Box;
    box.kill_inside = false;
    e.update = {Gravity{}, Drag{true, 0.3f}, CurlNoiseForce{}, Vortex{}, PointAttractor{}, box, CollisionPlane{}, SizeOverLife{true, FloatCurve::Line(1, 0)},
                ColorOverLife{true, ColorGradient::Fade({1, 1, 1, 1}, {1, 1, 1, 0})}, SpeedOverLife{}};
    ParticleSystemAsset asset;
    asset.name = "Fire";
    asset.emitters = {e, Basic()};
    const std::string text = SaveParticleSystem(asset);
    ParticleSystemAsset loaded;
    std::string error;
    CHECK(LoadParticleSystem(text, loaded, &error) && SaveParticleSystem(loaded) == text && loaded.name == "Fire" && loaded.emitters.size() == 2);
    const Emitter& le = loaded.emitters[0];
    CHECK(le.settings.space == SimSpace::Local && le.settings.warmup == 0.5f && le.spawn.size() == 3 && le.init.size() == 7 && le.update.size() == 10);
    CHECK(!std::get<SpawnPerDistance>(le.spawn[2]).enabled && std::get<InitShape>(le.init[1]).shape == ShapeKind::Box && !std::get<KillVolume>(le.update[5]).kill_inside);
    CHECK(std::get<SpawnBurst>(le.spawn[1]).count == FloatRange{5, 10} && std::get<SizeOverLife>(le.update[7]).curve == FloatCurve::Line(1, 0));
    // Names and the add menu.
    CHECK(std::string(ModuleName(le.update[3])) == "Vortex" && SpawnModuleNames().size() == 3 && InitModuleNames().size() == 7 && UpdateModuleNames().size() == 10);
    UpdateModule made;
    CHECK(MakeModule("Drag", made) && std::holds_alternative<Drag>(made) && !MakeModule("SpawnRate", made));

    // Errors say where.
    CHECK(!LoadParticleSystem("{nope", loaded, &error) && error.find("malformed") != std::string::npos);
    CHECK(!LoadParticleSystem(R"({"version":9})", loaded, &error) && error.find("newer") != std::string::npos);
    CHECK(!LoadParticleSystem(R"({"emitters":[{"name":"A","update":[{"module":"Gravity"},{"module":"Vortex","center":[1,2]}]}]})", loaded, &error) &&
          error == "emitter 'A': update[1] (Vortex): center: a vector is [x, y, z]");
    CHECK(!LoadParticleSystem(R"({"emitters":[{"name":"A","spawn":[{"module":"Explode"}]}]})", loaded, &error) && error.find("unknown module 'Explode'") != std::string::npos);
    CHECK(!LoadParticleSystem(R"({"emitters":[{"name":"A","init":[{"module":"InitShape","shape":"Donut"}]}]})", loaded, &error) && error.find("Donut") != std::string::npos);
    CHECK(!LoadParticleSystem(R"({"emitters":[{"name":"A","space":"Galaxy"}]})", loaded, &error) && error.find("Galaxy") != std::string::npos);
    CHECK(!LoadParticleSystem(R"({"emitters":[{"name":"A","spawn":[{"module":"SpawnBurst","cycles":-2}]}]})", loaded, &error) && error.find("cycles") != std::string::npos);
    CHECK(!LoadParticleSystem(R"({"emitters":[{"name":"A","max_particles":"lots"}]})", loaded, &error) && error.find("wrong type") != std::string::npos);

    // Checks.
    auto has = [](const std::vector<EmitterDiagnostic>& d, const char* code) {
        return std::any_of(d.begin(), d.end(), [&](const EmitterDiagnostic& x) { return x.code == code; });
    };
    Emitter ok = Basic();
    ok.spawn.push_back(SpawnRate{});
    CHECK(ValidateEmitter(ok).empty() && has(ValidateEmitter(Basic()), "FX001") && !ValidateEmitter(Basic())[0].error);
    Emitter bad = ok;
    bad.settings.max_particles = 0;
    bad.settings.duration = 0.0f;
    bad.init.push_back(InitLifetime{true, {0.0f, 1.0f}});
    bad.init.push_back(InitSize{true, {2.0f, 1.0f}});
    bad.spawn.push_back(SpawnBurst{true, 0.0f, {5, 5}, 1, 1});
    bad.update.push_back(SizeOverLife{true, FloatCurve{{{1, 0}, {0, 1}}, CurveInterp::Linear}});
    const auto d = ValidateEmitter(bad);
    for (const char* code : {"FX002", "FX003", "FX004", "FX005", "FX006", "FX007"}) CHECK(has(d, code));
    const auto backwards = std::find_if(d.begin(), d.end(), [](const EmitterDiagnostic& x) { return x.code == "FX004"; });
    CHECK(backwards->message == "init[3] (InitSize): size has its min over its max");
}

AETHER_TEST(VFX_Simulation) {
    // Rates spread births across the frame; each carries its part-particles.
    Emitter e = Basic();
    e.spawn.push_back(SpawnRate{true, 30.0f});
    EmitterInstance rate(e, 7);
    rate.Update(0.1f);
    const ParticleBuffer& p = rate.Particles();
    CHECK(rate.Count() == 3 && Near(*std::max_element(p.age.begin(), p.age.begin() + 3), 2.0f / 30.0f) && Near(*std::min_element(p.age.begin(), p.age.begin() + 3), 0.0f));
    for (int i = 0; i < 9; ++i) rate.Update(0.1f);
    CHECK(rate.Count() == 30 && rate.TotalSpawned() == 30 && Near(rate.Time(), 1.0f));
    Emitter two = e;
    two.spawn.push_back(SpawnRate{true, 5.0f}); // rates add
    EmitterInstance both(two, 7);
    Run(both, 2.0f, 0.05f);
    CHECK(both.Count() == 70);

    // Deaths at the end of life keep a steady count.
    Emitter short_lived = Basic(0.5f);
    short_lived.spawn.push_back(SpawnRate{true, 10.0f});
    EmitterInstance steady(short_lived);
    Run(steady, 3.0f, 0.01f);
    CHECK(steady.Count() >= 4 && steady.Count() <= 6 && steady.TotalSpawned() == 30);

    // Bursts: at their time, in cycles, once per loop.
    Emitter burst = Basic(0.2f);
    burst.settings.duration = 2.0f;
    burst.settings.looping = false;
    burst.spawn.push_back(SpawnBurst{true, 0.5f, FloatRange::Constant(20), 3, 0.25f});
    EmitterInstance bursting(burst);
    Run(bursting, 0.4f, 0.1f);
    CHECK(bursting.TotalSpawned() == 0);
    Run(bursting, 0.2f, 0.1f);
    CHECK(bursting.TotalSpawned() == 20);
    Run(bursting, 0.5f, 0.1f);
    CHECK(bursting.TotalSpawned() == 60 && bursting.Spawning());
    Run(bursting, 1.0f, 0.1f);
    CHECK(bursting.TotalSpawned() == 60 && !bursting.Spawning() && bursting.Finished());
    Emitter looped = Basic();
    looped.settings.duration = 1.0f;
    looped.spawn.push_back(SpawnBurst{true, 0.0f, FloatRange::Constant(5), 1, 1});
    EmitterInstance loops(looped);
    Run(loops, 3.05f, 0.05f);
    CHECK(loops.TotalSpawned() == 20 && loops.Loop() == 3);
    // Forever cycles, a start delay, and the particle cap.
    Emitter cycling = Basic();
    cycling.settings.duration = 1.0f;
    cycling.settings.start_delay = 0.5f;
    cycling.settings.max_particles = 25;
    cycling.spawn.push_back(SpawnBurst{true, 0.0f, FloatRange::Constant(2), 0, 0.1f});
    EmitterInstance cyc(cycling);
    Run(cyc, 0.45f, 0.05f);
    CHECK(cyc.TotalSpawned() == 0);
    Run(cyc, 0.5f, 0.05f); // 0.45 s into the loop: bursts at 0, 0.1, ... 0.4
    CHECK(cyc.TotalSpawned() == 10);
    Run(cyc, 1.0f, 0.05f);
    CHECK(cyc.Count() == 25);
    CHECK(cyc.Emit(10) == 0); // full

    // The same seed and steps give the same particles; Restart replays them.
    Emitter lively = Basic(2.0f);
    lively.spawn.push_back(SpawnRate{true, 100.0f});
    lively.init.push_back(InitShape{true, ShapeKind::Sphere, 1.0f, {}, 1.0f});
    lively.init.push_back(InitVelocity{});
    lively.update.push_back(Gravity{});
    lively.update.push_back(CurlNoiseForce{true, 2.0f, 0.5f, 1.0f});
    EmitterInstance first(lively, 11), second(lively, 11), other(lively, 12);
    Run(first, 1.0f), Run(second, 1.0f), Run(other, 1.0f);
    bool identical = first.Count() == second.Count(), different = false;
    for (usize i = 0; identical && i < first.Count(); ++i) identical &= Same(first.Particles().position[i], second.Particles().position[i]);
    for (usize i = 0; i < std::min(first.Count(), other.Count()); ++i) different |= !Same(first.Particles().position[i], other.Particles().position[i]);
    CHECK(identical && different);
    const Vec3 was = first.Particles().position[10];
    first.Restart();
    CHECK(first.Count() == 0 && first.Time() == 0.0f);
    Run(first, 1.0f);
    CHECK(Same(first.Particles().position[10], was));

    // Gravity and drag.
    Emitter falling = Basic();
    falling.update.push_back(Gravity{});
    EmitterInstance fall(falling);
    fall.Emit(1);
    Run(fall, 1.0f, 1.0f / 120.0f);
    CHECK(Near(fall.Particles().position[0].y, -0.5f * 9.81f, 0.05f) && Near(fall.Particles().velocity[0].y, -9.81f, 0.01f));
    Emitter dragged = Basic();
    dragged.init.push_back(InitVelocity{true, VelocityMode::Direction, {1, 0, 0}, 0.0f, FloatRange::Constant(10)});
    dragged.update.push_back(Drag{true, 2.0f});
    EmitterInstance drag(dragged);
    drag.Emit(1);
    Run(drag, 1.0f, 0.01f);
    CHECK(Near(drag.Particles().velocity[0].x, 10.0f * std::exp(-2.0f), 0.01f) && Near(drag.Particles().position[0].x, 5.0f * (1.0f - std::exp(-2.0f)), 0.05f));

    // Shapes.
    auto shaped = [&](const InitShape& shape, usize count = 500) {
        Emitter s = Basic();
        s.init.push_back(shape);
        auto inst = std::make_unique<EmitterInstance>(s, 3);
        inst->Emit(count);
        return inst;
    };
    auto all = [](const EmitterInstance& inst, auto pred) {
        for (usize i = 0; i < inst.Count(); ++i) {
            if (!pred(inst.Particles().position[i])) return false;
        }
        return true;
    };
    CHECK(all(*shaped({true, ShapeKind::Sphere, 2.0f, {}, 0.0f}), [](const Vec3& q) { return Near(q.Length(), 2.0f); }));
    CHECK(all(*shaped({true, ShapeKind::Sphere, 2.0f, {}, 1.0f}), [](const Vec3& q) { return q.Length() <= 2.0f + 1e-4f; }));
    CHECK(all(*shaped({true, ShapeKind::Hemisphere, 1.0f, {}, 1.0f}), [](const Vec3& q) { return q.y >= 0.0f; }));
    CHECK(all(*shaped({true, ShapeKind::Box, 0.0f, {1, 2, 3}, 1.0f}), [](const Vec3& q) { return std::fabs(q.x) <= 1 && std::fabs(q.y) <= 2 && std::fabs(q.z) <= 3; }));
    CHECK(all(*shaped({true, ShapeKind::Box, 0.0f, {1, 2, 3}, 0.0f}), [](const Vec3& q) {
        return Near(std::fabs(q.x), 1) || Near(std::fabs(q.y), 2) || Near(std::fabs(q.z), 3);
    }));
    CHECK(all(*shaped({true, ShapeKind::Circle, 1.0f, {}, 0.0f}), [](const Vec3& q) { return q.y == 0.0f && Near(q.Length(), 1.0f); }));
    CHECK(all(*shaped({true, ShapeKind::Edge, 3.0f, {}, 1.0f}), [](const Vec3& q) { return q.y == 0.0f && q.z == 0.0f && std::fabs(q.x) <= 3.0f; }));

    // Velocities are in the emitter's frame: turned 90 degrees about Z, +Y sprays towards -X.
    Emitter spray = Basic();
    spray.init.push_back(InitVelocity{true, VelocityMode::Cone, {0, 1, 0}, 0.3f, FloatRange::Constant(2)});
    EmitterInstance turned(spray);
    turned.Teleport({{5, 0, 0}, Quaternion::FromAxisAngle({0, 0, 1}, Radians(90.0f))});
    turned.Emit(200);
    bool towards = true;
    for (usize i = 0; i < turned.Count(); ++i) {
        const Vec3 vel = turned.Particles().velocity[i];
        towards &= vel.x < -2.0f * std::cos(0.3f) + 1e-3f && Near(vel.Length(), 2.0f) && Near(turned.Particles().position[i], {5, 0, 0});
    }
    CHECK(towards);
    Emitter radial = Basic();
    radial.init.push_back(InitShape{true, ShapeKind::Sphere, 1.0f, {}, 0.0f});
    radial.init.push_back(InitVelocity{true, VelocityMode::Radial, {}, 0.0f, FloatRange::Constant(1)});
    EmitterInstance out(radial);
    out.Emit(100);
    bool outwards = true;
    for (usize i = 0; i < out.Count(); ++i) outwards &= Near(out.Particles().velocity[i], out.Particles().position[i]);
    CHECK(outwards);

    // World particles stay behind a moving emitter; local ones come along.
    Emitter still = Basic();
    Emitter carried = Basic();
    carried.settings.space = SimSpace::Local;
    EmitterInstance world_space(still), local_space(carried);
    world_space.Emit(1), local_space.Emit(1);
    world_space.SetPose({{10, 0, 0}, {}});
    local_space.SetPose({{10, 0, 0}, {}});
    world_space.Update(0.1f), local_space.Update(0.1f);
    CHECK(Near(world_space.Particles().position[0], {0, 0, 0}) && Near(local_space.Particles().position[0], {0, 0, 0}) &&
          Near(local_space.Pose().ToWorld(local_space.Particles().position[0]), {10, 0, 0}));

    // Per distance: a trail along the path; inherited velocity.
    Emitter trail = Basic();
    trail.spawn.push_back(SpawnPerDistance{true, 2.0f});
    trail.init.push_back(InheritVelocity{true, 1.0f});
    EmitterInstance trailing(trail);
    trailing.Teleport({{0, 0, 0}, {}});
    trailing.SetPose({{10, 0, 0}, {}});
    trailing.Update(1.0f);
    f32 min_x = 100.0f, max_x = -100.0f;
    bool inherited = true;
    for (usize i = 0; i < trailing.Count(); ++i) {
        const ParticleBuffer& t = trailing.Particles();
        // Born along the way and moved on at the emitter's speed since: they end where the emitter is.
        min_x = std::min(min_x, t.position[i].x), max_x = std::max(max_x, t.position[i].x);
        inherited &= Near(t.velocity[i].x, 10.0f);
    }
    CHECK(trailing.Count() == 20 && inherited && Near(min_x, 10.0f, 0.01f) && Near(max_x, 10.0f, 0.01f));
    trailing.Update(1.0f); // standing still: none
    CHECK(trailing.Count() == 20);

    // A vortex spins particles about its axis; an attractor pulls and kills near it.
    Emitter spun = Basic();
    spun.init.push_back(InitShape{true, ShapeKind::Circle, 1.0f, {}, 0.0f});
    spun.update.push_back(Vortex{true, false, {}, {0, 1, 0}, 3.0f, 0.0f});
    EmitterInstance vortex(spun);
    vortex.Emit(50);
    vortex.Update(0.1f);
    bool tangential = true;
    for (usize i = 0; i < vortex.Count(); ++i) {
        const Vec3 q = vortex.Particles().position[i], vel = vortex.Particles().velocity[i];
        tangential &= std::fabs(vel.Dot(Vec3(q.x, 0, q.z).Normalized())) < 0.05f && Near(vel.Length(), 0.3f, 0.01f) && vel.y == 0.0f;
    }
    CHECK(tangential);
    Emitter pulled = Basic();
    pulled.init.push_back(InitShape{true, ShapeKind::Sphere, 3.0f, {}, 0.0f});
    pulled.update.push_back(PointAttractor{true, false, {}, 20.0f, 0.0f, 0.5f});
    EmitterInstance attract(pulled);
    attract.Emit(100);
    attract.Update(0.05f);
    bool inwards = true;
    for (usize i = 0; i < attract.Count(); ++i) inwards &= attract.Particles().velocity[i].Dot(attract.Particles().position[i]) < 0.0f;
    CHECK(inwards && attract.Count() == 100);
    Run(attract, 2.0f, 0.01f);
    CHECK(attract.Count() < 10);

    // Kill volumes: outside a sphere, inside a box.
    Emitter leaving = Basic();
    leaving.init.push_back(InitVelocity{true, VelocityMode::Cone, {0, 1, 0}, 3.14f, {1, 5}});
    leaving.update.push_back(KillVolume{true, false, VolumeShape::Sphere, {}, 2.0f, {}, false});
    EmitterInstance escape(leaving);
    escape.Emit(100);
    Run(escape, 0.3f, 0.01f);
    const usize left_now = escape.Count();
    Run(escape, 2.2f, 0.01f); // even the slowest (1 unit/s) is out by now
    bool within = true;
    for (usize i = 0; i < escape.Count(); ++i) within &= escape.Particles().position[i].Length() <= 2.0f;
    CHECK(left_now > 0 && escape.Count() == 0 && within);
    Emitter boxed = Basic();
    boxed.init.push_back(InitShape{true, ShapeKind::Edge, 4.0f, {}, 1.0f});
    boxed.update.push_back(KillVolume{true, false, VolumeShape::Box, {0, 0, 0}, 0.0f, {1, 1, 1}, true});
    EmitterInstance cut(boxed);
    cut.Emit(400);
    cut.Update(0.01f);
    bool outside = cut.Count() > 250 && cut.Count() < 350;
    for (usize i = 0; i < cut.Count(); ++i) outside &= std::fabs(cut.Particles().position[i].x) > 1.0f;
    CHECK(outside);

    // A ground plane: bounces, loses speed, never falls through.
    Emitter ball = Basic();
    ball.init.push_back(InitSize{true, FloatRange::Constant(0.2f)});
    ball.update.push_back(Gravity{});
    ball.update.push_back(CollisionPlane{true, true, {0, 1, 0}, 0.0f, 0.5f, 0.0f, 0.0f, 0.5f});
    EmitterInstance bouncing(ball);
    bouncing.Teleport({{0, 2, 0}, {}});
    bouncing.Emit(1);
    f32 lowest = 10.0f, top_after_bounce = 0.0f;
    bool bounced = false;
    for (int i = 0; i < 300; ++i) {
        bouncing.Update(1.0f / 120.0f);
        const f32 y = bouncing.Particles().position[0].y, vy = bouncing.Particles().velocity[0].y;
        lowest = std::min(lowest, y);
        if (vy > 0.0f) bounced = true;
        if (bounced) top_after_bounce = std::max(top_after_bounce, y);
    }
    CHECK(lowest >= 0.1f - 1e-4f && bounced && top_after_bounce > 0.3f && top_after_bounce < 1.0f); // a quarter of the height
    Emitter fragile = ball;
    std::get<CollisionPlane>(fragile.update[1]).lifetime_loss = 1.0f; // dies on the first hit
    EmitterInstance shatter(fragile);
    shatter.Teleport({{0, 1, 0}, {}});
    shatter.Emit(1);
    Run(shatter, 1.0f, 0.01f);
    CHECK(shatter.Count() == 0);

    // Size and colour over life, speed over life, spin.
    Emitter fading = Basic(1.0f);
    fading.init.push_back(InitSize{true, FloatRange::Constant(2.0f)});
    fading.init.push_back(InitColor{true, ColorGradient::Constant({1, 0.5f, 0, 1})});
    fading.init.push_back(InitRotation{true, FloatRange::Constant(1.0f), FloatRange::Constant(2.0f)});
    fading.init.push_back(InitVelocity{true, VelocityMode::Direction, {1, 0, 0}, 0.0f, FloatRange::Constant(1)});
    fading.update.push_back(SizeOverLife{true, FloatCurve::Line(1, 0)});
    fading.update.push_back(ColorOverLife{true, ColorGradient::Fade({1, 1, 1, 1}, {1, 1, 1, 0})});
    fading.update.push_back(SpeedOverLife{true, FloatCurve::Constant(0.0f)});
    EmitterInstance fades(fading);
    fades.Emit(1);
    Run(fades, 0.25f, 0.05f);
    const ParticleBuffer& fp = fades.Particles();
    CHECK(Near(fp.size[0], 1.5f) && Near(fp.color[0].a, 0.75f) && Near(fp.color[0].g, 0.5f) && Near(fp.rotation[0], 1.5f) && fp.position[0].x == 0.0f);

    // Warmup, stopping, bounds, and whole systems.
    Emitter warm = Basic();
    warm.settings.warmup = 1.0f;
    warm.spawn.push_back(SpawnRate{true, 10.0f});
    EmitterInstance warmed(warm);
    warmed.Update(0.0f);
    CHECK(warmed.Count() == 10 && Near(warmed.Time(), 1.0f));
    warmed.Stop();
    Run(warmed, 1.0f);
    CHECK(warmed.Count() == 10 && !warmed.Spawning() && !warmed.Finished());
    const Bounds bounds = shaped({true, ShapeKind::Box, 0.0f, {1, 2, 3}, 1.0f})->ComputeBounds();
    CHECK(bounds.valid && bounds.max.x <= 1.05f + 1e-4f && bounds.max.x > 0.9f && bounds.min.z >= -3.05f - 1e-4f && !EmitterInstance(Basic()).ComputeBounds().valid);
    ParticleSystemAsset asset;
    asset.emitters = {e, short_lived};
    asset.emitters[1].settings.looping = false;
    asset.emitters[1].settings.duration = 1.0f;
    ParticleSystemInstance system(asset, 5);
    system.Teleport({{1, 2, 3}, {}});
    for (int i = 0; i < 10; ++i) system.Update(0.1f);
    CHECK(system.EmitterCount() == 2 && system.Count() == system.Emitter(0).Count() + system.Emitter(1).Count() && system.Emitter(0).Count() == 30);
    CHECK(Near(system.Emitter(0).Particles().position[0], {1, 2, 3}) && !system.Finished());
    system.Stop();
    system.Emitter(0).Clear();
    for (int i = 0; i < 10; ++i) system.Update(0.1f);
    CHECK(system.Finished());

    // Throughput: 50,000 particles through a typical stack.
    Emitter heavy = Basic(1000.0f);
    heavy.settings.max_particles = 50000;
    heavy.init.push_back(InitVelocity{});
    heavy.update = {Gravity{}, Drag{}, SizeOverLife{true, FloatCurve::Line(1, 0)}, ColorOverLife{}, CollisionPlane{}};
    EmitterInstance big(heavy);
    big.Emit(50000);
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 20; ++i) big.Update(1.0f / 60.0f);
    const f64 ms = std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - t0).count() / 20.0;
    std::printf("    50,000 particles (gravity, drag, size, colour, a plane): %.3f ms per frame\n", ms);
    CHECK(big.Count() == 50000);
}
