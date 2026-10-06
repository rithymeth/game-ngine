#pragma once

#include "aether/terrain/foliage.h"

#include <functional>
#include <random>
#include <vector>

namespace aether::editor {

// Foliage being painted (Phase 21 step 7): a layer and its density map, the
// paint or erase tool with its brush, new instances set onto the ground
// (through a height sampler), and undo per stroke.
class FoliagePaintDocument {
public:
    using HeightSampler = std::function<f32(f32 x, f32 z)>;
    explicit FoliagePaintDocument(terrain::FoliageLayer layer, terrain::DensityMap density = {}, HeightSampler ground = {}, u32 seed = 1);

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

    std::vector<usize> CountsByType() const;

private:
    void Record();
    void Changed() { ++revision_; }

    terrain::FoliageLayer layer_;
    terrain::DensityMap density_;
    HeightSampler ground_;
    std::mt19937 rng_;
    bool in_stroke_ = false;
    terrain::FoliageLayer stroke_start_;
    std::vector<terrain::FoliageLayer> undo_, redo_;
    u64 revision_ = 1;
};

} // namespace aether::editor
