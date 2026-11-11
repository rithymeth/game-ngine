#pragma once

#include "aether/assets/model_importer.h"
#include "aether/core/base.h"
#include "aether/math/math.h"

#include <string>
#include <vector>

namespace aether::assets::procedural {

// Procedural mesh generation: rocks, cliff faces, trees, buildings and modular environment pieces built from parameters and a seed, as the
// engine's own MeshData (positions, normals, UVs, tangents, indices; one primitive per material) so they cook, draw and save like any imported
// model. Every generator is deterministic: the same parameters and seed give the same bytes on any machine. GenerateVariant turns a kind and
// an index into a fresh, unique piece, so hundreds of different rocks or trees are a loop, not an art task.
//
// Conventions: metres, +Y up, the piece's base at y = 0 and centred on x = z = 0 (a cliff's back face is at z = 0 instead, so it stands against a
// slope), counter-clockwise triangles seen from outside. Solid pieces (rocks, pillars, buildings) are closed; foliage and bark are separate
// primitives so a game can give them their own shaders.

struct ProceduralMesh {
    MeshData mesh;                      // one primitive per material actually used; MeshPrimitiveData::material indexes `materials`
    std::vector<MaterialData> materials;
    u32 TriangleCount() const;
    u32 VertexCount() const;
};

enum class Shading : u8 { Smooth, Flat };
enum class UvMode : u8 { Given, BoxProjected };

// Collects triangles, then makes a MeshPrimitiveData. Positions that coincide (to a millimetre) share one smooth normal, so a seam in the UVs
// does not show as a crease; Flat unwelds every triangle to its face normal. Tangents come from the UVs. Public so a game can compose its own
// pieces from the generators' parts.
class MeshBuilder {
public:
    u32 Vertex(const Vec3& position, f32 u = 0.0f, f32 v = 0.0f);
    void Triangle(u32 a, u32 b, u32 c);
    void Quad(u32 a, u32 b, u32 c, u32 d); // counter-clockwise a, b, c, d: triangles (a, b, c) and (a, c, d)
    // An axis-aligned box as six flat quads (24 vertices) with a texture repeating once per metre.
    void Box(const Vec3& min, const Vec3& max);
    // A tube along `points` with `radii[i]` at each (at least two points), `sides` around (at least 3), UV v along the length in metres. Open at
    // both ends; `cap_start` / `cap_end` close them with a fan.
    void Tube(const std::vector<Vec3>& points, const std::vector<f32>& radii, u32 sides, bool cap_start, bool cap_end);
    // A unit icosphere with `subdivisions` splits (0 is 20 triangles), welded; returns the vertex indices' range start. Positions are on the
    // unit sphere; the caller moves them.
    u32 IcoSphere(u32 subdivisions);
    u32 VertexCount() const { return static_cast<u32>(positions_.size()); }
    Vec3& Position(u32 i) { return positions_[i]; }
    const Vec3& Position(u32 i) const { return positions_[i]; }
    bool Empty() const { return indices_.empty(); }
    // Adds everything of `other`, moved by `offset`.
    void Append(const MeshBuilder& other, const Vec3& offset = Vec3(0, 0, 0));
    MeshPrimitiveData Finish(Shading shading, UvMode uv_mode, i32 material) const;

private:
    std::vector<Vec3> positions_;
    std::vector<f32> uvs_; // u, v per vertex
    std::vector<u32> indices_;
};

// ---------------------------------------------------------------------------------------------------------------------------------------
// Generators.

struct RockParams {
    f32 size = 1.0f;          // roughly the diameter, metres
    f32 elongation = 0.3f;    // 0 a ball, 1 stretched up to twice as long on one axis
    u32 subdivisions = 3;     // 2 to 4: 320, 1280 or 5120 triangles
    f32 roughness = 0.25f;    // how far the noise pushes the surface, as a share of the radius
    u32 cuts = 5;             // planar cuts that give the angular, split-stone look (0 keeps it lumpy)
    f32 flatten_bottom = 0.45f; // 0..1: how much of the lower part is cut flat so it sits on the ground
    Shading shading = Shading::Smooth;
    Vec3 color{0.42f, 0.40f, 0.38f};
};
ProceduralMesh GenerateRock(const RockParams& params, u32 seed);

struct CliffParams {
    f32 width = 12.0f, height = 8.0f, depth = 3.0f; // the slab's extent; the face bulges up to `depth` from the back plane at z = 0
    u32 columns = 48, rows = 32;                    // the face's grid
    u32 strata = 6;                                 // horizontal ledges up the face
    f32 roughness = 0.35f;
    f32 overhang = 0.25f;                           // how far each stratum can stand out past the one below, as a share of depth
    Vec3 color{0.45f, 0.40f, 0.34f};
};
ProceduralMesh GenerateCliff(const CliffParams& params, u32 seed);

enum class TreeSpecies : u8 { Pine, Broadleaf, Dead };
struct TreeParams {
    TreeSpecies species = TreeSpecies::Broadleaf;
    f32 height = 8.0f;          // metres, ground to the tip
    f32 trunk_radius = 0.28f;
    u32 sides = 8;              // trunk faces
    u32 branch_levels = 3;      // broadleaf and dead: how many times branches split
    f32 foliage = 1.0f;         // 0 bare to 1.5 dense
    f32 lean = 0.06f;           // how far the trunk sways, as a share of the height
    u32 foliage_detail = 1;     // icosphere splits per leaf mass (broadleaf), 0 to 2
    Vec3 bark_color{0.30f, 0.21f, 0.14f};
    Vec3 leaf_color{0.22f, 0.45f, 0.16f};
};
ProceduralMesh GenerateTree(const TreeParams& params, u32 seed);

enum class RoofType : u8 { Gable, Flat };
struct BuildingParams {
    u32 floors = 2;
    u32 bays_x = 3, bays_z = 2; // windows' worth of wall along the front and the side
    f32 bay_size = 3.0f;        // metres per bay
    f32 floor_height = 3.0f;
    RoofType roof = RoofType::Gable;
    bool chimney = true;
    bool door = true;           // in the middle bay of the front (+z) wall, ground floor
    Vec3 wall_color{0.78f, 0.72f, 0.62f};
    Vec3 roof_color{0.45f, 0.20f, 0.15f};
};
ProceduralMesh GenerateBuilding(const BuildingParams& params, u32 seed);

enum class ModularKind : u8 { Wall, Pillar, Arch, Stairs };
struct ModularParams {
    ModularKind kind = ModularKind::Wall;
    f32 width = 4.0f, height = 3.0f, depth = 0.4f; // Wall: the slab; Pillar: width is the diameter; Arch: the opening and posts; Stairs: width, rise, run
    u32 segments = 8;      // Pillar: sides; Arch: pieces in the curve; Stairs: steps
    bool window = false;   // Wall: a window opening through the middle
    Vec3 color{0.62f, 0.60f, 0.56f};
};
ProceduralMesh GenerateModular(const ModularParams& params, u32 seed);

// ---------------------------------------------------------------------------------------------------------------------------------------
// Variants.

enum class ProceduralKind : u8 { Rock, Cliff, PineTree, BroadleafTree, DeadTree, House, Tower, Wall, Pillar, Arch, Stairs };
inline constexpr u32 kProceduralKindCount = 11;
const char* ProceduralKindName(ProceduralKind kind);

// The `variant`th member of the family for `kind` under `seed`: parameters drawn from the kind's own ranges (sizes, proportions, colours) and
// a mesh seed from the same stream. Different variants differ in shape, not only in scale; the same (kind, variant, seed) is the same piece.
ProceduralMesh GenerateVariant(ProceduralKind kind, u32 variant, u32 seed);

// ---------------------------------------------------------------------------------------------------------------------------------------
// Checks and export.

struct MeshStats {
    u32 vertices = 0, triangles = 0;
    Vec3 bounds_min{0, 0, 0}, bounds_max{0, 0, 0};
    f32 volume = 0.0f; // signed (positive when the triangles wind outward); meaningful for closed pieces
    u32 open_edges = 0; // edges (over positions welded to a millimetre) that belong to one triangle: 0 for a closed piece
};
MeshStats ComputeStats(const MeshData& mesh);

// False, with the first reason in `error`, when a primitive has indices out of range, a count that is not a multiple of three, non-finite
// values, a normal that is not unit length or a tangent that is not a unit xyz with w = +-1, or a material index outside `material_count`.
bool ValidateMesh(const ProceduralMesh& mesh, std::string* error);

// A .glb (binary glTF 2.0): one mesh with the primitives, their materials as metallic-roughness factors, one node. Loads with LoadGltfFromMemory.
std::vector<u8> EncodeGlb(const ProceduralMesh& mesh);

} // namespace aether::assets::procedural
