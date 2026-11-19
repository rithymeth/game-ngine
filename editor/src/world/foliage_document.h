#pragma once

#include "aether/terrain/foliage.h"

#include <filesystem>
#include <functional>
#include <random>
#include <string>
#include <vector>

namespace aether::editor {

// Foliage being painted (Phase 21 step 7): a layer and its density map, the
// paint or erase tool with its brush, new instances set onto the ground
// (through a height sampler), and undo per stroke.
//
// It is a foliage asset (.afoliage, Stage 6): Save writes the layer (types,
// settings, instances) and the density map, and Dirty() says whether what is
// on screen is on disk.
class FoliagePaintDocument {
public:
    using HeightSampler = std::function<f32(f32 x, f32 z)>;
    explicit FoliagePaintDocument(terrain::FoliageLayer layer, terrain::DensityMap density = {}, HeightSampler ground = {}, u32 seed = 1);

    // Reads an .afoliage file (terrain::FoliageFromJson); the undo history goes. On failure the document is unchanged.
    bool Load(const std::filesystem::path& file, std::string* error = nullptr);
    // Writes to the open file, or to `file` (save as). Ends a stroke in progress first.
    bool Save(std::string* error = nullptr);
    bool SaveAs(const std::filesystem::path& file, std::string* error = nullptr);
    // Treats what is there as saved: clears Dirty and the undo history, for a document seeded with a sample layer.
    void MarkSaved();
    // Where Save writes; empty for a foliage asset that has never been saved. SetPath names the file without reading it.
    void SetPath(const std::filesystem::path& file) { path_ = file; }
    const std::filesystem::path& Path() const { return path_; }
    std::string Name() const;
    bool Dirty() const { return dirty_; }

    const terrain::FoliageLayer& Layer() const { return layer_; }
    const terrain::DensityMap& Density() const { return density_; }
    u64 Revision() const { return revision_; }

    bool erase = false;
    f32 radius = 4.0f;

    void BeginStroke();
    void Dab(const Vec3& center);
    void EndStroke();
    void DabOnce(const Vec3& center);

    bool Undo();
    bool Redo();
    bool CanUndo() const { return !undo_.empty(); }
    bool CanRedo() const { return !redo_.empty(); }

    // Types and settings (undoable).
    usize AddType(const terrain::FoliageType& type);
    bool RemoveType(usize index); // its instances go, and later types' indices shift down
    bool SetType(usize index, const terrain::FoliageType& type);
    void SetDensity(f32 per_square_metre);
    void SetMaxInstances(u32 max);
    void Clear(); // every instance
    // Swaps in another layer (a scattered one) as one undo step.
    void ReplaceLayer(terrain::FoliageLayer layer);

    std::vector<usize> CountsByType() const;

private:
    void Record();
    void Changed() { ++revision_; }

    terrain::FoliageLayer layer_;
    terrain::DensityMap density_;
    std::filesystem::path path_;
    bool dirty_ = false;
    HeightSampler ground_;
    std::mt19937 rng_;
    bool in_stroke_ = false;
    terrain::FoliageLayer stroke_start_;
    std::vector<terrain::FoliageLayer> undo_, redo_;
    u64 revision_ = 1;
};

} // namespace aether::editor
