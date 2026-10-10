#pragma once

// @stability: experimental
//
// Cooked meshes (Phase 39 step 6a, docs/design/UPGRADE_PLAN.md): the packaged form of a model's Mesh sub-asset.
// Each primitive keeps its vertices once, a chain of LOD index buffers (level 0 is the source), and the meshlets
// of every level (see meshlet_cook.h). The container is "AMSC" v1, little-endian, with a CRC-32 trailer; the
// reader checks every count against the bytes that remain, so hostile input fails cleanly.
//
// Skinned primitives keep their source indices only (no LODs or meshlets) and add a warning: v1 does not carry
// joints and weights. Consuming this on the GPU (virtual geometry) is the rest of step 39.6.

#include "aether/assets/model_importer.h"
#include "aether/cook/meshlet_cook.h"
#include "aether/core/base.h"

#include <span>
#include <string>
#include <vector>

namespace aether::cook {

struct MeshCookSettings {
    MeshletParams meshlets;
    LodParams lods;
    bool build_lods = true;
    bool build_meshlets = true;
};

struct CookedMeshLod {
    std::vector<u32> indices;
    f32 error = 0.0f; // relative to the mesh's extent; 0 for level 0
    MeshletMesh meshlets;
};

struct CookedMeshPrimitive {
    std::vector<assets::MeshVertex> vertices;
    i32 material = -1;
    Vec3 bounds_min;
    Vec3 bounds_max;
    std::vector<CookedMeshLod> lods; // lods[0] is the source mesh
};

struct CookedMesh {
    std::vector<CookedMeshPrimitive> primitives;
    std::vector<std::string> warnings; // not saved
};

// False (with a reason) when a primitive's indices are out of range or the settings are invalid.
bool CookMesh(const assets::MeshData& mesh, const MeshCookSettings& settings, CookedMesh& out,
              std::string* error = nullptr);

// Deterministic: the same input gives the same bytes.
std::vector<u8> SaveAmesh(const CookedMesh& mesh);
[[nodiscard]] bool LoadAmesh(std::span<const u8> bytes, CookedMesh& out, std::string* error = nullptr);

} // namespace aether::cook
