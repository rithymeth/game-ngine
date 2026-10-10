#pragma once

// @stability: experimental
//
// What a model would cook to (Phase 39 step 6, docs/design/UPGRADE_PLAN.md): triangles, LODs and meshlets summed
// over a glTF scene's meshes, for an editor to show beside the model without cooking it. It runs the same
// GenerateLods and BuildMeshlets the cooker does, so the numbers match the cooked mesh.

#include "aether/assets/gltf_loader.h"
#include "aether/cook/meshlet_cook.h"
#include "aether/core/base.h"

#include <string>
#include <vector>

namespace aether::cook {

struct MeshLodRow {
    u32 level = 0;
    u64 triangles = 0;
    f32 error = 0.0f;  // the worst across primitives, relative to each primitive's extent
    u64 meshlets = 0;
};

struct MeshSummary {
    bool ok = false;
    std::string message;           // why not, or a note ("2 skinned primitives are not included")
    u32 primitives = 0;
    u32 skipped_primitives = 0;    // skinned or empty: no LODs or meshlets
    u64 vertices = 0;
    std::vector<MeshLodRow> lods;  // level 0 first
};

struct MeshSummaryOptions {
    MeshletParams meshlets;
    LodParams lods;
    // A scene with more triangles than this is not processed (the summary is not ok), so selecting a huge model
    // can't freeze an editor.
    u64 max_triangles = 500000;
};

// Counts the scene's unique meshes once each (instances are not multiplied).
MeshSummary SummarizeMeshLods(const assets::GltfScene& scene, const MeshSummaryOptions& options = {});

} // namespace aether::cook
