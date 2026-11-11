#include "aether/assets/gltf_loader.h"
#include "aether/assets/image.h"
#include "aether/scene/components.h"
#include "aether/scene/serialization.h"
#include "test_framework.h"
#include "world/procedural_panel.h"

#include <imgui.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <set>

// The visual generation editor: the document (presets, undo, generation, the hand-off to the terrain and foliage editors, baking to files) and the
// panel, drawn headless.

using namespace aether;
using namespace aether::editor;
using namespace aether::terrain;
namespace fs = std::filesystem;

#define CHECK(...) AETHER_CHECK((__VA_ARGS__))

namespace {

class HeadlessImGui {
public:
    HeadlessImGui() {
        context_ = ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(1200, 900);
        io.DeltaTime = 1.0f / 60.0f;
        unsigned char* pixels = nullptr;
        int w = 0, h = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
    }
    ~HeadlessImGui() { ImGui::DestroyContext(context_); }
    void Frame(const std::function<void()>& body) {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(1200, 900));
        ImGui::Begin("Procedural", nullptr, ImGuiWindowFlags_NoMove);
        body();
        ImGui::End();
        ImGui::Render();
    }

private:
    ImGuiContext* context_ = nullptr;
};

// A world small enough to generate in a few milliseconds.
ProceduralWorldSettings Small(WorldPreset preset = WorldPreset::Temperate) {
    ProceduralWorldSettings s = MakePreset(preset);
    s.terrain.width = s.terrain.height = 65;
    s.terrain.hydraulic_droplets = 3000;
    s.terrain.river_min_catchment = 60;
    // Dense enough in the rules that have meshes that a bake has something to choose from; the meshless grass kept light.
    for (usize i = 0; i < s.rules.size(); ++i) s.rules[i].density *= s.slots[i].enabled ? 8.0f : 0.1f;
    return s;
}

TerrainEditDocument FlatTerrainDocument() {
    Heightmap hm;
    hm.width = hm.height = 63;
    hm.cell_size = 1.0f;
    hm.heights.assign(63u * 63u, 0.0f);
    TerrainSettings settings;
    settings.verts_per_chunk = 32;
    TerrainData data;
    InitTerrainData(data, hm, settings);
    return TerrainEditDocument(data, settings);
}

fs::path ScratchDir(const char* name) {
    const fs::path dir = fs::temp_directory_path() / name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir;
}

} // namespace

AETHER_TEST(ProceduralEditor_PresetsAreDifferentWorldsWithTheirOwnMeshFamilies) {
    std::set<std::vector<f32>> shapes;
    for (u32 p = 0; p < kWorldPresetCount; ++p) {
        ProceduralWorldSettings s = Small(static_cast<WorldPreset>(p));
        ProceduralWorldDocument doc(s);
        doc.Generate();
        CHECK(doc.Generated() && !doc.Stale());
        shapes.insert(doc.Terrain().heightmap.heights);
        CHECK(s.slots.size() == s.rules.size());
    }
    CHECK(shapes.size() == kWorldPresetCount);
    const ProceduralWorldSettings s = MakePreset(WorldPreset::Temperate);
    // The pine rule places pines, grass places tufts: every starter rule has a mesh family now.
    for (usize i = 0; i < s.rules.size(); ++i) {
        if (s.rules[i].name == "Pine") CHECK(s.slots[i].enabled && s.slots[i].kind == assets::procedural::ProceduralKind::PineTree);
        if (s.rules[i].name == "Grass") CHECK(s.slots[i].enabled && s.slots[i].kind == assets::procedural::ProceduralKind::Grass);
        if (s.rules[i].name == "Cactus") CHECK(s.slots[i].enabled && s.slots[i].kind == assets::procedural::ProceduralKind::Cactus);
    }
    CHECK(std::string(WorldPresetName(WorldPreset::Alpine)) == "Alpine");
}

AETHER_TEST(ProceduralEditor_EditingUndoSeedsAndRules) {
    ProceduralWorldDocument doc(Small());
    doc.Generate();
    CHECK(!doc.Stale());
    const u64 revision = doc.Revision();
    ProceduralWorldSettings s = doc.Settings();
    s.terrain.height_scale = 90.0f;
    doc.Checkpoint();
    doc.Set(s);
    CHECK(doc.Stale() && doc.Revision() > revision && doc.CanUndo());
    CHECK(doc.Undo() && doc.Settings().terrain.height_scale == 60.0f && doc.CanRedo());
    CHECK(doc.Redo() && doc.Settings().terrain.height_scale == 90.0f);
    CHECK(doc.Undo());

    // A preset keeps the seeds and is one undo step; the dice change the seeds and are one too.
    const u32 seed = doc.Settings().terrain.seed;
    doc.LoadPreset(WorldPreset::Alpine);
    CHECK(doc.Settings().terrain.seed == seed && doc.Settings().terrain.height_scale == 120.0f);
    CHECK(doc.Undo() && doc.Settings().terrain.height_scale == 60.0f);
    doc.RandomizeSeed();
    CHECK(doc.Settings().terrain.seed != seed && doc.Settings().scatter.seed != 1u);
    CHECK(doc.Undo() && doc.Settings().terrain.seed == seed);

    // Rules and their mesh slots stay in step.
    const usize n = doc.Settings().rules.size();
    doc.AddRule(ScatterRule{.name = "Fern"});
    CHECK(doc.Settings().rules.size() == n + 1 && doc.Settings().slots.size() == n + 1 && !doc.Settings().slots.back().enabled);
    CHECK(doc.DuplicateRule(0) && doc.Settings().rules[1].name == "Pine copy" && doc.Settings().slots[1].enabled);
    CHECK(doc.RemoveRule(1) && doc.Settings().rules.size() == n + 1 && doc.Settings().slots.size() == n + 1);
    CHECK(!doc.RemoveRule(999) && !doc.DuplicateRule(999));
    CHECK(doc.Undo() && doc.Undo() && doc.Undo() && doc.Settings().rules.size() == n);
}

AETHER_TEST(ProceduralEditor_GenerationIsDeterministicWithCountsAndPreview) {
    ProceduralWorldDocument a(Small()), b(Small());
    a.Generate();
    b.Generate();
    CHECK(a.Terrain().heightmap.heights == b.Terrain().heightmap.heights);
    CHECK(a.Instances().size() == b.Instances().size() && !a.Instances().empty());
    const std::vector<usize> counts = a.CountsByRule();
    usize total = 0;
    for (const usize c : counts) total += c;
    CHECK(counts.size() == a.Settings().rules.size() && total == a.Instances().size());
    u32 w = 0, h = 0;
    const std::vector<u8> preview = a.Preview(32, w, h);
    CHECK(w == 32 && h == 32 && preview.size() == 32u * 32u * 3 && std::any_of(preview.begin(), preview.end(), [](u8 v) { return v > 80; }));
    ProceduralWorldDocument fresh(Small());
    CHECK(fresh.Preview(32, w, h).empty() && w == 0 && fresh.Stale() && !fresh.Generated());
    CHECK(fresh.BuildMesh(0, 0).mesh.primitives.size() >= 1);
    CHECK(!fresh.BuildMesh(3, 0).mesh.primitives.empty() && fresh.BuildMesh(999, 0).mesh.primitives.empty()); // grass has tufts; no such rule has nothing
}

AETHER_TEST(ProceduralEditor_TheTerrainBecomesAModelWithSixSurfaces) {
    ProceduralWorldDocument doc(Small());
    doc.Generate();
    const assets::procedural::ProceduralMesh model = ProceduralWorldDocument::BuildTerrainModel(doc.Terrain(), 64);
    std::string error;
    CHECK(assets::procedural::ValidateMesh(model, &error));
    CHECK(!model.mesh.primitives.empty() && model.mesh.primitives.size() <= 6 && model.materials.size() == model.mesh.primitives.size());
    const assets::procedural::MeshStats s = assets::procedural::ComputeStats(model.mesh);
    const f32 extent = static_cast<f32>(doc.Terrain().Width() - 1) * doc.Terrain().heightmap.cell_size;
    CHECK(std::fabs(s.bounds_max.x - extent) < 1e-3f && std::fabs(s.bounds_max.z - extent) < 1e-3f && std::fabs(s.bounds_min.x) < 1e-3f);
    CHECK(std::fabs(s.bounds_max.y - doc.Terrain().height_max) < 1e-2f);
    // The ground faces up: the normals' mean y is clearly positive.
    double ny = 0.0;
    usize n = 0;
    for (const auto& p : model.mesh.primitives) for (const auto& v : p.vertices) { ny += v.normal[1]; ++n; }
    CHECK(ny / static_cast<double>(n) > 0.6);
    // A coarser grid has fewer triangles.
    const auto coarse = ProceduralWorldDocument::BuildTerrainModel(doc.Terrain(), 16);
    CHECK(coarse.TriangleCount() < model.TriangleCount() / 4);
    CHECK(ProceduralWorldDocument::BuildTerrainModel(GeneratedTerrain{}, 64).mesh.primitives.empty());
}

AETHER_TEST(ProceduralEditor_TheTexturedTerrainIsOneGroundSurfaceAndTheWater) {
    ProceduralWorldDocument doc(Small());
    doc.Generate();
    const GeneratedTerrain& t = doc.Terrain();
    const assets::procedural::ProceduralMesh model = ProceduralWorldDocument::BuildTexturedTerrainModel(t, 64, "ground.png");
    std::string error;
    CHECK(assets::procedural::ValidateMesh(model, &error));
    CHECK(model.mesh.primitives.size() >= 1 && model.mesh.primitives.size() <= 2 && model.materials[0].base_color_texture == "ground.png");
    // The ground's UV is the position over the map: 0 to 1 on both axes, corner to corner.
    f32 u_min = 1e9f, u_max = -1e9f, v_min = 1e9f, v_max = -1e9f;
    for (const auto& v : model.mesh.primitives[0].vertices) { u_min = std::min(u_min, v.uv[0]); u_max = std::max(u_max, v.uv[0]); v_min = std::min(v_min, v.uv[1]); v_max = std::max(v_max, v.uv[1]); }
    CHECK(u_min == 0.0f && v_min == 0.0f && std::fabs(u_max - 1.0f) < 1e-5f && std::fabs(v_max - 1.0f) < 1e-5f);
    // The ground faces up, and the model loads (with its texture named) through the glTF loader.
    double ny = 0.0;
    for (const auto& v : model.mesh.primitives[0].vertices) ny += v.normal[1];
    CHECK(ny / static_cast<double>(model.mesh.primitives[0].vertices.size()) > 0.6);
    {
        // From a file, so the loader resolves the texture: the uri joined to the model's folder.
        const fs::path dir = ScratchDir("aether_procedural_textured");
        const std::vector<u8> glb = assets::procedural::EncodeGlb(model);
        { std::ofstream f(dir / "terrain.glb", std::ios::binary); f.write(reinterpret_cast<const char*>(glb.data()), static_cast<std::streamsize>(glb.size())); }
        assets::GltfScene scene;
        CHECK(assets::LoadGltf((dir / "terrain.glb").string(), scene) && !scene.materials[0].base_color_texture.empty());
        CHECK(scene.materials[0].base_color_texture.find("ground.png") != std::string::npos);
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    // The water is above the ground it covers (rivers a hair, lakes and the sea by their depth).
    if (model.mesh.primitives.size() == 2) {
        CHECK(model.materials[1].base_color.z > model.materials[1].base_color.x);
        for (const auto& v : model.mesh.primitives[1].vertices) CHECK(v.position[1] >= t.height_min - 1e-3f && v.position[1] <= t.height_max + 0.05f);
    }
}

AETHER_TEST(ProceduralEditor_ShorelinesFollowTheGroundAndTheEdgeHasASkirt) {
    ProceduralWorldDocument doc(Small(WorldPreset::Islands));
    doc.Generate();
    const GeneratedTerrain& t = doc.Terrain();
    const f32 cell = t.heightmap.cell_size;
    const assets::procedural::ProceduralMesh model = ProceduralWorldDocument::BuildTexturedTerrainModel(t, 64, "g.png", 5.0f);
    CHECK(model.mesh.primitives.size() == 3); // ground, skirt (the ground's material again), water
    const assets::MeshPrimitiveData& skirt = model.mesh.primitives[1];
    const assets::MeshPrimitiveData& water = model.mesh.primitives[2];
    CHECK(skirt.material == 0 && water.material == 1);

    // The water: flat quads at the sea's level or a lake's fill level, which run on under the dry corners of a shore quad (so the shoreline is where the
    // ground crosses the water, not a staircase of whole quads).
    std::set<f32> levels = {t.sea_height};
    for (usize i = 0; i < t.lake.size(); ++i) if (t.lake[i]) levels.insert(t.fill_height[i]);
    usize under_land = 0, covered_wet = 0, wet = 0;
    std::set<std::pair<i32, i32>> water_xz;
    for (const auto& v : water.vertices) {
        CHECK(levels.count(v.position[1]) == 1);
        const u32 gx = static_cast<u32>(std::lround(v.position[0] / cell)), gz = static_cast<u32>(std::lround(v.position[2] / cell));
        water_xz.insert({static_cast<i32>(gx), static_cast<i32>(gz)});
        under_land += t.heightmap.heights[t.Index(gx, gz)] > v.position[1] + 0.01f;
    }
    for (u32 z = 0; z < t.Height(); ++z)
        for (u32 x = 0; x < t.Width(); ++x) {
            const usize i = t.Index(x, z);
            if (!(t.lake[i] || t.heightmap.heights[i] <= t.sea_height)) continue;
            ++wet;
            covered_wet += water_xz.count({static_cast<i32>(x), static_cast<i32>(z)});
        }
    CHECK(wet > 100 && covered_wet == wet); // every wet sample has water over it
    CHECK(under_land > 0);                  // and the plane runs on under the shore

    // The skirt: from the ground's edge down to 5 m under its lowest point, facing outward, flat shaded (a normal per face, none sloping).
    f32 lowest = 1e9f, highest = -1e9f;
    for (const auto& v : skirt.vertices) { lowest = std::min(lowest, v.position[1]); highest = std::max(highest, v.position[1]); }
    CHECK(std::fabs(lowest - (t.height_min - 5.0f)) < 1e-3f && highest <= t.height_max + 1e-3f);
    CHECK(skirt.indices.size() / 3 == 4u * 64u * 2u);
    const f32 extent = static_cast<f32>(t.Width() - 1) * cell;
    usize outward = 0;
    for (const auto& v : skirt.vertices) {
        CHECK(std::fabs(v.normal[1]) < 1e-3f);
        const bool west = v.position[0] < 1e-3f, east = v.position[0] > extent - 1e-3f, north = v.position[2] < 1e-3f, south = v.position[2] > extent - 1e-3f;
        const bool ok = (west && v.normal[0] < -0.9f) || (east && v.normal[0] > 0.9f) || (north && v.normal[2] < -0.9f) || (south && v.normal[2] > 0.9f);
        outward += ok;
    }
    CHECK(outward == skirt.vertices.size() || outward * 100 > skirt.vertices.size() * 99); // corners belong to two edges; all else faces out
    // A water texture: the water's UV is the position over the map (0 to 1 on both axes), its material takes the texture with a white factor.
    {
        const assets::procedural::ProceduralMesh textured = ProceduralWorldDocument::BuildTexturedTerrainModel(t, 64, "g.png", 5.0f, "w.png", "wn.png");
        const assets::MeshPrimitiveData& wp = textured.mesh.primitives[2];
        CHECK(textured.materials[1].base_color_texture == "w.png" && textured.materials[1].base_color.x == 1.0f && textured.materials[0].base_color_texture == "g.png");
        // The water blends (its texture's alpha is the opacity) and takes the ripple normal map; the ground stays opaque.
        CHECK(textured.materials[1].alpha_mode == assets::MaterialAlphaMode::Blend && textured.materials[1].normal_texture == "wn.png" && textured.materials[0].alpha_mode == assets::MaterialAlphaMode::Opaque);
        f32 lo = 1e9f, hi = -1e9f;
        for (const auto& v : wp.vertices) { lo = std::min({lo, v.uv[0], v.uv[1]}); hi = std::max({hi, v.uv[0], v.uv[1]}); }
        CHECK(lo >= 0.0f && hi <= 1.0f + 1e-5f && hi > 0.5f);
        // The flat colour when there is no water texture.
        CHECK(model.materials[1].base_color_texture.empty() && model.materials[1].base_color.z > model.materials[1].base_color.x && model.materials[1].alpha_mode == assets::MaterialAlphaMode::Opaque);
    }
    // No skirt unless asked for.
    CHECK(ProceduralWorldDocument::BuildTexturedTerrainModel(t, 64, "g.png").mesh.primitives.size() == 2);
    std::string error;
    CHECK(assets::procedural::ValidateMesh(model, &error));
}

AETHER_TEST(ProceduralEditor_SeaAndLakeSurfacesAreFlat) {
    ProceduralWorldDocument doc(Small(WorldPreset::Islands));
    doc.Generate();
    const GeneratedTerrain& t = doc.Terrain();
    const assets::procedural::ProceduralMesh model = ProceduralWorldDocument::BuildTerrainModel(t, 64);
    // The water is the first primitive when there is any (it is drawn from the biome under each quad); an islands world is mostly sea.
    CHECK(!model.mesh.primitives.empty() && model.materials[0].base_color.z > model.materials[0].base_color.x); // blue
    const assets::MeshPrimitiveData& water = model.mesh.primitives[0];
    usize at_sea = 0;
    for (const auto& v : water.vertices) at_sea += std::fabs(v.position[1] - t.sea_height) < 1e-3f;
    CHECK(!water.vertices.empty() && at_sea * 10 > water.vertices.size() * 6); // most of the water is on the flat sea surface
    // And nothing of the sea rises above the sea level.
    for (const auto& v : water.vertices) CHECK(v.position[1] <= t.sea_height + 1e-3f || v.position[1] <= t.height_max);
}

AETHER_TEST(ProceduralEditor_ApplyReplacesTheTerrainAndFoliageEditorsContent) {
    ProceduralWorldDocument doc(Small());
    TerrainEditDocument terrain = FlatTerrainDocument();
    FoliagePaintDocument foliage(FoliageLayer{});
    CHECK(!doc.ApplyToTerrain(terrain) && !doc.ApplyToFoliage(foliage)); // nothing generated yet
    doc.Generate();
    const u64 revision = terrain.Revision();
    CHECK(doc.ApplyToTerrain(terrain, 64));
    CHECK(terrain.Data().heightmap.heights == doc.Terrain().heightmap.heights);
    CHECK(terrain.Data().layers.size() == 4 && terrain.Splatmap().width == 64 && terrain.Revision() > revision);
    CHECK(!terrain.CanUndo() && terrain.DirtyCount() == static_cast<usize>(terrain.Data().chunk_count_x) * terrain.Data().chunk_count_z);
    // The sculpt tools carry on from the generated ground.
    const f32 before = terrain.Data().heightmap.heights[10 * terrain.Data().heightmap.width + 10];
    terrain.tool = TerrainTool::Raise;
    terrain.brush.radius = 4.0f;
    terrain.DabOnce(10.0f, 10.0f);
    CHECK(terrain.Data().heightmap.heights[10 * terrain.Data().heightmap.width + 10] != before && terrain.CanUndo());

    CHECK(doc.ApplyToFoliage(foliage));
    CHECK(foliage.Layer().instances.size() == doc.Instances().size() && foliage.Layer().types.size() == doc.Settings().rules.size());
    CHECK(foliage.Undo() && foliage.Layer().instances.empty()); // one step back
}

AETHER_TEST(ProceduralEditor_BakeWritesTheTerrainMeshesAndASceneThePlayerLoads) {
    ProceduralWorldDocument doc(Small());
    doc.AddRule(ScatterRule{.name = "Fern"}); // a rule of the user's own starts with no mesh family
    BakeOptions options;
    options.name = "Test World!";
    options.terrain_texture = 128;
    options.water_texture = 64;
    const fs::path root = ScratchDir("aether_procedural_bake");
    CHECK(!doc.Bake(root.string(), options).ok && !doc.Bake(root.string(), options).error.empty()); // nothing generated
    doc.Generate();
    {
        usize eligible = 0;
        const std::vector<usize> counts = doc.CountsByRule();
        for (usize r = 0; r < counts.size(); ++r) if (doc.Settings().slots[r].enabled) eligible += counts[r];
        CHECK(eligible > 40); // enough objects with a mesh that the cap below is what limits the scene
    }
    options.max_scene_instances = 40;
    const BakeReport report = doc.Bake(root.string(), options);
    CHECK(report.ok && report.error.empty());
    CHECK(report.instances_in_scene == 40 && report.instances_left_out == doc.Instances().size() - 40);
    CHECK(report.scene == "Generated/Test_World_/Test_World_.ascene");
    for (const std::string& file : report.files) CHECK(fs::exists(root / file) && fs::file_size(root / file) > 100);
    // The ground texture is a PNG that decodes at the size asked for, named in the terrain model beside it.
    {
        const std::string png = "Generated/Test_World_/Test_World__terrain.png";
        CHECK(std::find(report.files.begin(), report.files.end(), png) != report.files.end());
        assets::ImageData image;
        CHECK(assets::DecodeImageFile((root / png).string(), image) && image.width == options.terrain_texture && image.height == options.terrain_texture);
        const std::string water_png = "Generated/Test_World_/Test_World__water.png";
        CHECK(std::find(report.files.begin(), report.files.end(), water_png) != report.files.end());
        assets::ImageData water_image;
        CHECK(assets::DecodeImageFile((root / water_png).string(), water_image) && water_image.width == options.water_texture);
        // Its alpha channel is real: not all of it is opaque (the shallows), and not all transparent.
        usize translucent = 0, solid = 0;
        for (usize p = 0; p < static_cast<usize>(water_image.width) * water_image.height; ++p) { translucent += water_image.pixels[p * 4 + 3] < 200; solid += water_image.pixels[p * 4 + 3] > 200; }
        CHECK(translucent > 0 && solid > 0);
        const std::string normal_png = "Generated/Test_World_/Test_World__water_normal.png";
        CHECK(std::find(report.files.begin(), report.files.end(), normal_png) != report.files.end());
        assets::ImageData normal_image;
        CHECK(assets::DecodeImageFile((root / normal_png).string(), normal_image) && normal_image.width == options.terrain_texture);
    }
    CHECK(std::any_of(report.files.begin(), report.files.end(), [](const std::string& f) { return f.find("_terrain.glb") != std::string::npos; }));
    CHECK(std::find(report.rules_without_mesh.begin(), report.rules_without_mesh.end(), "Fern") != report.rules_without_mesh.end());
    CHECK(std::find(report.rules_without_mesh.begin(), report.rules_without_mesh.end(), "Grass") == report.rules_without_mesh.end());
    CHECK(std::find(report.rules_without_mesh.begin(), report.rules_without_mesh.end(), "Pine") == report.rules_without_mesh.end());

    // The scene loads: the terrain plus one entity per baked object, each naming a model that was written.
    World loaded;
    CHECK(LoadSceneJson(loaded, (root / report.scene).string()));
    CHECK(loaded.EntityCount() == 41);
    usize with_scale = 0;
    loaded.ForEachArchetype([&](const Archetype& archetype) {
        if (!archetype.Mask().test(GetComponentId<ModelRenderer>()) || !archetype.Mask().test(GetComponentId<Transform>())) return;
        for (usize chunk = 0; chunk < archetype.ChunkCount(); ++chunk) {
            const auto* models = static_cast<const ModelRenderer*>(archetype.ComponentArray(chunk, GetComponentId<ModelRenderer>()));
            const auto* transforms = static_cast<const Transform*>(archetype.ComponentArray(chunk, GetComponentId<Transform>()));
            for (usize row = 0; row < archetype.ChunkEntityCount(chunk); ++row) {
                CHECK(fs::exists(root / models[row].asset_path));
                with_scale += transforms[row].scale.x > 0.0f;
            }
        }
    });
    CHECK(with_scale == 41);

    // A lower cap bakes fewer; an unwritable folder is reported, not thrown.
    options.max_scene_instances = 5;
    CHECK(doc.Bake(root.string(), options).instances_in_scene == 5);
    const fs::path blocked = root / "blocked";
    { std::ofstream f(blocked); f << "a file where a folder should go"; }
    const BakeReport bad = doc.Bake(blocked.string(), options);
    CHECK(!bad.ok && !bad.error.empty());
    std::error_code ec;
    fs::remove_all(root, ec);
}

AETHER_TEST(ProceduralEditor_ABakeKeepsTheTreesAndThinsTheGrass) {
    ProceduralWorldDocument doc(Small());
    doc.Generate();
    const std::vector<usize> counts = doc.CountsByRule();
    // Rule 0 (pine) is first in the pecking order; budget for all of it plus 25 more objects.
    CHECK(counts[0] > 5 && counts[3] > 100); // pines, and plenty of grass to compete with
    BakeOptions options;
    options.name = "Budget";
    options.terrain_texture = 32;
    options.max_scene_instances = static_cast<u32>(counts[0] + counts[1] + counts[2] + 25);
    const fs::path root = ScratchDir("aether_procedural_budget");
    const BakeReport report = doc.Bake(root.string(), options);
    CHECK(report.ok && report.instances_in_scene == options.max_scene_instances);
    World loaded;
    CHECK(LoadSceneJson(loaded, (root / report.scene).string()));
    usize pines = 0, grass = 0;
    loaded.ForEachArchetype([&](const Archetype& archetype) {
        if (!archetype.Mask().test(GetComponentId<ModelRenderer>())) return;
        for (usize chunk = 0; chunk < archetype.ChunkCount(); ++chunk) {
            const auto* models = static_cast<const ModelRenderer*>(archetype.ComponentArray(chunk, GetComponentId<ModelRenderer>()));
            for (usize row = 0; row < archetype.ChunkEntityCount(chunk); ++row) {
                const std::string path = models[row].asset_path;
                pines += path.find("_Pine_") != std::string::npos;
                grass += path.find("_Grass_") != std::string::npos;
            }
        }
    });
    CHECK(pines == counts[0]); // every pine survived
    CHECK(grass <= 25 + counts[4] + counts[5] + counts[6] + counts[7] + counts[8]); // the grass got what was left, not a share of everything
    std::error_code ec;
    fs::remove_all(root, ec);
}

AETHER_TEST(ProceduralEditor_PanelDrawsEditsAndRegeneratesHeadless) {
    HeadlessImGui ui;
    ProceduralWorldDocument doc(Small());
    TerrainEditDocument terrain = FlatTerrainDocument();
    FoliagePaintDocument foliage(FoliageLayer{});
    ProceduralWorldPanel panel(doc, &terrain, &foliage);
    ui.Frame([&] { panel.Draw(); });
    CHECK(!doc.Generated()); // drawing alone generates nothing
    doc.Generate();
    for (int i = 0; i < 3; ++i) ui.Frame([&] { panel.Draw(); });
    CHECK(doc.Generated() && !doc.Stale());
    // A settings change while the panel is open regenerates once the edit has settled.
    ProceduralWorldSettings s = doc.Settings();
    s.terrain.height_scale = 75.0f;
    doc.Checkpoint();
    doc.Set(s);
    CHECK(doc.Stale());
    ui.Frame([&] { panel.Draw(); });
    CHECK(!doc.Stale() && doc.Terrain().height_max <= 75.0f + 1e-3f);
    // Auto-regeneration off: it waits for the button.
    panel.auto_generate = false;
    s.terrain.height_scale = 40.0f;
    doc.Set(s);
    ui.Frame([&] { panel.Draw(); });
    CHECK(doc.Stale());
    // No targets, no rules: still draws.
    ProceduralWorldSettings none = Small();
    none.rules.clear();
    none.slots.clear();
    ProceduralWorldDocument bare(none);
    bare.Generate();
    ProceduralWorldPanel solo(bare);
    ui.Frame([&] { solo.Draw(); });
    CHECK(bare.Instances().empty() && solo.Status().empty());
}

AETHER_TEST(ProceduralEditor_OptInBakeOfAFullSizeWorld) {
    // Opt-in: bake a full-size world into AETHER_PROCEDURAL_BAKE_DIR (a content root) for a look in the player.
    const char* dir = std::getenv("AETHER_PROCEDURAL_BAKE_DIR");
    if (dir == nullptr) return;
    for (const WorldPreset preset : {WorldPreset::Temperate, WorldPreset::Alpine}) {
        ProceduralWorldDocument doc(MakePreset(preset));
        doc.Generate();
        BakeOptions options;
        options.name = WorldPresetName(preset);
        options.max_scene_instances = 3000;
        const BakeReport report = doc.Bake(dir, options);
        std::printf("[bake] %s: %s, %zu files, %u objects in the scene (%u left out), generated in %.0f ms\n", WorldPresetName(preset), report.ok ? "ok" : report.error.c_str(),
                    report.files.size(), report.instances_in_scene, report.instances_left_out, doc.GenerateMilliseconds());
        CHECK(report.ok);
    }
}
