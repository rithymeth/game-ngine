#include "aether/math/math.h"
#include "aether/vfx/gpu.h"
#include "test_framework.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>

// Phase 19 step 5: the GPU path - what can run there, choosing CPU or GPU,
// the generated compute shader (compiled by glslang when it's installed),
// its constants, baked curves, frame constants and the CPU-side driver.

using namespace aether;
using namespace aether::vfx;

#define CHECK(...) AETHER_CHECK((__VA_ARGS__))

namespace {

bool Near(f32 a, f32 b, f32 tol = 1e-3f) { return std::fabs(a - b) <= tol; }

bool GlslangAvailable() {
    static const bool available = std::system("glslangValidator --version > /dev/null 2>&1") == 0;
    return available;
}

// Compiles every kernel of a generated shader to SPIR-V (and validates it, with spirv-val).
bool Compiles(const GeneratedParticleShader& g, std::string& log) {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "aether_vfx_gpu";
    std::filesystem::create_directories(dir);
    const std::filesystem::path src = dir / "particles.hlsl", out = dir / "log.txt", spv = dir / "out.spv";
    {
        std::ofstream f(src);
        f << g.hlsl;
    }
    static const bool validator = std::system("spirv-val --version > /dev/null 2>&1") == 0;
    bool ok = true;
    log.clear();
    for (const char* entry : {"ResetMain", "EmitMain", "UpdateMain"}) {
        const std::string cmd = std::string("glslangValidator -D -V -S comp -e ") + entry + " \"" + src.string() + "\" -o \"" + spv.string() + "\" > \"" + out.string() +
                                "\" 2>&1" + (validator ? " && spirv-val \"" + spv.string() + "\" >> \"" + out.string() + "\" 2>&1" : std::string());
        if (std::system(cmd.c_str()) != 0) {
            ok = false;
            std::ifstream l(out);
            log += std::string(entry) + ":\n" + std::string(std::istreambuf_iterator<char>(l), std::istreambuf_iterator<char>());
        }
    }
    std::filesystem::remove_all(dir);
    return ok;
}

// Every GPU-able module, with the shapes and modes given.
Emitter Everything(ShapeKind shape, VelocityMode mode, SimSpace space, VolumeShape volume, bool world_modules) {
    Emitter e;
    e.settings.space = space;
    e.settings.max_particles = 100000;
    e.spawn = {SpawnRate{true, 1000}, SpawnBurst{}, SpawnPerDistance{}};
    InitShape s;
    s.shape = shape;
    InitVelocity v;
    v.mode = mode;
    e.init = {InitLifetime{}, s, v, InitSize{}, InitColor{true, ColorGradient::Fade({1, 0, 0, 1}, {0, 0, 1, 0})}, InitRotation{true, {0, 1}, {-1, 1}},
              InheritVelocity{}};
    Vortex vortex;
    vortex.world = world_modules;
    PointAttractor attractor;
    attractor.world = world_modules;
    attractor.kill_radius = 0.1f;
    KillVolume kill;
    kill.world = world_modules;
    kill.shape = volume;
    CollisionPlane plane;
    plane.world = !world_modules;
    e.update = {Gravity{}, Drag{}, CurlNoiseForce{}, vortex, attractor, SpeedOverLife{true, FloatCurve::Line(1, 0.5f)}, plane, SceneCollision{}, kill,
                SizeOverLife{true, FloatCurve{{{0, 0}, {0.2f, 1}, {1, 0}}, CurveInterp::Smooth}}, ColorOverLife{}};
    MeshRenderer rocks;
    rocks.mesh = "SM_Rock";
    e.render = {SpriteRenderer{}, rocks};
    return e;
}

} // namespace

AETHER_TEST(VFX_GpuSupportAndTargets) {
    Emitter plain;
    plain.spawn = {SpawnRate{}};
    plain.render = {SpriteRenderer{}};
    CHECK(CheckGpuSupport(plain).supported && CheckGpuSupport(plain).reasons.empty());
    Emitter needy = plain;
    needy.sub_emitters.push_back({ParticleEventKind::Death, "Sparks"});
    needy.render.push_back(LightRenderer{});
    needy.render.push_back(RibbonRenderer{});
    const GpuSupport s = CheckGpuSupport(needy);
    CHECK(!s.supported && s.reasons.size() == 3 && s.reasons[0].find("sub-emitters") != std::string::npos);
    LightRenderer off;
    off.enabled = false;
    Emitter disabled_light = plain;
    disabled_light.render.push_back(off);
    CHECK(CheckGpuSupport(disabled_light).supported);
    // CPU or GPU.
    plain.settings.max_particles = 1000;
    CHECK(ChooseSimTarget(plain, true) == SimTarget::Cpu); // Auto: too small to be worth it
    plain.settings.max_particles = 50000;
    CHECK(ChooseSimTarget(plain, true) == SimTarget::Gpu && ChooseSimTarget(plain, false) == SimTarget::Cpu);
    CHECK(ChooseSimTarget(plain, true, 100000) == SimTarget::Cpu);
    plain.settings.target = SimTarget::Cpu;
    CHECK(ChooseSimTarget(plain, true) == SimTarget::Cpu);
    plain.settings.target = SimTarget::Gpu;
    plain.settings.max_particles = 10;
    CHECK(ChooseSimTarget(plain, true) == SimTarget::Gpu && ChooseSimTarget(plain, false) == SimTarget::Cpu);
    needy.settings.target = SimTarget::Gpu;
    CHECK(ChooseSimTarget(needy, true) == SimTarget::Cpu); // can't
    // Saved.
    ParticleSystemAsset asset;
    asset.emitters = {plain};
    ParticleSystemAsset loaded;
    std::string error;
    CHECK(LoadParticleSystem(SaveParticleSystem(asset), loaded) && loaded.emitters[0].settings.target == SimTarget::Gpu);
    CHECK(!LoadParticleSystem(R"({"emitters":[{"name":"A","target":"Tpu"}]})", loaded, &error) && error.find("Tpu") != std::string::npos);
    const GeneratedParticleShader refused = GenerateParticleShader(needy);
    CHECK(!refused.ok && refused.errors.size() == 3 && refused.hlsl.empty());
}

AETHER_TEST(VFX_GpuShaderGeneration) {
    const Emitter e = Everything(ShapeKind::Box, VelocityMode::Cone, SimSpace::World, VolumeShape::Box, false);
    const GeneratedParticleShader g = GenerateParticleShader(e);
    CHECK(g.ok && g.uses_depth && g.uses_noise && g.thread_group_size == 64);
    for (const char* piece : {"void ResetMain", "void EmitMain", "void UpdateMain", "cbuffer Frame", "cbuffer Modules", "Texture2D<float> SceneDepth", "CurlNoise(",
                              "RWStructuredBuffer<Particle> Particles"}) {
        CHECK(g.hlsl.find(piece) != std::string::npos);
    }
    // The constants: every value the modules read, in order, named.
    const std::vector<f32> constants = PackModuleConstants(e);
    CHECK(constants.size() == g.module_constants * 4u && g.constant_names.size() == g.module_constants);
    // 7 init float4s + 16 for the colour gradient; 14 update values + 3 tables (4 + 4 + 16).
    CHECK(g.module_constants == (1 + 2 + 2 + 1 + 16 + 1 + 1) + (1 + 1 + 1 + 2 + 2 + 4 + 2 + 1 + 2 + 4 + 16));
    const auto gravity = std::find(g.constant_names.begin(), g.constant_names.end(), "update[0] Gravity.acceleration");
    CHECK(gravity != g.constant_names.end());
    const usize slot = static_cast<usize>(gravity - g.constant_names.begin());
    CHECK(Near(constants[slot * 4 + 1], -9.81f) && Near(constants[slot * 4 + 0], 0.0f));
    // Values change without new code: the key stays; a different stack's differs.
    Emitter tuned = e;
    CHECK(SetModuleField(tuned, "update[0].acceleration", ParameterValue::Vector({0, -1, 0})));
    CHECK(GenerateParticleShader(tuned).key == g.key && PackModuleConstants(tuned)[slot * 4 + 1] == -1.0f);
    Emitter reshaped = e;
    std::get<InitShape>(reshaped.init[1]).shape = ShapeKind::Sphere;
    CHECK(GenerateParticleShader(reshaped).key != g.key);
    Emitter disabled = e;
    std::visit([](auto& m) { m.enabled = false; }, disabled.update[2]); // no curl noise
    CHECK(!GenerateParticleShader(disabled).uses_noise && GenerateParticleShader(disabled).module_constants == g.module_constants - 1);

    // It compiles: every shape, velocity mode, space, volume and module frame.
    if (!GlslangAvailable()) {
        std::printf("  (glslangValidator not found: skipping the HLSL compile check)\n");
        return;
    }
    std::string log;
    const bool compiled = Compiles(g, log);
    if (!compiled) std::printf("%s\n", log.c_str());
    CHECK(compiled);
    int variants = 0, failures = 0;
    for (ShapeKind shape : {ShapeKind::Point, ShapeKind::Sphere, ShapeKind::Hemisphere, ShapeKind::Cone, ShapeKind::Circle, ShapeKind::Edge}) {
        for (VelocityMode mode : {VelocityMode::Radial, VelocityMode::Direction}) {
            const bool local = (variants % 2) == 1;
            const Emitter v = Everything(shape, mode, local ? SimSpace::Local : SimSpace::World, variants % 3 == 0 ? VolumeShape::Sphere : VolumeShape::Box,
                                         variants % 4 < 2);
            ++variants;
            if (!Compiles(GenerateParticleShader(v), log)) {
                ++failures;
                std::printf("%s\n", log.c_str());
            }
        }
    }
    Emitter bare; // nothing but defaults: one Modules float4 still declared
    const GeneratedParticleShader minimal = GenerateParticleShader(bare);
    CHECK(minimal.ok && minimal.module_constants == 0 && !minimal.uses_depth && !minimal.uses_noise && Compiles(minimal, log));
    CHECK(variants == 12 && failures == 0);
}

AETHER_TEST(VFX_GpuTablesFramesAndDriver) {
    // Baked curves read back close to the curve.
    const FloatCurve smooth{{{0, 0}, {0.3f, 1}, {1, 0.2f}}, CurveInterp::Smooth};
    const std::vector<f32> table = BakeCurve(smooth);
    CHECK(table.size() == kCurveSamples && table.front() == 0.0f && Near(table.back(), 0.2f));
    f32 worst = 0.0f;
    for (int i = 0; i <= 100; ++i) {
        const f32 t = static_cast<f32>(i) / 100.0f;
        worst = std::max(worst, std::fabs(SampleBakedCurve(table, t) - smooth.Evaluate(t)));
    }
    CHECK(worst < 0.03f && SampleBakedCurve({}, 0.5f) == 0.0f && SampleBakedCurve({2.0f}, 0.5f) == 2.0f && SampleBakedCurve(table, 7.0f) == table.back());
    const std::vector<LinearColor> colors = BakeGradient(ColorGradient::Fade({1, 0, 0, 1}, {0, 0, 1, 0}));
    CHECK(colors.size() == kGradientSamples && colors.front() == (LinearColor{1, 0, 0, 1}) && colors.back() == (LinearColor{0, 0, 1, 0}) && Near(colors[5].r, 1.0f - 5.0f / 15.0f));

    // Frame constants in the cbuffer's layout.
    GpuFrameConstants f;
    f.delta_time = 0.5f;
    f.spawn_count = 7;
    f.frame_seed = 99;
    f.emitter_position = {1, 2, 3};
    f.max_particles = 1000;
    f.emitter_rotation = Quaternion(0.1f, 0.2f, 0.3f, 0.9f);
    f.previous_position = {4, 5, 6};
    f.emitter_velocity = {7, 8, 9};
    f.depth_width = 640, f.depth_height = 480;
    f.view_projection = Mat4::Translation({10, 20, 30});
    const std::vector<u32> words = PackFrameConstants(f);
    auto as_f = [&](usize i) {
        f32 v;
        std::memcpy(&v, &words[i], sizeof v);
        return v;
    };
    CHECK(words.size() == kFrameConstantWords && as_f(0) == 0.5f && words[2] == 7 && words[3] == 99 && as_f(6) == 3.0f && words[7] == 1000);
    CHECK(as_f(9) == 0.2f && as_f(11) == 0.9f && as_f(12) == 4.0f && as_f(18) == 9.0f && words[19] == 640 && words[20] == 480);
    CHECK(as_f(24 + 12) == 10.0f && as_f(24 + 13) == 20.0f && as_f(24 + 0) == 1.0f && as_f(40 + 15) == 1.0f); // column-major; the inverse left as identity

    // The driver: the clock and spawn modules on the CPU, counts and constants for the GPU.
    Emitter e;
    e.settings.max_particles = 100000;
    e.spawn = {SpawnRate{true, 30.0f}, SpawnBurst{true, 0.5f, FloatRange::Constant(100), 1, 1}};
    e.init = {InitSize{}};
    e.update = {Gravity{}};
    GpuEmitterDriver driver(e, 5);
    driver.Teleport({{0, 0, 0}, {}});
    u64 total = 0;
    u32 last_seed = 0;
    bool seeds_differ = true;
    for (int i = 0; i < 10; ++i) {
        const GpuFrameConstants fc = driver.Update(0.1f);
        total += fc.spawn_count;
        seeds_differ &= fc.frame_seed != last_seed;
        last_seed = fc.frame_seed;
        CHECK(fc.max_particles == 100000 && Near(fc.delta_time, 0.1f));
    }
    CHECK(total == 130 && driver.TotalSpawned() == 130 && seeds_differ);
    // Motion: previous position and velocity for sub-frame births and InheritVelocity.
    driver.SetPose({{2, 0, 0}, {}});
    const GpuFrameConstants moved = driver.Update(0.5f);
    CHECK(moved.emitter_position.x == 2.0f && moved.previous_position.x == 0.0f && Near(moved.emitter_velocity.x, 4.0f) && moved.spawn_count == 15);
    const GpuFrameConstants still = driver.Update(0.5f);
    CHECK(still.previous_position.x == 2.0f && Near(still.emitter_velocity.x, 0.0f));
    // Parameters reach the spawner and the constants.
    CHECK(driver.SetField("spawn[0].rate", ParameterValue::Float(100.0f)) && driver.SetField("update[0].acceleration", ParameterValue::Vector({0, 0, -2})));
    CHECK(driver.Update(0.1f).spawn_count == 10 && driver.ModuleConstants()[4 + 2] == -2.0f);
    std::string error;
    CHECK(!driver.SetField("spawn[5].rate", ParameterValue::Float(1), &error) && !error.empty());
    driver.Stop();
    CHECK(driver.Update(1.0f).spawn_count == 0 && !driver.Spawning());
    driver.Restart();
    CHECK(driver.Spawning() && driver.Update(0.1f).spawn_count == 10);
    CHECK(driver.Asset().spawn.size() == 2 && std::get<SpawnRate>(driver.Asset().spawn[0]).rate == 100.0f);
}
