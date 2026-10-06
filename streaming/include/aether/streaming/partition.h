#pragma once

#include "aether/ecs/world.h"
#include "aether/math/math.h"
#include "aether/reflection/reflection.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace aether {

// Keeps an entity in the persistent scene (never streamed out), wherever it is.
struct AlwaysLoaded {
    bool enabled = true;
};

// A camera, player or other point the world streams in around.
struct StreamingSource {
    f32 radius = 128.0f; // cells within this are loaded
    bool enabled = true;
};

void RegisterStreamingComponents();

} // namespace aether

namespace aether::streaming {

// World partition (Phase 21 step 6, docs/design/PHASE_SPECS.md §21.6): a
// level split into square grid cells on the ground plane, each saved as its
// own additive scene, plus a persistent scene for what never streams out.

struct CellCoord {
    i32 x = 0, z = 0;
    bool operator==(const CellCoord&) const = default;
    auto operator<=>(const CellCoord&) const = default;
};
std::string CellName(const CellCoord& cell); // "x_z" ("-1_2"), as cell files are named

struct PartitionSettings {
    f32 cell_size = 64.0f;
    Vec3 origin; // cell (0, 0) starts here
};

CellCoord CellOf(const Vec3& position, const PartitionSettings& settings);
void CellBounds(const CellCoord& cell, const PartitionSettings& settings, Vec3& min, Vec3& max); // y spans everything

struct CellInfo {
    CellCoord coord;
    u32 entity_count = 0;
    Vec3 min, max; // of its entities' positions
};

// A partitioned world: the settings and what each cell holds.
struct WorldIndex {
    PartitionSettings settings;
    std::vector<CellInfo> cells; // sorted by coord
    u32 persistent_count = 0;
    const CellInfo* Find(const CellCoord& cell) const;
};
nlohmann::json SaveWorldIndex(const WorldIndex& index);
bool LoadWorldIndex(const nlohmann::json& json, WorldIndex& out, std::string* error = nullptr);

struct PartitionResult {
    WorldIndex index;
    std::map<CellCoord, std::vector<u8>> cells; // each a binary scene (.aesc in memory)
    std::vector<u8> persistent;
};

// Splits a world: root entities with a Transform go to the cell their position
// is in, and their descendants (by Parent) with them; entities with no
// Transform, or AlwaysLoaded, or a StreamingSource, go to the persistent scene.
PartitionResult PartitionWorld(const World& world, const PartitionSettings& settings);

// Files: <dir>/world.aworld (the index, JSON), <dir>/persistent.aesc, <dir>/cells/<x_z>.aesc.
bool SavePartition(const PartitionResult& partition, const std::filesystem::path& dir, std::string* error = nullptr);
bool LoadWorldIndexFile(const std::filesystem::path& dir, WorldIndex& out, std::string* error = nullptr);
std::filesystem::path CellFile(const std::filesystem::path& dir, const CellCoord& cell);

// Copies one entity (all its components) from one world into another; the new entity.
Entity CopyEntity(const World& from, Entity entity, World& to);

} // namespace aether::streaming

AETHER_REFLECT(aether::AlwaysLoaded, 1, AETHER_FIELD(enabled, Field_EditAnywhere, {.tooltip = "Never streamed out"}))
AETHER_REFLECT(aether::StreamingSource, 1,
    AETHER_FIELD(radius, Field_EditAnywhere, {.tooltip = "Cells within this are loaded", .range_min = 0.0, .range_max = 100000.0, .units = "m"}),
    AETHER_FIELD(enabled, Field_EditAnywhere)
)
