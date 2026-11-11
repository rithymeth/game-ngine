#pragma once

#include "aether/assets/procedural_mesh.h"
#include "aether/terrain/generator.h"
#include "aether/terrain/scatter.h"
#include "world/foliage_document.h"
#include "world/terrain_document.h"

#include <string>
#include <vector>

namespace aether::editor {

// The visual generation editor's model (item 4 of the procedural-world list): one document holding every setting of the terrain generator,
// the scatter rules, and which procedural mesh family each rule places; the results of the last generation (terrain, instances, a preview
// picture); undo for the settings; and the ways out: into the terrain and foliage editors, or baked to files a game can load. Headless, like
// the other documents: the panel draws it, tests drive it.

enum class WorldPreset : u8 { Temperate, Alpine, Desert, Islands };
inline constexpr u32 kWorldPresetCount = 4;
const char* WorldPresetName(WorldPreset preset);

// The mesh family a scatter rule places when baked: `kind`'s variants (0..variants-1) are made, and an instance picks one by its index. A rule
// with no family (grass, reeds and cacti have no generator yet) is scattered and shown in the preview but baked as nothing.
struct MeshSlot {
    bool enabled = false;
    assets::procedural::ProceduralKind kind = assets::procedural::ProceduralKind::Rock;
    u32 variants = 4;
    f32 scale = 1.0f; // times the instance's own scale
};

struct ProceduralWorldSettings {
    terrain::TerrainGenSettings terrain;
    std::vector<terrain::ScatterRule> rules;
    std::vector<MeshSlot> slots; // one per rule
    terrain::ScatterSettings scatter;
    u32 mesh_seed = 1;
};

ProceduralWorldSettings MakePreset(WorldPreset preset);

struct BakeOptions {
    std::string name = "Generated";
    u32 max_scene_instances = 5000; // the player draws one call per object; the rest are left out and counted
    u32 terrain_grid = 256;         // the terrain mesh's quads per side at most
    u32 terrain_tiles = 8;          // the ground cut into this many tiles a side, each its own draw the player can cull (1: one piece)
    bool terrain_collision = true;  // a static triangle-mesh collider over the ground (the very triangles drawn), so a character can walk on it; built only where the editor has physics
    f32 terrain_skirt = 6.0f;       // metres the terrain's edges are carried down below its lowest ground (0: an open edge)
    u32 water_texture = 512;        // the water's depth-tinted, foam-edged texture in texels (0: one flat colour)
    bool add_wind = true;           // a Wind entity in the baked scene, so the swaying plants blow the way the world is set to
    f32 wind_direction_degrees = 27.0f;
    f32 wind_strength = 1.0f;
    u32 terrain_texture = 1024;     // the ground texture's size in texels (0: no texture, the six flat-coloured surfaces instead)
};

struct BakeReport {
    bool ok = false;
    std::string error;
    std::vector<std::string> files;             // as written, relative to the content root
    std::string scene;                          // the .ascene, relative to the content root
    u32 instances_in_scene = 0;
    u32 instances_left_out = 0;                 // over max_scene_instances, or of a rule with no mesh
    std::vector<std::string> rules_without_mesh;
};

class ProceduralWorldDocument {
public:
    explicit ProceduralWorldDocument(ProceduralWorldSettings settings = MakePreset(WorldPreset::Temperate));

    const ProceduralWorldSettings& Settings() const { return settings_; }
    u64 Revision() const { return revision_; }

    // Editing: Checkpoint() saves the settings for undo (one call per edit, not per slider tick); Set() replaces them.
    void Checkpoint();
    void Set(const ProceduralWorldSettings& settings);
    void LoadPreset(WorldPreset preset); // one undo step
    void RandomizeSeed();                // a new terrain seed and scatter seed, one undo step
    bool Undo();
    bool Redo();
    bool CanUndo() const { return !undo_.empty(); }
    bool CanRedo() const { return !redo_.empty(); }
    void AddRule(const terrain::ScatterRule& rule); // one undo step; its slot starts with no mesh
    bool RemoveRule(usize index);
    bool DuplicateRule(usize index);

    // Generation: terrain, then scatter over it. The results are those of the settings at the last call; Stale() says they are older than the settings.
    void Generate();
    bool Generated() const { return generated_; }
    bool Stale() const { return !generated_ || generated_revision_ != revision_; }
    const terrain::GeneratedTerrain& Terrain() const { return terrain_; }
    const std::vector<terrain::ScatterInstance>& Instances() const { return instances_; }
    std::vector<usize> CountsByRule() const;
    double GenerateMilliseconds() const { return generate_ms_; }
    // The shaded biome picture with the scatter as dots, at most `max_side` samples a side (nearest). width/height of the result in `w`, `h`.
    std::vector<u8> Preview(u32 max_side, u32& w, u32& h) const;

    // One piece of a rule's mesh family (empty when the rule has no mesh).
    assets::procedural::ProceduralMesh BuildMesh(usize rule, u32 variant) const;
    // The terrain as one model: a primitive each for water, sand, grass, forest, rock and snow, by the biome under each triangle.
    static assets::procedural::ProceduralMesh BuildTerrainModel(const terrain::GeneratedTerrain& terrain, u32 max_grid);

    // The terrain as the ground (cut into `tiles_per_side` by `tiles_per_side` primitives, 1 for one piece), the skirt and the water (the skirt, when `skirt_depth` is above 0, carries the four edges down that far below the lowest ground): the ground, one surface textured by `texture_uri` (a path relative to the .glb; RenderTerrainTexture makes the picture) so
    // biome borders blend and rivers have banks, and the water, flat on every lake and the sea (rivers a hair above their carved beds).
    static assets::procedural::ProceduralMesh BuildTexturedTerrainModel(const terrain::GeneratedTerrain& terrain, u32 max_grid, const std::string& texture_uri, f32 skirt_depth = 0.0f,
                                                                           const std::string& water_texture_uri = "", const std::string& water_normal_uri = "", u32 tiles_per_side = 1);

    // The triangles a character walks on: the ground as `BuildTexturedTerrainModel` draws it (the same grid for the same `max_grid`, so the feet meet the
    // surface that is seen), without the skirt or the water. Positions are in the map's space, three indices a triangle, counter-clockwise from above.
    struct CollisionMesh {
        std::vector<Vec3> vertices;
        std::vector<u32> indices;
    };
    static CollisionMesh BuildTerrainCollisionMesh(const terrain::GeneratedTerrain& terrain, u32 max_grid);

    // Into the other editors: the terrain editor takes the heightmap, layers and splatmap (the sculpt tools carry on from it); the foliage editor takes
    // the instances as a layer with one type per rule (one undo step). False when nothing is generated.
    bool ApplyToTerrain(TerrainEditDocument& target, u32 splat_resolution = 256) const;
    bool ApplyToFoliage(FoliagePaintDocument& target) const;

    // To files under `content_root`/Generated/<name>/: the terrain model, a .glb per mesh variant used, and a scene with the terrain and every
    // scatter instance as model entities (Transform and ModelRenderer), which the player loads like any other.
    BakeReport Bake(const std::string& content_root, const BakeOptions& options) const;

private:
    void Changed();
    ProceduralWorldSettings settings_;
    std::vector<ProceduralWorldSettings> undo_, redo_;
    u64 revision_ = 1;
    bool generated_ = false;
    u64 generated_revision_ = 0;
    terrain::GeneratedTerrain terrain_;
    terrain::TerrainGenSettings generated_terrain_settings_;
    std::vector<terrain::ScatterInstance> instances_;
    double generate_ms_ = 0.0;
};

} // namespace aether::editor
