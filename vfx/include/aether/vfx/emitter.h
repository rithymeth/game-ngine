#pragma once

#include "aether/vfx/curves.h"

#include <string>
#include <variant>
#include <vector>

namespace aether::vfx {

// An emitter is a stack of modules in three stages, run in order each
// frame (Phase 19 step 1, §19.1), as in Niagara or Unity's VFX Graph:
//   Spawn:      how many particles are born (rate, bursts, per distance moved)
//   Initialize: each new particle's lifetime, place, velocity, size, colour, rotation
//   Update:     forces, collisions and kill volumes, then size and colour over life
// Directions in Initialize modules are in the emitter's frame, so a turned
// emitter sprays where it faces. Places in Update modules are the
// emitter's too unless `world` is set (a ground plane).

// --- Spawn -----------------------------------------------------------------------------------------------
struct SpawnRate {
    bool enabled = true;
    f32 rate = 10.0f; // particles per second
};
struct SpawnBurst {
    bool enabled = true;
    f32 time = 0.0f;             // seconds into each loop
    FloatRange count{10.0f, 10.0f};
    u32 cycles = 1;              // how many times per loop (0: every `interval` until the loop ends)
    f32 interval = 1.0f;         // seconds between cycles
};
struct SpawnPerDistance {
    bool enabled = true;
    f32 per_unit = 1.0f; // particles per world unit the emitter moves (trails behind a moving object)
};

// --- Initialize ------------------------------------------------------------------------------------------
enum class ShapeKind : u8 { Point, Sphere, Hemisphere, Box, Cone, Circle, Edge };
struct InitShape {
    bool enabled = true;
    ShapeKind shape = ShapeKind::Point;
    f32 radius = 1.0f;              // Sphere, Hemisphere (+Y half), Cone (base), Circle (XZ), Edge (half length, along X)
    Vec3 half_extents{0.5f, 0.5f, 0.5f}; // Box
    f32 thickness = 1.0f;           // 1: the whole volume or disc; 0: only its surface or rim
};
enum class VelocityMode : u8 {
    Cone,      // within `cone_angle` of `direction`
    Radial,    // away from the emitter's origin (from where the shape put it)
    Direction, // exactly along `direction`
};
struct InitVelocity {
    bool enabled = true;
    VelocityMode mode = VelocityMode::Cone;
    Vec3 direction{0.0f, 1.0f, 0.0f};
    f32 cone_angle = 0.3f; // radians
    FloatRange speed{1.0f, 2.0f};
};
struct InitLifetime {
    bool enabled = true;
    FloatRange seconds{1.0f, 2.0f};
};
struct InitSize {
    bool enabled = true;
    FloatRange size{0.1f, 0.2f}; // world units across
};
struct InitColor {
    bool enabled = true;
    ColorGradient color = ColorGradient::Constant({}); // each particle picks a random point along it
};
struct InitRotation {
    bool enabled = true;
    FloatRange angle{0.0f, 0.0f}; // radians
    FloatRange spin{0.0f, 0.0f};  // radians per second
};
struct InheritVelocity {
    bool enabled = true;
    f32 amount = 1.0f; // of the emitter's own velocity
};

// --- Update ----------------------------------------------------------------------------------------------
struct Gravity {
    bool enabled = true;
    Vec3 acceleration{0.0f, -9.81f, 0.0f}; // world
};
struct Drag {
    bool enabled = true;
    f32 coefficient = 1.0f; // velocity falls by e^-coefficient per second
};
struct CurlNoiseForce {
    bool enabled = true;
    f32 strength = 1.0f;  // units per second squared
    f32 frequency = 1.0f; // noise cells per unit
    f32 scroll = 0.0f;    // the field drifts this fast (units per second), so still particles still swirl
};
struct Vortex {
    bool enabled = true;
    bool world = false;
    Vec3 center{0.0f, 0.0f, 0.0f};
    Vec3 axis{0.0f, 1.0f, 0.0f};
    f32 strength = 1.0f; // spin (tangential acceleration)
    f32 pull = 0.0f;     // towards the axis
};
struct PointAttractor {
    bool enabled = true;
    bool world = false;
    Vec3 position{0.0f, 0.0f, 0.0f};
    f32 strength = 5.0f;    // acceleration at the point's reach (negative repels)
    f32 radius = 0.0f;      // reach (0: everywhere, full strength); fades linearly to it
    f32 kill_radius = 0.0f; // particles this close die
};
enum class VolumeShape : u8 { Sphere, Box };
struct KillVolume {
    bool enabled = true;
    bool world = false;
    VolumeShape shape = VolumeShape::Sphere;
    Vec3 center{0.0f, 0.0f, 0.0f};
    f32 radius = 1.0f;
    Vec3 half_extents{1.0f, 1.0f, 1.0f};
    bool kill_inside = true; // false: kill what leaves it
};
struct CollisionPlane {
    bool enabled = true;
    bool world = true;
    Vec3 normal{0.0f, 1.0f, 0.0f}; // the free side
    f32 offset = 0.0f;             // the plane is normal · p = offset
    f32 bounce = 0.5f;             // of the speed into the plane, kept
    f32 friction = 0.1f;           // of the speed along it, lost per hit
    f32 lifetime_loss = 0.0f;      // of a particle's whole life, used up per hit (1: dies)
    f32 radius_scale = 0.5f;       // the particle's radius as a share of its size
};
struct SizeOverLife {
    bool enabled = true;
    FloatCurve curve = FloatCurve::Constant(1.0f); // times the initial size
};
struct ColorOverLife {
    bool enabled = true;
    ColorGradient gradient = ColorGradient::Constant({}); // times the initial colour
};
struct SpeedOverLife {
    bool enabled = true;
    FloatCurve curve = FloatCurve::Constant(1.0f); // times the velocity, when moving
};

using SpawnModule = std::variant<SpawnRate, SpawnBurst, SpawnPerDistance>;
using InitModule = std::variant<InitLifetime, InitShape, InitVelocity, InitSize, InitColor, InitRotation, InheritVelocity>;
using UpdateModule = std::variant<Gravity, Drag, CurlNoiseForce, Vortex, PointAttractor, KillVolume, CollisionPlane, SizeOverLife, ColorOverLife, SpeedOverLife>;

// A module's type name ("SpawnRate"), and every name a stage takes (the editor's add menu).
const char* ModuleName(const SpawnModule& m);
const char* ModuleName(const InitModule& m);
const char* ModuleName(const UpdateModule& m);
std::vector<std::string> SpawnModuleNames();
std::vector<std::string> InitModuleNames();
std::vector<std::string> UpdateModuleNames();
// A module with its defaults, by name (false if there's no such module in that stage).
bool MakeModule(const std::string& name, SpawnModule& out);
bool MakeModule(const std::string& name, InitModule& out);
bool MakeModule(const std::string& name, UpdateModule& out);

enum class SimSpace : u8 {
    World, // particles stay where they were born when the emitter moves
    Local, // particles move with the emitter
};

struct EmitterSettings {
    std::string name = "Emitter";
    bool enabled = true;
    u32 max_particles = 1000; // new ones beyond this aren't born
    SimSpace space = SimSpace::World;
    f32 duration = 5.0f; // one loop, seconds
    bool looping = true;
    f32 start_delay = 0.0f;
    f32 warmup = 0.0f; // seconds simulated at once when it starts (a fire that's already burning)
};

struct Emitter {
    EmitterSettings settings;
    std::vector<SpawnModule> spawn;
    std::vector<InitModule> init;
    std::vector<UpdateModule> update;
};

// A particle system asset (.avfx): emitters that play together.
struct ParticleSystemAsset {
    std::string name;
    std::vector<Emitter> emitters;
};

nlohmann::json EmitterToJson(const Emitter& e);
bool EmitterFromJson(const nlohmann::json& j, Emitter& out, std::string* error = nullptr);
std::string SaveParticleSystem(const ParticleSystemAsset& asset);
// Errors name where: "emitter 'Sparks': update[2] (Vortex): a vector is [x, y, z]".
bool LoadParticleSystem(const std::string& text, ParticleSystemAsset& out, std::string* error = nullptr);

struct EmitterDiagnostic {
    std::string code; // FX001..
    std::string message;
    bool error = true;
};
// FX001 nothing spawns (warning); FX002 a lifetime that isn't positive;
// FX003 max_particles 0 or over a million; FX004 a range whose min is over
// its max; FX005 a burst bigger than max_particles (warning); FX006 a
// duration that isn't positive; FX007 curve or gradient keys out of order.
std::vector<EmitterDiagnostic> ValidateEmitter(const Emitter& e);

} // namespace aether::vfx
