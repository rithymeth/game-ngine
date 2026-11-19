#pragma once

#include "aether/terrain/splat.h"
#include "aether/terrain/terrain.h"

#include <filesystem>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace aether::editor {

enum class TerrainTool : u8 { Raise, Lower, Smooth, Flatten, Paint };
const char* TerrainToolName(TerrainTool tool);

// A terrain being sculpted and painted (Phase 21 step 7): the heightmap
// and splatmap, the active tool and brush, strokes made of dabs (each
// stroke one undo step, stored as the changed region only), the chunks
// they touched (for the renderer to rebuild), and the viewport's brush
// cursor. The splatmap covers the whole terrain.
//
// It is a terrain asset (.aterrain, Stage 6): Open and Save write the
// heightmap, layers, settings and splatmap, and Dirty() says whether what
// is on screen is on disk.
class TerrainEditDocument {
public:
    TerrainEditDocument(terrain::TerrainData data, terrain::TerrainSettings settings, terrain::SplatmapData splatmap = {});

    // Reads a .aterrain file (terrain::TerrainFromJson); the undo history goes and every chunk is dirty. On failure the document
    // is unchanged. The file stays the open one only when it loaded.
    bool Load(const std::filesystem::path& file, std::string* error = nullptr);
    // Writes to the open file, or to `file` (save as). Ends a stroke in progress first, so the file holds what is on screen.
    bool Save(std::string* error = nullptr);
    bool SaveAs(const std::filesystem::path& file, std::string* error = nullptr);
    // Treats what is there as saved: clears Dirty and the undo history, for a document seeded with a generated terrain.
    void MarkSaved();
    // Where Save writes; empty for a terrain that has never been saved. SetPath names the file without reading it.
    void SetPath(const std::filesystem::path& file) { path_ = file; }
    const std::filesystem::path& Path() const { return path_; }
    std::string Name() const;
    bool Dirty() const { return unsaved_; }

    const terrain::TerrainData& Data() const { return data_; }
    const terrain::TerrainSettings& Settings() const { return settings_; }
    const terrain::SplatmapData& Splatmap() const { return splat_; }
    u64 Revision() const { return revision_; }

    TerrainTool tool = TerrainTool::Raise;
    terrain::TerrainBrush brush;
    f32 raise_height = 1.0f; // world units a full-weight Raise or Lower dab moves

    // A stroke: dabs at world positions (on the ground plane) between Begin and End.
    void BeginStroke();
    void Dab(f32 world_x, f32 world_z);
    void EndStroke();
    bool InStroke() const { return in_stroke_; }
    // Begin + Dab + End.
    void DabOnce(f32 world_x, f32 world_z);

    // Swaps in another terrain (a generated one): the undo history goes, every chunk is dirty, the revision moves on.
    void Replace(terrain::TerrainData data, terrain::TerrainSettings settings, terrain::SplatmapData splatmap);

    bool Undo();
    bool Redo();
    bool CanUndo() const { return !undo_.empty(); }
    bool CanRedo() const { return !redo_.empty(); }
    usize UndoDepth() const { return undo_.size(); }

    // Chunks (x, z) whose mesh changed since the last call (normals reach one sample out).
    std::set<std::pair<i32, i32>> TakeDirtyChunks();
    usize DirtyCount() const { return dirty_.size(); }

    // The terrain's extent on the ground plane.
    f32 WorldWidth() const;
    f32 WorldDepth() const;
    // The brush outline at a position, following the ground (a closed loop).
    std::vector<Vec3> CursorRing(f32 world_x, f32 world_z, u32 segments = 32) const;

private:
    struct Step {
        // Heights: the rectangle [x0, x1) x [z0, z1) of samples, before and after.
        u32 x0 = 0, z0 = 0, x1 = 0, z1 = 0;
        std::vector<f32> heights_before, heights_after;
        std::vector<u8> splat_before, splat_after; // the whole splatmap (paint strokes only)
    };
    void Apply(const Step& step, bool after);
    void MarkDirty(u32 x0, u32 z0, u32 x1, u32 z1);

    terrain::TerrainData data_;
    terrain::TerrainSettings settings_;
    terrain::SplatmapData splat_;
    std::filesystem::path path_;
    bool unsaved_ = false;
    bool in_stroke_ = false;
    std::vector<f32> stroke_heights_;
    std::vector<u8> stroke_splat_;
    std::vector<Step> undo_, redo_;
    std::set<std::pair<i32, i32>> dirty_;
    u64 revision_ = 1;
};

} // namespace aether::editor
