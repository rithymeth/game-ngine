#include "aether/blueprint/compiler.h"
#include "aether/blueprint/vm.h"
#include "aether/math/math.h"
#include "aether/scene/components.h"
#include "aether/vfx/particle_system.h"
#include "test_framework.h"

#include <algorithm>
#include <cmath>
#include <map>

// Phase 19 step 4: the ParticleSystem component and ParticleWorld (auto
// activation, following entities, commands, one-shots that remove
// themselves, pooling, attached effects, parameters, culling and LOD,
// render data), and the Blueprint nodes.

using namespace aether;
using namespace aether::vfx;

#define CHECK(...) AETHER_CHECK((__VA_ARGS__))

namespace {

bool Near(f32 a, f32 b, f32 tol = 1e-3f) { return std::fabs(a - b) <= tol; }
bool Near(const Vec3& a, const Vec3& b, f32 tol = 1e-3f) { return Near(a.x, b.x, tol) && Near(a.y, b.y, tol) && Near(a.z, b.z, tol); }

Emitter Base(const std::string& name, f32 lifetime) {
    Emitter e;
    e.settings.name = name;
    e.init.push_back(InitLifetime{true, FloatRange::Constant(lifetime)});
    e.init.push_back(InitSize{true, FloatRange::Constant(0.5f)});
    e.init.push_back(InitColor{});
    e.render.push_back(SpriteRenderer{});
    return e;
}

struct Library {
    std::map<std::string, ParticleSystemAsset> assets;
    Library() {
        ParticleSystemAsset smoke;
        Emitter s = Base("Smoke", 1.0f);
        s.spawn.push_back(SpawnRate{true, 10.0f});
        s.bindings = {{"Rate", "spawn[0].rate"}, {"Tint", "init[2].color"}};
        smoke.emitters = {s};
        smoke.parameters = {{"Rate", ParameterValue::Float(10)}, {"Tint", ParameterValue::Color({1, 1, 1, 1})}};
        assets["Smoke"] = smoke;
        ParticleSystemAsset burst;
        Emitter b = Base("Burst", 0.3f);
        b.settings.looping = false;
        b.settings.duration = 0.5f;
        b.spawn.push_back(SpawnBurst{true, 0.0f, FloatRange::Constant(5), 1, 1});
        burst.emitters = {b};
        assets["Burst"] = burst;
    }
    ParticleWorld::AssetLookup Lookup() {
        return [this](const std::string& name) -> const ParticleSystemAsset* {
            const auto it = assets.find(name);
            return it == assets.end() ? nullptr : &it->second;
        };
    }
};

Transform At(const Vec3& p) { return Transform{p, Quaternion::Identity()}; }

void Run(ParticleWorld& pw, f32 seconds, f32 dt = 0.05f, const ParticleCamera* cam = nullptr) {
    const long steps = std::lround(seconds / dt);
    for (long i = 0; i < steps; ++i) pw.Update(dt, cam);
}

} // namespace

AETHER_TEST(VFX_ParticleWorld) {
    Library lib;
    World world;
    ParticleWorld pw(world, lib.Lookup());
    CHECK(ParticleWorld::Active() == &pw);

    // Placed in the level: starts by itself and follows its entity.
    ParticleSystem smoke;
    smoke.asset = "Smoke";
    const Entity chimney = world.CreateEntity(At({1, 2, 3}), smoke);
    Run(pw, 0.5f);
    const ParticleSystemInstance* inst = pw.InstanceOf(chimney);
    CHECK(inst != nullptr && inst->Count() == 5 && world.GetComponent<ParticleSystem>(chimney)->IsActive());
    CHECK(Near(inst->Emitter(0).Particles().position[0], {1, 2, 3}) && pw.GetStats().systems == 1 && pw.GetStats().simulated == 1 && pw.GetStats().particles == 5);
    world.GetComponent<Transform>(chimney)->position = {10, 0, 0};
    Run(pw, 0.2f);
    const ParticleBuffer& p = inst->Emitter(0).Particles();
    bool moved = false;
    for (usize i = 0; i < p.count; ++i) moved |= p.position[i].x > 5.0f;
    CHECK(moved);

    // Parameters through the component, and bad ones reported once.
    ParticleSystem* ps = world.GetComponent<ParticleSystem>(chimney);
    ps->SetFloatParameter("Rate", 50.0f);
    ps->SetColorParameter("Tint", {1, 0, 0}, 0.5f);
    ps->SetFloatParameter("Nope", 1.0f);
    pw.Update(0.1f);
    CHECK(inst->GetParameter("Rate")->f == 50.0f && inst->Emitter(0).Particles().color[inst->Count() - 1] == (LinearColor{1, 0, 0, 0.5f}) && ps->commands.empty());
    world.GetComponent<ParticleSystem>(chimney)->SetFloatParameter("Nope", 2.0f);
    pw.Update(0.05f);
    CHECK(pw.Problems().size() == 1 && pw.Problems()[0].find("no parameter 'Nope'") != std::string::npos);

    // Deactivate: stops spawning, finishes when empty (once), stays in the level.
    world.GetComponent<ParticleSystem>(chimney)->Deactivate();
    Run(pw, 1.5f);
    CHECK(inst->Count() == 0 && !world.GetComponent<ParticleSystem>(chimney)->IsActive() && world.IsAlive(chimney));
    usize finished = 0;
    world.GetComponent<ParticleSystem>(chimney)->Activate();
    for (int i = 0; i < 10; ++i) {
        pw.Update(0.05f);
        finished += pw.Events().size();
    }
    // Back on from the start, still at the rate set above (50/s).
    CHECK(finished == 0 && inst->Count() == 25 && world.GetComponent<ParticleSystem>(chimney)->IsActive());

    // Not auto-activated: nothing until told.
    ParticleSystem later;
    later.asset = "Smoke";
    later.auto_activate = false;
    const Entity idle = world.CreateEntity(At({0, 0, 0}), later);
    Run(pw, 0.5f);
    CHECK(pw.InstanceOf(idle) != nullptr && pw.InstanceOf(idle)->Count() == 0 && !world.GetComponent<ParticleSystem>(idle)->IsActive());
    world.GetComponent<ParticleSystem>(idle)->Activate();
    Run(pw, 0.5f);
    CHECK(pw.InstanceOf(idle)->Count() == 5);
    world.GetComponent<ParticleSystem>(idle)->Restart();
    pw.Update(0.1f);
    CHECK(pw.InstanceOf(idle)->Count() == 1);

    // One-shots: finish, raise the event, remove their entity, and go back to the pool for the next one.
    const Entity boom = pw.SpawnAtLocation("Burst", {0, 1, 0});
    CHECK(world.GetComponent<ParticleSystem>(boom)->destroy_when_finished);
    pw.Update(0.05f);
    CHECK(pw.InstanceOf(boom)->Count() == 5);
    std::vector<ParticleSystemFinishedEvent> ends;
    for (int i = 0; i < 20; ++i) {
        pw.Update(0.05f);
        ends.insert(ends.end(), pw.Events().begin(), pw.Events().end());
    }
    CHECK(ends.size() == 1 && ends[0].entity == boom && ends[0].asset == "Burst" && !world.IsAlive(boom) && pw.GetStats().pooled == 1);
    const Entity again = Particles::SpawnEmitterAtLocation("Burst", {5, 0, 0}, {0, 90, 0});
    pw.Update(0.05f);
    CHECK(pw.GetStats().reused == 1 && pw.GetStats().pooled == 0 && pw.InstanceOf(again)->Count() == 5 &&
          Near(pw.InstanceOf(again)->Emitter(0).Particles().position[0], {5, 0, 0}));
    const Quaternion turned = world.GetComponent<Transform>(again)->rotation;
    const Quaternion expected = Quaternion::FromAxisAngle({0, 1, 0}, Radians(90.0f));
    CHECK(Near(turned.y, expected.y) && Near(turned.w, expected.w));

    // Attached: follows its target; when the target goes, it stops, finishes and goes too.
    const Entity ship = world.CreateEntity(At({0, 0, 0}));
    const Entity exhaust = Particles::SpawnEmitterAttached("Smoke", ship, {0, 0, -2});
    CHECK(!exhaust.IsNull() && Particles::SpawnEmitterAttached("Smoke", kNullEntity, {}).IsNull() && pw.Problems().size() == 2);
    world.GetComponent<Transform>(ship)->position = {20, 0, 0};
    world.GetComponent<Transform>(ship)->rotation = Quaternion::FromAxisAngle({0, 1, 0}, Radians(90.0f));
    pw.Update(0.1f);
    CHECK(Near(world.GetComponent<Transform>(exhaust)->position, {18, 0, 0}) && pw.InstanceOf(exhaust)->Count() == 1);
    world.DestroyEntity(ship);
    ends.clear();
    for (int i = 0; i < 30; ++i) {
        pw.Update(0.05f);
        ends.insert(ends.end(), pw.Events().begin(), pw.Events().end());
    }
    // (The second burst finished in these frames too.)
    CHECK(!world.IsAlive(exhaust) && std::count_if(ends.begin(), ends.end(), [&](const ParticleSystemFinishedEvent& x) { return x.entity == exhaust; }) == 1 &&
          !world.IsAlive(again));

    // A new asset makes a new instance; a removed component returns its instance to the pool.
    world.GetComponent<ParticleSystem>(idle)->SetAsset("Burst");
    pw.Update(0.05f);
    CHECK(pw.InstanceOf(idle) != nullptr && pw.InstanceOf(idle)->Emitter(0).Asset().settings.name == "Burst");
    const usize pooled = pw.GetStats().pooled;
    world.RemoveComponent<ParticleSystem>(chimney);
    pw.Update(0.05f);
    CHECK(pw.InstanceOf(chimney) == nullptr && pw.GetStats().pooled == pooled + 1);

    // Missing assets and transforms, reported once.
    ParticleSystem missing;
    missing.asset = "Nope";
    world.CreateEntity(At({}), missing);
    ParticleSystem floating;
    floating.asset = "Smoke";
    world.CreateEntity(floating);
    pw.Update(0.05f);
    pw.Update(0.05f);
    const std::vector<std::string>& problems = pw.Problems();
    auto reported = [&](const char* text) {
        return std::count_if(problems.begin(), problems.end(), [&](const std::string& x) { return x.find(text) != std::string::npos; }) == 1;
    };
    CHECK(problems.size() == 4 && reported("'Nope' couldn't be found") && reported("its entity has no Transform"));
}

AETHER_TEST(VFX_ParticleWorldCullingAndDrawing) {
    Library lib;
    World world;
    ParticleWorld pw(world, lib.Lookup());
    ParticleSystem near_ps, far_ps, lod_ps;
    near_ps.asset = far_ps.asset = lod_ps.asset = "Smoke";
    far_ps.cull_distance = 50.0f;
    lod_ps.lod_distance = 20.0f;
    lod_ps.lod_spawn_scale = 0.5f;
    const Entity near_e = world.CreateEntity(At({0, 0, -5}), near_ps);
    const Entity far_e = world.CreateEntity(At({0, 0, -100}), far_ps);
    const Entity lod_e = world.CreateEntity(At({0, 0, -30}), lod_ps);
    const ParticleCamera cam = ParticleCamera::FromView(Mat4::LookAtRH({0, 0, 0}, {0, 0, -1}, {0, 1, 0}));
    Run(pw, 1.0f, 0.05f, &cam);
    CHECK(pw.InstanceOf(near_e)->Count() == 10 && pw.InstanceOf(far_e)->Count() == 0 && pw.InstanceOf(lod_e)->Count() == 5);
    CHECK(pw.GetStats().culled == 1 && pw.GetStats().lod == 1 && pw.GetStats().simulated == 2 && pw.GetStats().particles == 15);
    CHECK(pw.InstanceOf(far_e)->Emitter(0).Time() == 0.0f); // paused, not running unseen
    // Drawn: what isn't culled.
    ParticleRenderData data;
    pw.BuildRenderData(cam, data);
    CHECK(data.sprites.size() == 2 && data.DrawnParticles() == 15);
    // With the view-projection, systems wholly off screen are left out.
    const Mat4 proj = Mat4::PerspectiveRH(Radians(60.0f), 16.0f / 9.0f, 0.1f, 1000.0f);
    const Mat4 facing = proj * Mat4::LookAtRH({0, 0, 0}, {0, 0, -1}, {0, 1, 0});
    const Mat4 away = proj * Mat4::LookAtRH({0, 0, 0}, {0, 0, 1}, {0, 1, 0});
    data.Clear();
    pw.BuildRenderData(cam, data, &facing);
    CHECK(data.sprites.size() == 2);
    data.Clear();
    pw.BuildRenderData(cam, data, &away);
    CHECK(data.sprites.empty());
    // Coming closer un-culls it; time scale speeds it up.
    world.GetComponent<Transform>(far_e)->position = {0, 0, -10};
    world.GetComponent<ParticleSystem>(far_e)->time_scale = 2.0f;
    Run(pw, 0.5f, 0.05f, &cam);
    CHECK(pw.InstanceOf(far_e)->Count() == 10 && pw.GetStats().culled == 0);
}

AETHER_TEST(VFX_ParticleBlueprintNodes) {
    Library lib;
    RegisterParticleComponents();
    bp::Blueprint blueprint;
    bp::Graph events;
    events.name = "EventGraph";
    blueprint.graphs.push_back(events);
    bp::GraphBuilder b(*blueprint.FindGraph("EventGraph"));
    const bp::NodeId begin = b.Add("Event.BeginPlay");
    const bp::NodeId activate = b.Add("Call.Native:ParticleSystem.Activate");
    const bp::NodeId spawn = b.Add("Call.Native:Particles.SpawnEmitterAtLocation");
    const bp::NodeId tint = b.Add("Call.Native:ParticleSystem.SetColorParameter");
    b.Default(spawn, "asset", "Smoke").Default(spawn, "location", nlohmann::json::array({0, 5, 0})).Default(spawn, "rotation_degrees", nlohmann::json::array({0, 0, 0}));
    b.Default(tint, "name", "Tint").Default(tint, "rgb", nlohmann::json::array({0, 1, 0})).Default(tint, "alpha", 1.0);
    b.Connect(begin, "then", activate, "exec").Connect(activate, "then", spawn, "exec");
    b.Connect(spawn, "then", tint, "exec").Connect(spawn, "return", tint, "target");
    const bp::NodeId finished = b.Add("Event.OnParticleSystemFinished"), print = b.Add("Debug.Print");
    b.Connect(finished, "then", print, "exec").Connect(finished, "asset", print, "text");
    bp::CompileResult compiled = bp::CompileBlueprint(blueprint);
    for (const bp::Diagnostic& d : compiled.diagnostics.diagnostics) std::printf("    %s: %s\n", d.code.c_str(), d.message.c_str());
    CHECK(compiled.Ok());

    World world;
    ParticleWorld pw(world, lib.Lookup());
    bp::BlueprintVM vm(world);
    std::vector<std::string> printed;
    vm.SetPrintHandler([&](Entity, const std::string& text) { printed.push_back(text); });
    ParticleSystem ps;
    ps.asset = "Burst";
    ps.auto_activate = false;
    const Entity e = world.CreateEntity(At({0, 0, 0}), ps);
    CHECK(vm.Attach(e, compiled.blueprint));
    vm.BeginPlay();
    // Activate queued; Spawn Emitter at Location made an entity whose tint was set.
    CHECK(world.GetComponent<ParticleSystem>(e)->commands.size() == 1);
    Entity spawned = kNullEntity;
    world.ForEachArchetype([&](Archetype& a) {
        if (!a.Mask().test(GetComponentId<ParticleSystem>())) return;
        for (usize c = 0; c < a.ChunkCount(); ++c) {
            for (usize i = 0; i < a.ChunkEntityCount(c); ++i) {
                if (a.EntityArray(c)[i] != e) spawned = a.EntityArray(c)[i];
            }
        }
    });
    CHECK(!spawned.IsNull() && Near(world.GetComponent<Transform>(spawned)->position, {0, 5, 0}) && world.GetComponent<ParticleSystem>(spawned)->commands.size() == 1);
    for (int frame = 0; frame < 30; ++frame) {
        pw.Update(0.05f);
        for (const ParticleSystemFinishedEvent& ev : pw.Events()) {
            const bp::VmValue args[] = {ev.asset};
            vm.Dispatch(ev.entity, ParticleWorld::kFinishedEvent, args);
        }
    }
    CHECK(printed == std::vector<std::string>{"Burst"} && pw.InstanceOf(spawned) != nullptr && pw.InstanceOf(spawned)->GetParameter("Tint")->c.g == 1.0f &&
          pw.InstanceOf(spawned)->GetParameter("Tint")->c.r == 0.0f);
}
