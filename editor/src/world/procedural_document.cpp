#include "world/procedural_document.h"

#include "aether/scene/components.h"
#include "aether/scene/serialization.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <set>

namespace aether::editor {

using namespace aether::terrain;
using namespace aether::assets::procedural;
namespace fs = std::filesystem;

namespace {
constexpr usize kMaxUndo = 64;

MeshSlot Slot(ProceduralKind kind, u32 variants, f32 scale = 1.0f) { return MeshSlot{true, kind, variants, scale}; }

// The mesh family for each of the starter rules, by name; any other rule starts with none.
MeshSlot DefaultSlotFor(const std::string& rule_name) {
    if (rule_name == "Pine") return Slot(ProceduralKind::PineTree, 6);
    if (rule_name == "Birch") return Slot(ProceduralKind::BroadleafTree, 6, 0.6f);
    if (rule_name == "Boulder") return Slot(ProceduralKind::Rock, 6);
    if (rule_name == "DeadTree") return Slot(ProceduralKind::DeadTree, 4);
    if (rule_name == "Shrub") return Slot(ProceduralKind::Shrub, 6);
    if (rule_name == "Grass") return Slot(ProceduralKind::Grass, 8);
    if (rule_name == "Reeds") return Slot(ProceduralKind::Reeds, 5);
    if (rule_name == "Cactus") return Slot(ProceduralKind::Cactus, 5);
    if (rule_name == "SnowRock") return Slot(ProceduralKind::Rock, 4);
    return MeshSlot{};
}

std::string FileSafe(const std::string& name) {
    std::string out;
    for (const char c : name) out += (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-') ? c : '_';
    return out.empty() ? std::string("unnamed") : out;
}

bool WriteFile(const fs::path& path, const std::vector<u8>& bytes) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(f);
}
} // namespace

const char* WorldPresetName(WorldPreset preset) {
    switch (preset) {
    case WorldPreset::Temperate: return "Temperate";
    case WorldPreset::Alpine: return "Alpine";
    case WorldPreset::Desert: return "Desert";
    case WorldPreset::Islands: return "Islands";
    }
    return "Unknown";
}

ProceduralWorldSettings MakePreset(WorldPreset preset) {
    ProceduralWorldSettings s;
    s.rules = DefaultScatterRules();
    for (const ScatterRule& r : s.rules) s.slots.push_back(DefaultSlotFor(r.name));
    s.terrain.width = s.terrain.height = 193;
    s.terrain.hydraulic_droplets = 40000;
    s.terrain.river_min_catchment = 250;
    switch (preset) {
    case WorldPreset::Temperate: break;
    case WorldPreset::Alpine:
        s.terrain.height_scale = 120.0f;
        s.terrain.ridge_amount = 0.85f;
        s.terrain.sea_level = 0.08f;
        s.terrain.snow_line = 0.66f;
        s.terrain.tree_line = 0.5f;
        s.terrain.rock_slope = 0.8f;
        s.terrain.talus = 1.0f;
        break;
    case WorldPreset::Desert:
        s.terrain.height_scale = 45.0f;
        s.terrain.ridge_amount = 0.35f;
        s.terrain.sea_level = 0.04f;
        s.terrain.snow_line = 0.98f;
        s.terrain.tree_line = 0.4f;
        s.terrain.river_min_catchment = 900;
        s.terrain.hydraulic_droplets = 20000;
        s.terrain.thermal_iterations = 40;
        break;
    case WorldPreset::Islands:
        s.terrain.height_scale = 50.0f;
        s.terrain.ridge_amount = 0.2f;
        s.terrain.sea_level = 0.42f;
        s.terrain.lowland_power = 1.9f;
        s.terrain.snow_line = 0.97f;
        break;
    }
    s.scatter.seed = 1;
    return s;
}

ProceduralWorldDocument::ProceduralWorldDocument(ProceduralWorldSettings settings) : settings_(std::move(settings)) {
    settings_.slots.resize(settings_.rules.size());
}

void ProceduralWorldDocument::Changed() { ++revision_; }

void ProceduralWorldDocument::Checkpoint() {
    undo_.push_back(settings_);
    if (undo_.size() > kMaxUndo) undo_.erase(undo_.begin());
    redo_.clear();
}

void ProceduralWorldDocument::Set(const ProceduralWorldSettings& settings) {
    settings_ = settings;
    settings_.slots.resize(settings_.rules.size());
    Changed();
}

void ProceduralWorldDocument::LoadPreset(WorldPreset preset) {
    Checkpoint();
    // A preset reshapes the world; the seeds stay, so "the same world, differently shaped" is what you get.
    const u32 terrain_seed = settings_.terrain.seed, scatter_seed = settings_.scatter.seed, mesh_seed = settings_.mesh_seed;
    settings_ = MakePreset(preset);
    settings_.terrain.seed = terrain_seed;
    settings_.scatter.seed = scatter_seed;
    settings_.mesh_seed = mesh_seed;
    Changed();
}

void ProceduralWorldDocument::RandomizeSeed() {
    Checkpoint();
    u32 x = settings_.terrain.seed * 747796405u + 2891336453u;
    x = ((x >> ((x >> 28u) + 4u)) ^ x) * 277803737u;
    settings_.terrain.seed = (x >> 22u) ^ x;
    settings_.scatter.seed = settings_.terrain.seed ^ 0x9E3779B9u;
    Changed();
}

bool ProceduralWorldDocument::Undo() {
    if (undo_.empty()) return false;
    redo_.push_back(settings_);
    settings_ = undo_.back();
    undo_.pop_back();
    Changed();
    return true;
}

bool ProceduralWorldDocument::Redo() {
    if (redo_.empty()) return false;
    undo_.push_back(settings_);
    settings_ = redo_.back();
    redo_.pop_back();
    Changed();
    return true;
}

void ProceduralWorldDocument::AddRule(const ScatterRule& rule) {
    Checkpoint();
    settings_.rules.push_back(rule);
    settings_.slots.resize(settings_.rules.size());
    Changed();
}

bool ProceduralWorldDocument::RemoveRule(usize index) {
    if (index >= settings_.rules.size()) return false;
    Checkpoint();
    settings_.rules.erase(settings_.rules.begin() + static_cast<std::ptrdiff_t>(index));
    settings_.slots.erase(settings_.slots.begin() + static_cast<std::ptrdiff_t>(index));
    Changed();
    return true;
}

bool ProceduralWorldDocument::DuplicateRule(usize index) {
    if (index >= settings_.rules.size()) return false;
    Checkpoint();
    ScatterRule copy = settings_.rules[index];
    copy.name += " copy";
    const MeshSlot slot = settings_.slots[index];
    settings_.rules.insert(settings_.rules.begin() + static_cast<std::ptrdiff_t>(index) + 1, copy);
    settings_.slots.insert(settings_.slots.begin() + static_cast<std::ptrdiff_t>(index) + 1, slot);
    Changed();
    return true;
}

void ProceduralWorldDocument::Generate() {
    const auto begin = std::chrono::steady_clock::now();
    terrain_ = GenerateTerrain(settings_.terrain);
    generated_terrain_settings_ = SanitizeSettings(settings_.terrain);
    instances_ = Scatter(terrain_, generated_terrain_settings_, settings_.rules, settings_.scatter);
    generate_ms_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
    generated_ = true;
    generated_revision_ = revision_;
}

std::vector<usize> ProceduralWorldDocument::CountsByRule() const {
    std::vector<usize> counts(settings_.rules.size(), 0);
    for (const ScatterInstance& i : instances_)
        if (i.rule < counts.size()) ++counts[i.rule];
    return counts;
}

std::vector<u8> ProceduralWorldDocument::Preview(u32 max_side, u32& w, u32& h) const {
    w = h = 0;
    if (!generated_) return {};
    std::vector<u8> full = RenderTerrainPreview(terrain_);
    OverlayScatterPreview(full, terrain_, instances_);
    const u32 sw = terrain_.Width(), sh = terrain_.Height();
    const u32 side = std::max(max_side, 1u);
    w = std::min(sw, side);
    h = std::min(sh, side);
    std::vector<u8> out(static_cast<usize>(w) * h * 3);
    for (u32 y = 0; y < h; ++y)
        for (u32 x = 0; x < w; ++x) {
            const usize src = (static_cast<usize>(y * sh / h) * sw + static_cast<usize>(x * sw / w)) * 3;
            std::copy_n(full.begin() + static_cast<std::ptrdiff_t>(src), 3, out.begin() + static_cast<std::ptrdiff_t>((static_cast<usize>(y) * w + x) * 3));
        }
    return out;
}

ProceduralMesh ProceduralWorldDocument::BuildMesh(usize rule, u32 variant) const {
    if (rule >= settings_.slots.size() || !settings_.slots[rule].enabled) return {};
    const MeshSlot& slot = settings_.slots[rule];
    return GenerateVariant(slot.kind, variant % std::max(slot.variants, 1u), settings_.mesh_seed + static_cast<u32>(rule) * 7919u);
}

ProceduralMesh ProceduralWorldDocument::BuildTerrainModel(const GeneratedTerrain& t, u32 max_grid) {
    ProceduralMesh out;
    const u32 w = t.Width(), h = t.Height();
    if (w < 2 || h < 2) return out;
    const u32 grid = std::max(max_grid, 2u);
    const u32 step = std::max(1u, (std::max(w, h) - 1 + grid - 1) / grid);
    const std::vector<TerrainLayer> layers = DefaultTerrainLayers();
    // Six surfaces by what is on the ground: water, sand, grass, forest floor, rock, snow.
    const Vec3 colors[6] = {Vec3(0.14f, 0.34f, 0.60f), layers[2].albedo, layers[0].albedo, Vec3(0.16f, 0.32f, 0.12f), layers[1].albedo, layers[3].albedo};
    const f32 roughness[6] = {0.15f, 0.95f, 0.9f, 0.9f, 0.85f, 0.7f};
    auto surface = [](Biome b) {
        switch (b) {
        case Biome::Water: return 0;
        case Biome::Beach: case Biome::Desert: return 1;
        case Biome::Grassland: return 2;
        case Biome::Forest: return 3;
        case Biome::Rock: return 4;
        case Biome::Snow: return 5;
        }
        return 2;
    };
    MeshBuilder builders[6];
    const f32 cell = t.heightmap.cell_size;
    // Sample columns and rows at `step`, the last one always the edge.
    std::vector<u32> xs, zs;
    for (u32 x = 0; x < w - 1; x += step) xs.push_back(x);
    xs.push_back(w - 1);
    for (u32 z = 0; z < h - 1; z += step) zs.push_back(z);
    zs.push_back(h - 1);
    auto height_at = [&](u32 x, u32 z) { return t.heightmap.heights[t.Index(x, z)]; };
    // The water's surface: a lake sits at the level its basin fills to and the sea at the sea level, both flat; anywhere else, the ground.
    auto water_at = [&](u32 x, u32 z) {
        const usize i = t.Index(x, z);
        if (i < t.lake.size() && t.lake[i] && i < t.fill_height.size()) return t.fill_height[i];
        if (t.heightmap.heights[i] <= t.sea_height) return t.sea_height;
        return t.heightmap.heights[i];
    };
    for (usize zi = 0; zi + 1 < zs.size(); ++zi) {
        for (usize xi = 0; xi + 1 < xs.size(); ++xi) {
            const u32 x0 = xs[xi], x1 = xs[xi + 1], z0 = zs[zi], z1 = zs[zi + 1];
            const Vec3 p00(static_cast<f32>(x0) * cell, height_at(x0, z0), static_cast<f32>(z0) * cell), p10(static_cast<f32>(x1) * cell, height_at(x1, z0), static_cast<f32>(z0) * cell);
            const Vec3 p01(static_cast<f32>(x0) * cell, height_at(x0, z1), static_cast<f32>(z1) * cell), p11(static_cast<f32>(x1) * cell, height_at(x1, z1), static_cast<f32>(z1) * cell);
            const int s = surface(t.biome[t.Index((x0 + x1) / 2, (z0 + z1) / 2)]);
            MeshBuilder& b = builders[s];
            if (s == 0) {
                // Water quads lie flat on the surface of the lake or sea, not on its bed.
                const Vec3 w00(p00.x, water_at(x0, z0), p00.z), w10(p10.x, water_at(x1, z0), p10.z), w01(p01.x, water_at(x0, z1), p01.z), w11(p11.x, water_at(x1, z1), p11.z);
                const u32 j00 = b.Vertex(w00), j10 = b.Vertex(w10), j01 = b.Vertex(w01), j11 = b.Vertex(w11);
                b.Triangle(j00, j01, j10);
                b.Triangle(j10, j01, j11);
                continue;
            }
            const u32 i00 = b.Vertex(p00), i10 = b.Vertex(p10), i01 = b.Vertex(p01), i11 = b.Vertex(p11);
            b.Triangle(i00, i01, i10); // counter-clockwise from above: the ground faces up
            b.Triangle(i10, i01, i11);
        }
    }
    for (int s = 0; s < 6; ++s) {
        if (builders[s].Empty()) continue;
        assets::MaterialData m;
        m.base_color = Vec4(colors[s].x, colors[s].y, colors[s].z, 1.0f);
        m.roughness = roughness[s];
        m.metallic = 0.0f;
        out.materials.push_back(m);
        out.mesh.primitives.push_back(builders[s].Finish(Shading::Smooth, UvMode::BoxProjected, static_cast<i32>(out.materials.size() - 1)));
    }
    return out;
}

bool ProceduralWorldDocument::ApplyToTerrain(TerrainEditDocument& target, u32 splat_resolution) const {
    if (!generated_) return false;
    TerrainSettings settings;
    settings.verts_per_chunk = 32;
    settings.chunk_world_size = 31.0f * terrain_.heightmap.cell_size;
    TerrainData data;
    InitTerrainData(data, terrain_.heightmap, settings);
    data.layers = DefaultTerrainLayers();
    std::vector<SplatmapLayer> layers(4);
    const char* names[4] = {"Grass", "Rock", "Sand", "Snow"};
    for (usize l = 0; l < 4; ++l) { layers[l].name = names[l]; layers[l].albedo_tint = data.layers[l].albedo; layers[l].roughness = data.layers[l].roughness; }
    target.Replace(std::move(data), settings, BuildTerrainSplatmap(terrain_, generated_terrain_settings_, layers, std::max(splat_resolution, 1u)));
    return true;
}

bool ProceduralWorldDocument::ApplyToFoliage(FoliagePaintDocument& target) const {
    if (!generated_) return false;
    FoliageLayer layer;
    layer.name = "Scattered";
    for (const ScatterRule& r : settings_.rules) {
        FoliageType type;
        type.scale_min = r.scale_min;
        type.scale_max = r.scale_max;
        type.align_to_normal = r.align_to_normal > 0.5f;
        type.anchor_offset = r.y_offset;
        layer.types.push_back(type);
    }
    layer.instances = ToFoliageInstances(instances_, settings_.terrain.cell_size * 32.0f);
    layer.max_instances = std::max<u32>(layer.max_instances, static_cast<u32>(layer.instances.size()));
    target.ReplaceLayer(std::move(layer));
    return true;
}

BakeReport ProceduralWorldDocument::Bake(const std::string& content_root, const BakeOptions& options) const {
    BakeReport report;
    if (!generated_) { report.error = "Nothing is generated yet: press Generate first."; return report; }
    const std::string name = FileSafe(options.name);
    const std::string relative = "Generated/" + name;
    const fs::path dir = fs::path(content_root) / "Generated" / name;
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) { report.error = "Cannot create " + dir.string() + ": " + ec.message(); return report; }
    auto write = [&](const std::string& file, const std::vector<u8>& bytes) {
        if (!WriteFile(dir / file, bytes)) { report.error = "Cannot write " + (dir / file).string(); return false; }
        report.files.push_back(relative + "/" + file);
        return true;
    };

    const ProceduralMesh ground = BuildTerrainModel(terrain_, options.terrain_grid);
    if (!write(name + "_terrain.glb", EncodeGlb(ground))) return report;

    // One .glb per mesh variant that a rule with a mesh can place.
    std::vector<std::vector<std::string>> variant_paths(settings_.rules.size());
    for (usize r = 0; r < settings_.rules.size(); ++r) {
        if (!settings_.slots[r].enabled) { report.rules_without_mesh.push_back(settings_.rules[r].name); continue; }
        const u32 variants = std::max(settings_.slots[r].variants, 1u);
        for (u32 v = 0; v < variants; ++v) {
            const std::string file = name + "_" + FileSafe(settings_.rules[r].name) + "_" + std::to_string(v) + ".glb";
            if (!write(file, EncodeGlb(BuildMesh(r, v)))) return report;
            variant_paths[r].push_back(relative + "/" + file);
        }
    }

    // The scene: the terrain, then the instances (those of a rule with a mesh, up to the cap, spread evenly over the lot).
    World world;
    {
        ModelRenderer renderer;
        SetModelPath(renderer, relative + "/" + name + "_terrain.glb");
        world.CreateEntity(Transform{Vec3(0, 0, 0), Quaternion::Identity()}, renderer);
    }
    // The budget goes to the rules in order, as the scatter's own pecking order does: the trees keep every instance while there is room and the
    // grass between them is what is thinned (evenly) to fit what is left.
    std::vector<usize> chosen;
    {
        std::vector<std::vector<usize>> by_rule(settings_.rules.size());
        for (usize i = 0; i < instances_.size(); ++i)
            if (instances_[i].rule < variant_paths.size() && !variant_paths[instances_[i].rule].empty()) by_rule[instances_[i].rule].push_back(i);
        usize left = options.max_scene_instances;
        for (const std::vector<usize>& list : by_rule) {
            if (left == 0) break;
            const usize take = std::min(left, list.size());
            for (usize k = 0; k < take; ++k) chosen.push_back(list[take >= list.size() ? k : k * list.size() / take]);
            left -= take;
        }
    }
    const usize cap = chosen.size();
    for (usize k = 0; k < cap; ++k) {
        const usize pick = chosen[k];
        const ScatterInstance& inst = instances_[pick];
        const std::vector<std::string>& paths = variant_paths[inst.rule];
        ModelRenderer renderer;
        SetModelPath(renderer, paths[static_cast<usize>((static_cast<u64>(pick) * 2654435761ull) >> 8) % paths.size()]); // a variant by instance, not by order
        const f32 s = inst.scale * settings_.slots[inst.rule].scale;
        Transform t{inst.position, inst.rotation};
        t.scale = Vec3(s, s, s);
        world.CreateEntity(t, renderer);
    }
    report.instances_in_scene = static_cast<u32>(cap);
    report.instances_left_out = static_cast<u32>(instances_.size() - cap);
    const std::string scene_file = name + ".ascene";
    if (!SaveSceneJson(world, (dir / scene_file).string())) { report.error = "Cannot write " + (dir / scene_file).string(); return report; }
    report.files.push_back(relative + "/" + scene_file);
    report.scene = relative + "/" + scene_file;
    report.ok = true;
    return report;
}

} // namespace aether::editor
