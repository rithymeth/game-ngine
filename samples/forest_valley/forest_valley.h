#pragma once

// Forest Valley: the world of the integrated showcase, free of rendering and platform code so it runs headless and is tested.
//
// BuildWorld generates a 512 x 512 m valley from a fixed seed, picks five areas on it by stated rules (Area A the meadow ... Area E
// the overlook), proves they are joined by ground a person can walk, and lays the walking route between them. Walker is the on-foot
// controller over that terrain. BenchmarkCameraPath turns the route into the keyframe file `aether_player --camera-path` flies, so
// every performance run uses the same route. Units are metres, +y is up, the terrain's origin is the world origin.

#include "aether/core/base.h"
#include "aether/math/math.h"
#include "aether/renderer/components.h"
#include "aether/terrain/generator.h"
#include "aether/terrain/scatter.h"

#include <array>
#include <string>
#include <vector>

namespace aether::forest_valley {

inline constexpr u32 kShowcaseSeed = 20260101;
inline constexpr f32 kWorldSize = 512.0f;
inline constexpr f32 kMaxWalkSlope = 0.75f; // rise over run, about 37 degrees: steeper ground is not walkable

enum class AreaId : u8 { Meadow, Woodland, River, Ruins, Overlook };
inline constexpr u32 kAreaCount = 5;
const char* AreaName(AreaId id);

struct Area {
    AreaId id = AreaId::Meadow;
    Vec3 center{0, 0, 0}; // on the terrain surface
    f32 radius = 0.0f;    // metres the area is meant to cover
};

struct World {
    terrain::TerrainGenSettings settings;
    terrain::GeneratedTerrain terrain;
    std::array<Area, kAreaCount> areas;
    std::vector<Vec3> route; // on the ground, in visiting order: meadow, woodland, river, ruins, overlook
    bool connected = false;  // every area reached from the meadow over walkable ground
    f32 route_length = 0.0f; // metres, along the ground

    f32 HeightAt(f32 x, f32 z) const { return terrain.heightmap.SampleLinear(x, z); }
    bool Walkable(f32 x, f32 z) const;
    const Area& GetArea(AreaId id) const { return areas[static_cast<usize>(id)]; }
};

// The ProceduralTerrain a scene carries to show this valley: the one set of generator settings BuildWorld uses, so what the player draws is the
// ground the areas, route and walker were built on (a test regenerates it from the component and compares heights).
ProceduralTerrain ValleyTerrainComponent(u32 seed = kShowcaseSeed);

// Deterministic: the same seed gives the same world on every machine.
World BuildWorld(u32 seed = kShowcaseSeed);

// The terrain scatter budget for the world (docs/design/FOREST_VALLEY_SHOWCASE.md, asset budget): instance counts by category.
struct ScatterBudget {
    u32 trees_min = 800, trees_max = 1500;
    u32 plants_min = 12000, plants_max = 30000;
    u32 rocks_min = 500, rocks_max = 1000;
};
std::vector<terrain::ScatterRule> ShowcaseScatterRules(const ScatterBudget& budget = {});

// --- The on-foot controller -------------------------------------------------------------------------------------------------------

struct WalkerInput {
    f32 move_x = 0, move_z = 0; // desired world-space direction, length <= 1
    bool run = false;
    bool jump = false; // held; a fresh press is detected inside
};

struct WalkerConfig {
    f32 walk_speed = 3.5f;
    f32 run_speed = 6.0f;
    f32 acceleration = 30.0f;
    f32 gravity = 22.0f;
    f32 jump_speed = 6.5f;
    f32 max_fall = 40.0f;
    f32 coyote_time = 0.1f;
};

class Walker {
public:
    static constexpr f32 kStep = 1.0f / 60.0f;

    Walker(const World& world, Vec3 start, WalkerConfig config = {});
    void Step(const WalkerInput& input);

    Vec3 Position() const { return pos_; }
    Vec3 Velocity() const { return vel_; }
    bool Grounded() const { return grounded_; }
    f32 Distance() const { return distance_; } // horizontal metres travelled

private:
    const World* world_;
    WalkerConfig config_;
    Vec3 pos_, vel_{0, 0, 0};
    bool grounded_ = false;
    bool jump_was_down_ = false;
    f32 coyote_ = 0;
    f32 distance_ = 0;
};

// --- The benchmark route --------------------------------------------------------------------------------------------------------

// The camera path file for `aether_player --camera-path`: the route at `speed` m/s with the camera `eye_height` above the ground, looking
// along the direction of travel. Same world and parameters give the same bytes, which is what makes a run repeatable.
std::string BenchmarkCameraPathJson(const World& world, f32 speed = 4.0f, f32 fps = 30.0f, f32 eye_height = 1.7f);

// The measured performance gates (docs/design/FOREST_VALLEY_SHOWCASE.md): p95 CPU <= 16.67 ms, p95 GPU <= 14 ms, p99 <= 20 ms.
struct FrameGates {
    f32 cpu_p95_ms = 16.67f, gpu_p95_ms = 14.0f, p99_ms = 20.0f;
};
struct GateResult {
    bool cpu_p95 = false, gpu_p95 = false, p99 = false;
    bool All() const { return cpu_p95 && gpu_p95 && p99; }
};
// Nearest-rank percentile, the definition aether_player's frame report uses. `values` need not be sorted. Empty gives 0.
f32 Percentile(std::vector<f32> values, f64 p);
// A run with no samples fails every gate: absence of a measurement is not a pass.
GateResult EvaluateGates(const std::vector<f32>& cpu_ms, const std::vector<f32>& gpu_ms, const FrameGates& gates = {});

} // namespace aether::forest_valley
