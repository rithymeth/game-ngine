#pragma once

#include "aether/streaming/streamer.h"
#include "world/foliage_document.h"
#include "world/spline_document.h"
#include "world/terrain_document.h"

#include <optional>

namespace aether::editor {

// The world-building panels (Phase 21 step 7). Each fills the current
// ImGui window and runs headless in tests; the viewport drives their
// tools through Pointer() with the ground position under the mouse.

// Sculpt and paint: the tool, the brush (radius, strength, falloff, raise
// height, paint layer), undo and the chunks waiting to rebuild.
class TerrainToolPanel {
public:
    explicit TerrainToolPanel(TerrainEditDocument& doc) : doc_(doc) {}
    void Draw();
    // The mouse over the terrain: down starts a stroke and dabs along the drag
    // (every quarter radius); up ends it.
    void Pointer(bool down, f32 world_x, f32 world_z);
    usize Dabs() const { return dabs_; }

private:
    TerrainEditDocument& doc_;
    bool has_last_ = false;
    f32 last_x_ = 0.0f, last_z_ = 0.0f;
    usize dabs_ = 0;
};

// Foliage: the types (scale, rotation, alignment, anchor), density and the
// cap, paint or erase with a radius, and counts per type.
class FoliagePanel {
public:
    explicit FoliagePanel(FoliagePaintDocument& doc) : doc_(doc) {}
    void Draw();
    void Pointer(bool down, const Vec3& ground);

private:
    FoliagePaintDocument& doc_;
    bool has_last_ = false;
    Vec3 last_;
};

// Splines: the points (position, width, roll), add, insert and remove;
// in the viewport a click picks the nearest point (Ctrl+click adds one) and
// a drag moves it.
class SplinePanel {
public:
    explicit SplinePanel(SplineEditDocument& doc) : doc_(doc) {}
    void Draw();
    void Pointer(bool down, bool add, const Vec3& ground, f32 pick_radius = 1.0f);

private:
    SplineEditDocument& doc_;
    bool dragging_ = false;
};

// The world partition map: every cell of the index (loaded, unloaded,
// failed or pinned) with its entity count, the streaming sources and their
// reach, and stats; clicking a cell pins or unpins it.
class WorldPartitionPanel {
public:
    explicit WorldPartitionPanel(streaming::WorldStreamer& streamer, World* world = nullptr) : streamer_(streamer), world_(world) {}
    void Draw();
    f32 pixels_per_cell = 24.0f;
    // The cell under a screen position on the last drawn map.
    std::optional<streaming::CellCoord> CellAt(f32 screen_x, f32 screen_y) const;
    bool IsPinned(const streaming::CellCoord& cell) const;
    void TogglePin(const streaming::CellCoord& cell);

private:
    streaming::WorldStreamer& streamer_;
    World* world_;
    std::set<streaming::CellCoord> pinned_;
    // The last map's placement: the screen corner and the lowest cell drawn there.
    f32 map_x_ = 0.0f, map_y_ = 0.0f;
    i32 min_x_ = 0, min_z_ = 0, cols_ = 0, rows_ = 0;
};

} // namespace aether::editor
