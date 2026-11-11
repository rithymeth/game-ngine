#include "world/procedural_document.h"

#include "aether/assets/png_writer.h"
#include "aether/renderer/components.h"
#include "aether/scene/components.h"
#include "aether/scene/serialization.h"
#if AETHER_EDITOR_NET_PHYSICS
#include "aether/physics/components.h"
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <limits>
#include <map>
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
    if (rule_name == "Oak") return Slot(ProceduralKind::OakTree, 6, 0.8f);
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

// The sample columns and rows the ground's mesh is built on: every `step`-th, so at most `max_grid` quads a side, and always the last. The drawn ground and the
// collision mesh both come from here, so they are the same triangles.
void TerrainGridLines(u32 w, u32 h, u32 max_grid, std::vector<u32>& xs, std::vector<u32>& zs) {
    const u32 step = std::max(1u, (std::max(w, h) - 1 + std::max(max_grid, 2u) - 1) / std::max(max_grid, 2u));
    for (u32 x = 0; x < w - 1; x += step) xs.push_back(x);
    xs.push_back(w - 1);
    for (u32 z = 0; z < h - 1; z += step) zs.push_back(z);
    zs.push_back(h - 1);
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

ProceduralWorldDocument::CollisionMesh ProceduralWorldDocument::BuildTerrainCollisionMesh(const GeneratedTerrain& t, u32 max_grid) {
    CollisionMesh out;
    const u32 w = t.Width(), h = t.Height();
    if (w < 2 || h < 2) return out;
    std::vector<u32> xs, zs;
    TerrainGridLines(w, h, max_grid, xs, zs);
    const f32 cell = t.heightmap.cell_size;
    out.vertices.reserve(xs.size() * zs.size());
    for (const u32 z : zs)
        for (const u32 x : xs) out.vertices.push_back(Vec3(static_cast<f32>(x) * cell, t.heightmap.heights[t.Index(x, z)], static_cast<f32>(z) * cell));
    out.indices.reserve((xs.size() - 1) * (zs.size() - 1) * 6);
    for (usize zi = 0; zi + 1 < zs.size(); ++zi)
        for (usize xi = 0; xi + 1 < xs.size(); ++xi) {
            const u32 i00 = static_cast<u32>(zi * xs.size() + xi), i10 = i00 + 1, i01 = static_cast<u32>((zi + 1) * xs.size() + xi), i11 = i01 + 1;
            out.indices.insert(out.indices.end(), {i00, i01, i10, i10, i01, i11}); // the same two triangles, the same winding, as the drawn ground
        }
    return out;
}

ProceduralMesh ProceduralWorldDocument::BuildTexturedTerrainModel(const GeneratedTerrain& t, u32 max_grid, const std::string& texture_uri, f32 skirt_depth, const std::string& water_texture_uri, const std::string& water_normal_uri, u32 tiles_per_side) {
    ProceduralMesh out;
    const u32 w = t.Width(), h = t.Height();
    if (w < 2 || h < 2) return out;
    std::vector<u32> xs, zs;
    TerrainGridLines(w, h, max_grid, xs, zs);
    const f32 cell = t.heightmap.cell_size;
    auto height_at = [&](u32 x, u32 z) { return t.heightmap.heights[t.Index(x, z)]; };
    // The water at a sample: a lake's fill level, the sea's level, or none (a negative infinity).
    auto water_level = [&](u32 x, u32 z) -> f32 {
        const usize i = t.Index(x, z);
        if (i < t.lake.size() && t.lake[i] && i < t.fill_height.size()) return t.fill_height[i];
        if (t.heightmap.heights[i] <= t.sea_height) return t.sea_height;
        return -std::numeric_limits<f32>::infinity();
    };
    MeshBuilder ground;
    std::map<i64, MeshBuilder> water_by_level; // one flat sheet per water level (millimetres), so each lake is a primitive of its own and the player can mirror it
    // The ground is a grid of shared vertices (smooth normals), its UV the position over the map.
    std::vector<u32> grid(xs.size() * zs.size());
    for (usize zi = 0; zi < zs.size(); ++zi)
        for (usize xi = 0; xi < xs.size(); ++xi)
            grid[zi * xs.size() + xi] = ground.Vertex(Vec3(static_cast<f32>(xs[xi]) * cell, height_at(xs[xi], zs[zi]), static_cast<f32>(zs[zi]) * cell),
                                                      static_cast<f32>(xs[xi]) / static_cast<f32>(w - 1), static_cast<f32>(zs[zi]) / static_cast<f32>(h - 1));
    for (usize zi = 0; zi + 1 < zs.size(); ++zi) {
        for (usize xi = 0; xi + 1 < xs.size(); ++xi) {
            const u32 i00 = grid[zi * xs.size() + xi], i10 = grid[zi * xs.size() + xi + 1], i01 = grid[(zi + 1) * xs.size() + xi], i11 = grid[(zi + 1) * xs.size() + xi + 1];
            ground.Triangle(i00, i01, i10); // counter-clockwise from above: the ground faces up
            ground.Triangle(i10, i01, i11);
            const u32 x0 = xs[xi], x1 = xs[xi + 1], z0 = zs[zi], z1 = zs[zi + 1];
            // Water geometry for lakes and the sea only (the rivers are in the ground texture). A quad with any corner in the water gets a flat quad at that
            // water's level, which runs on under the dry corners: the ground hides it there, so the visible shoreline is where the ground really crosses the
            // water level, not a staircase of whole quads.
            const f32 level = std::max({water_level(x0, z0), water_level(x1, z0), water_level(x0, z1), water_level(x1, z1)});
            if (!std::isfinite(level)) continue;
            const f32 fx0 = static_cast<f32>(x0) * cell, fx1 = static_cast<f32>(x1) * cell, fz0 = static_cast<f32>(z0) * cell, fz1 = static_cast<f32>(z1) * cell;
            // The water's UV is the position over the map, like the ground's, so the water texture lines up with the ground it covers.
            const f32 u0 = static_cast<f32>(x0) / static_cast<f32>(w - 1), u1 = static_cast<f32>(x1) / static_cast<f32>(w - 1), v0 = static_cast<f32>(z0) / static_cast<f32>(h - 1), v1 = static_cast<f32>(z1) / static_cast<f32>(h - 1);
            MeshBuilder& water = water_by_level[static_cast<i64>(std::llround(level * 1000.0f))];
            const u32 j00 = water.Vertex(Vec3(fx0, level, fz0), u0, v0), j10 = water.Vertex(Vec3(fx1, level, fz0), u1, v0);
            const u32 j01 = water.Vertex(Vec3(fx0, level, fz1), u0, v1), j11 = water.Vertex(Vec3(fx1, level, fz1), u1, v1);
            water.Triangle(j00, j01, j10);
            water.Triangle(j10, j01, j11);
        }
    }
    assets::MaterialData ground_material;
    ground_material.base_color = Vec4(1, 1, 1, 1);
    ground_material.roughness = 0.9f;
    ground_material.metallic = 0.0f;
    ground_material.base_color_texture = texture_uri;
    ground_material.detail = 0.85f; // a micro-surface over the stretched texture, so the ground stays rough up close
    out.materials.push_back(ground_material);
    // Cut into tiles: one primitive has one bounding sphere around the whole map, so it is never culled, by the camera or by a shadow cascade.
    for (assets::MeshPrimitiveData& tile : SplitIntoTiles(ground.Finish(Shading::Smooth, UvMode::Given, 0), tiles_per_side, tiles_per_side)) out.mesh.primitives.push_back(std::move(tile));
    // The skirt: the four edges carried down to `skirt_depth` below the lowest ground, so the terrain is a block with its soil showing and not a sheet
    // with an open lip. It is flat shaded, textured by the edge of the ground texture (stretched down), and drawn with the ground's material.
    MeshBuilder skirt;
    if (skirt_depth > 0.0f) {
        const f32 base = t.height_min - skirt_depth;
        auto column = [&](u32 x, u32 z) {
            const f32 fx = static_cast<f32>(x) * cell, fz = static_cast<f32>(z) * cell, u = static_cast<f32>(x) / static_cast<f32>(w - 1), v = static_cast<f32>(z) / static_cast<f32>(h - 1);
            return std::pair<u32, u32>{skirt.Vertex(Vec3(fx, height_at(x, z), fz), u, v), skirt.Vertex(Vec3(fx, base, fz), u, v)};
        };
        // North (z = 0, facing -z) and south (z = max, facing +z) edges along x; west (x = 0, facing -x) and east (x = max, facing +x) along z.
        for (usize xi = 0; xi + 1 < xs.size(); ++xi) {
            const auto a = column(xs[xi], 0), b = column(xs[xi + 1], 0), c = column(xs[xi], h - 1), d = column(xs[xi + 1], h - 1);
            skirt.Quad(a.first, b.first, b.second, a.second);
            skirt.Quad(d.first, c.first, c.second, d.second);
        }
        for (usize zi = 0; zi + 1 < zs.size(); ++zi) {
            const auto a = column(0, zs[zi]), b = column(0, zs[zi + 1]), c = column(w - 1, zs[zi]), d = column(w - 1, zs[zi + 1]);
            skirt.Quad(b.first, a.first, a.second, b.second);
            skirt.Quad(c.first, d.first, d.second, c.second);
        }
        out.mesh.primitives.push_back(skirt.Finish(Shading::Flat, UvMode::Given, 0));
    }
    if (!water_by_level.empty()) {
        assets::MaterialData m;
        // With a water texture the colour comes from it (white factor); without, one flat blue.
        m.base_color = water_texture_uri.empty() ? Vec4(0.14f, 0.34f, 0.60f, 1.0f) : Vec4(1, 1, 1, 1);
        m.roughness = 0.1f;
        m.metallic = 0.0f;
        m.base_color_texture = water_texture_uri;
        m.normal_texture = water_normal_uri;
        m.water = !water_normal_uri.empty(); // the ripples move: the scene shader scrolls this material's normal map with the game's time
        // With a texture its alpha is the water's opacity (the glTF material blends); without one, the water is a solid colour.
        if (!water_texture_uri.empty()) m.alpha_mode = assets::MaterialAlphaMode::Blend;
        out.materials.push_back(m);
        for (const auto& [millimetres, sheet] : water_by_level) out.mesh.primitives.push_back(sheet.Finish(Shading::Smooth, UvMode::Given, 1));
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

    ProceduralMesh ground;
    if (options.terrain_texture > 0) {
        // The texture sits beside the model, named in it by its file name (the player finds it in the package from the model's own folder).
        const u32 size = std::clamp(options.terrain_texture, 16u, 4096u);
        const std::vector<u8> pixels = RenderTerrainTexture(terrain_, generated_terrain_settings_, size);
        if (!write(name + "_terrain.png", assets::EncodePngRgb8(size, size, pixels))) return report;
        std::string water_uri, water_normal_uri;
        if (options.water_texture > 0) {
            const u32 water_size = std::clamp(options.water_texture, 16u, 4096u);
            if (!write(name + "_water.png", assets::EncodePngRgba8(water_size, water_size, RenderWaterTexture(terrain_, generated_terrain_settings_, water_size)))) return report;
            water_uri = name + "_water.png";
            // The ripples' normal map, over the same map; it wants more texels than the colour (a ripple is about 0.7 m), so it follows the terrain texture's size.
            const u32 normal_size = std::clamp(options.terrain_texture, 16u, 4096u);
            if (!write(name + "_water_normal.png", assets::EncodePngRgb8(normal_size, normal_size, RenderWaterNormalMap(terrain_, generated_terrain_settings_, normal_size)))) return report;
            water_normal_uri = name + "_water_normal.png";
        }
        ground = BuildTexturedTerrainModel(terrain_, options.terrain_grid, name + "_terrain.png", options.terrain_skirt, water_uri, water_normal_uri, options.terrain_tiles);
    } else {
        ground = BuildTerrainModel(terrain_, options.terrain_grid);
    }
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
#if AETHER_EDITOR_NET_PHYSICS
    if (options.terrain_collision) {
        // Static: a mesh collider on an entity with no RigidBody. It follows the drawn ground triangle for triangle (the same grid), so a character's feet meet what is seen.
        CollisionMesh mesh = BuildTerrainCollisionMesh(terrain_, options.terrain_grid);
        MeshCollider collider;
        collider.vertices = std::move(mesh.vertices);
        collider.indices = std::move(mesh.indices);
        collider.friction = 0.8f;
        world.CreateEntity(Transform{Vec3(0, 0, 0), Quaternion::Identity()}, collider);
    }
#endif
    if (options.add_wind) {
        Wind wind;
        wind.direction_degrees = options.wind_direction_degrees;
        wind.strength = options.wind_strength;
        world.CreateEntity(Transform{Vec3(0, 0, 0), Quaternion::Identity()}, wind);
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
