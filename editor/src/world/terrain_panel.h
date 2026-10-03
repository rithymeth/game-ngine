#pragma once

#include "aether/terrain/heightfield.h"
#include "aether/terrain/splat.h"

#include <string>
#include <vector>

namespace aether::editor {

// The Terrain panel (Phase 21): sculpt and paint tools over a TerrainData and
// its splatmap. The viewport feeds it a mouse ray (Pick) and strokes (Begin/
// Stroke/EndStroke); each stroke becomes one undo step holding just the
// samples it changed. It reports which chunks need their meshes rebuilt and
// builds the brush cursor the viewport draws. Draw() fills the current ImGui
// window; everything runs headless in tests.

enum class TerrainTool { Sculpt, Paint };

struct TerrainChunkCoord {
    i32 x = 0;
    i32 z = 0;
    bool operator==(const TerrainChunkCoord& o) const { return x == o.x && z == o.z; }
};

struct TerrainOverlayLine {
    Vec3 a, b;
    u32 color = 0xFFFFFFFF; // ABGR, ImGui order
};

namespace terrain_colors {
inline constexpr u32 kCursor = 0xFF40E0FF;
inline constexpr u32 kCursorInner = 0x8040E0FF;
} // namespace terrain_colors

class TerrainPanel {
public:
    TerrainPanel(terrain::TerrainData& data, terrain::SplatmapData& splat, const terrain::TerrainSettings& settings);

    void Draw();

    TerrainTool tool = TerrainTool::Sculpt;
    terrain::TerrainBrush brush;
    f32 stroke_rate = 10.0f; // a stroke's strength per second, as a share of the brush strength
    bool show_cursor = true;
    usize max_history = 64;

    // Casts a viewport ray at the terrain; on a hit, remembers it as the brush position.
    bool Pick(const Vec3& origin, const Vec3& direction, f32 max_distance);
    bool HasHover() const { return has_hover_; }
    const Vec3& Hover() const { return hover_; }

    // One stroke: BeginStroke, any number of Stroke calls (dt scales the strength), EndStroke.
    // Stroke without a BeginStroke is ignored. Returns false when the point is off the terrain.
    void BeginStroke();
    bool Stroke(const Vec3& world_point, f32 dt);
    bool EndStroke(); // true if the stroke changed anything and became an undo step
    bool InStroke() const { return in_stroke_; }

    bool Undo();
    bool Redo();
    usize UndoDepth() const { return undo_.size(); }
    usize RedoDepth() const { return redo_.size(); }

    // Chunks whose meshes (or splat data) changed since the last call.
    std::vector<TerrainChunkCoord> TakeDirtyChunks();
    usize DirtyChunkCount() const { return dirty_.size(); }

    // The brush ring draped over the terrain at the hover point, plus a center mark.
    void BuildCursor(std::vector<TerrainOverlayLine>& out) const;

    const std::string& Status() const { return status_; }

private:
    struct Patch {
        bool splat = false;
        u32 x0 = 0, z0 = 0, w = 0, h = 0;
        std::vector<f32> height_before, height_after;
        std::vector<u8> pixel_before, pixel_after;
    };

    void MarkDirty(f32 world_x, f32 world_z, f32 radius);
    void RefreshBounds();
    void Apply(const Patch& patch, bool after);
    void Push(Patch patch);

    terrain::TerrainData& data_;
    terrain::SplatmapData& splat_;
    terrain::TerrainSettings settings_;
    bool in_stroke_ = false;
    bool has_hover_ = false;
    Vec3 hover_{0, 0, 0};
    std::vector<f32> heights_before_;
    std::vector<u8> pixels_before_;
    std::vector<Patch> undo_, redo_;
    std::vector<TerrainChunkCoord> dirty_;
    std::string status_;
};

} // namespace aether::editor
