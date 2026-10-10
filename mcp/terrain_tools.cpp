#include "terrain_tools.h"

#include "aether/terrain/foliage.h"
#include "aether/terrain/splat.h"
#include "aether/terrain/terrain.h"

#include "image_util.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <sstream>

namespace aether::mcp {

namespace {

namespace fs = std::filesystem;
namespace tr = aether::terrain;

constexpr u32 kMaxSamples = 2049; // per side
constexpr usize kMaxUndo = 20;

// ---------------------------------------------------------------------------
// The workbench
// ---------------------------------------------------------------------------

struct Workbench {
    bool exists = false;
    tr::TerrainData data;
    tr::TerrainSettings settings;
    std::vector<tr::SplatmapLayer> layers;
    tr::SplatmapData splat;
    tr::FoliageLayer foliage;
    struct Snapshot {
        std::vector<f32> heights;
        std::vector<u8> splat;
    };
    std::vector<Snapshot> undo, redo;

    f32 WorldWidth() const { return data.heightmap.width > 1 ? static_cast<f32>(data.heightmap.width - 1) * data.heightmap.cell_size : 0.0f; }
    f32 WorldDepth() const { return data.heightmap.height > 1 ? static_cast<f32>(data.heightmap.height - 1) * data.heightmap.cell_size : 0.0f; }
    f32 HeightAt(f32 x, f32 z) const { return data.heightmap.SampleLinear(x, z) * settings.vertical_scale; }

    Vec3 NormalAt(f32 x, f32 z) const {
        const f32 d = data.heightmap.cell_size;
        const f32 dx = (HeightAt(x + d, z) - HeightAt(x - d, z)) / (2.0f * d);
        const f32 dz = (HeightAt(x, z + d) - HeightAt(x, z - d)) / (2.0f * d);
        const f32 len = std::sqrt(dx * dx + 1.0f + dz * dz);
        return Vec3(-dx / len, 1.0f / len, -dz / len);
    }
    static f32 SlopeDegrees(const Vec3& n) { return std::acos(std::clamp(n.y, -1.0f, 1.0f)) * 57.29578f; }

    Snapshot Capture() const { return {data.heightmap.heights, splat.pixels}; }
    void Restore(const Snapshot& s) {
        data.heightmap.heights = s.heights;
        splat.pixels = s.splat;
    }
    void PushUndo(Snapshot before) {
        undo.push_back(std::move(before));
        if (undo.size() > kMaxUndo) undo.erase(undo.begin());
        redo.clear();
    }
    void Refresh() { tr::InitTerrainData(data, data.heightmap, settings); } // bounds and chunk layout from the heights
};

Json Schema(Json properties, std::vector<std::string> required = {}) {
    Json schema = {{"type", "object"}, {"properties", std::move(properties)}};
    if (!required.empty()) schema["required"] = required;
    return schema;
}

Json Vec3Json(const Vec3& v) { return Json::array({v.x, v.y, v.z}); }

double Number(const Json& args, const char* key, double fallback, double lo, double hi) {
    if (!args.contains(key)) return fallback;
    if (!args[key].is_number() || !(args[key].get<double>() >= lo) || !(args[key].get<double>() <= hi)) {
        throw ToolError(std::string("\"") + key + "\" must be a number from " + std::to_string(lo) + " to " + std::to_string(hi));
    }
    return args[key].get<double>();
}

std::vector<tr::SplatmapLayer> DefaultLayers() {
    auto make = [](const char* name, Vec3 tint, f32 roughness) {
        tr::SplatmapLayer l;
        l.name = name;
        l.albedo_texture_id = 0;
        l.normal_texture_id = 0;
        l.albedo_tint = tint;
        l.roughness = roughness;
        return l;
    };
    return {make("Grass", Vec3(0.30f, 0.52f, 0.18f), 0.9f), make("Rock", Vec3(0.46f, 0.44f, 0.41f), 0.85f), make("Dirt", Vec3(0.42f, 0.31f, 0.2f), 0.95f)};
}

Json LayersJson(const Workbench& w) {
    Json out = Json::array();
    for (usize i = 0; i < w.layers.size(); ++i) {
        out.push_back({{"index", i}, {"name", w.layers[i].name}, {"albedo", Vec3Json(w.layers[i].albedo_tint)},
                       {"metallic", w.layers[i].metallic}, {"roughness", w.layers[i].roughness}});
    }
    return out;
}

void RebuildSplat(Workbench& w) {
    const u32 resolution = std::clamp<u32>(std::max(w.data.heightmap.width, w.data.heightmap.height), 16u, 512u);
    w.splat = tr::BuildSplatmap(resolution, w.layers, {});
}

void RequireTerrain(const Workbench& w) {
    if (!w.exists) throw ToolError("No terrain: call terrain_create (or terrain_import) first");
}

struct HeightStats {
    f32 min = 0, max = 0;
    double mean = 0;
};
HeightStats Stats(const Workbench& w) {
    HeightStats s;
    const std::vector<f32>& h = w.data.heightmap.heights;
    if (h.empty()) return s;
    s.min = s.max = h[0];
    double sum = 0;
    for (f32 v : h) {
        s.min = std::min(s.min, v);
        s.max = std::max(s.max, v);
        sum += v;
    }
    s.mean = sum / static_cast<double>(h.size());
    s.min *= w.settings.vertical_scale;
    s.max *= w.settings.vertical_scale;
    s.mean *= w.settings.vertical_scale;
    return s;
}

Json InfoJson(const Workbench& w) {
    const HeightStats st = Stats(w);
    const tr::Heightmap& hm = w.data.heightmap;
    return {{"samples", {{"width", hm.width}, {"depth", hm.height}}},
            {"cell_size", hm.cell_size},
            {"world_size", {{"width", w.WorldWidth()}, {"depth", w.WorldDepth()}}},
            {"heights", {{"min", st.min}, {"max", st.max}, {"mean", st.mean}}},
            {"vertical_scale", w.settings.vertical_scale},
            {"chunks", {{"vertices_per_side", w.data.chunk_size}, {"x", w.data.chunk_count_x}, {"z", w.data.chunk_count_z}, {"total", w.data.chunk_count_x * w.data.chunk_count_z}}},
            {"lod_distances", w.data.lod_distances},
            {"layers", LayersJson(w)},
            {"splatmap_resolution", w.splat.width},
            {"foliage_instances", w.foliage.instances.size()},
            {"undo_depth", w.undo.size()},
            {"redo_depth", w.redo.size()}};
}

// ---------------------------------------------------------------------------
// Pictures
// ---------------------------------------------------------------------------

Vec3 HeightColor(f32 t) {
    struct Stop { f32 at; Vec3 c; };
    static const Stop stops[] = {{0.0f, Vec3(0.10f, 0.20f, 0.50f)}, {0.25f, Vec3(0.20f, 0.50f, 0.20f)}, {0.55f, Vec3(0.50f, 0.40f, 0.25f)},
                                 {0.80f, Vec3(0.60f, 0.60f, 0.60f)}, {1.0f, Vec3(1.0f, 1.0f, 1.0f)}};
    t = std::clamp(t, 0.0f, 1.0f);
    for (usize i = 1; i < std::size(stops); ++i) {
        if (t <= stops[i].at) {
            const f32 u = (t - stops[i - 1].at) / (stops[i].at - stops[i - 1].at);
            return stops[i - 1].c + (stops[i].c - stops[i - 1].c) * u;
        }
    }
    return stops[std::size(stops) - 1].c;
}

// ---------------------------------------------------------------------------
// Files: <path>.r16 (heights, little-endian 16-bit across min..max), <path>.splat (RGBA8 weights), <path>.terrain.json
// ---------------------------------------------------------------------------

void WriteBytes(const fs::path& file, const void* data, usize size) {
    std::error_code ec;
    if (file.has_parent_path()) fs::create_directories(file.parent_path(), ec);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    if (!out) throw ToolError("Could not write " + file.string());
}

std::vector<u8> ReadBytes(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) throw ToolError("Could not read " + file.string());
    std::vector<char> raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return std::vector<u8>(raw.begin(), raw.end());
}

std::shared_ptr<Workbench>& SharedBench() {
    static std::shared_ptr<Workbench> bench = std::make_shared<Workbench>();
    return bench;
}

} // namespace

bool TerrainTriangles(u32 stride, std::vector<Vec3>& vertices, std::vector<u32>& indices, std::string* error) {
    const Workbench& w = *SharedBench();
    if (!w.exists) {
        if (error != nullptr) *error = "No terrain: call terrain_create (or terrain_import) first";
        return false;
    }
    stride = std::max(1u, stride);
    const tr::Heightmap& hm = w.data.heightmap;
    const u32 nx = (hm.width - 1) / stride + 1, nz = (hm.height - 1) / stride + 1;
    if (nx < 2 || nz < 2) {
        if (error != nullptr) *error = "The terrain is too small for that stride";
        return false;
    }
    vertices.reserve(vertices.size() + static_cast<usize>(nx) * nz);
    const u32 base = static_cast<u32>(vertices.size());
    for (u32 j = 0; j < nz; ++j) {
        for (u32 i = 0; i < nx; ++i) {
            const f32 x = static_cast<f32>(i * stride) * hm.cell_size, z = static_cast<f32>(j * stride) * hm.cell_size;
            vertices.push_back(Vec3(x, w.HeightAt(x, z), z));
        }
    }
    for (u32 j = 0; j + 1 < nz; ++j) {
        for (u32 i = 0; i + 1 < nx; ++i) {
            const u32 a = base + j * nx + i, b = a + 1, c = a + nx, d = c + 1;
            indices.insert(indices.end(), {a, c, b, b, c, d}); // both triangles face up
        }
    }
    return true;
}

void RegisterTerrainTools(McpServer& server) {
    SharedBench() = std::make_shared<Workbench>(); // each server starts with an empty workbench; TerrainTriangles reads the latest
    auto bench = SharedBench();

    server.AddTool(
        {"terrain_create",
         "Start a new terrain in the workbench (replacing any current one): a heightmap of width x depth samples, flat or procedural (fractal noise "
         "with the engine's fixed noise pattern, heights in -scale..scale), with 1-4 paint layers (default Grass, Rock, Dirt) and the splatmap "
         "painted with the first layer. The workbench is not part of any scene or game (see the terrain tools' note in the README).",
         Schema({{"width", {{"type", "integer"}, {"description", "Samples along X, 17-2049 (default 129)"}}},
                 {"depth", {{"type", "integer"}, {"description", "Samples along Z, 17-2049 (default = width)"}}},
                 {"cell_size", {{"type", "number"}, {"description", "World units per sample (default 1)"}}},
                 {"kind", {{"type", "string"}, {"description", "flat (default) or procedural"}}},
                 {"base_height", {{"type", "number"}, {"description", "Flat height (default 0)"}}},
                 {"octaves", {{"type", "integer"}, {"description", "Procedural: 1-8 (default 4)"}}},
                 {"persistence", {{"type", "number"}, {"description", "Procedural: 0-1 (default 0.5)"}}},
                 {"scale", {{"type", "number"}, {"description", "Procedural: heights span -scale..scale (default 10)"}}},
                 {"vertical_scale", {{"type", "number"}, {"description", "Multiplies stored heights into world units (default 1)"}}},
                 {"chunk_size", {{"type", "integer"}, {"description", "Vertices per chunk side: 9, 17, 33 or 65 (default 33)"}}},
                 {"layers", {{"type", "array"}, {"items", {{"type", "object"}}}, {"description", "[{name, albedo:[r,g,b], metallic?, roughness?}], 1-4"}}}}),
         [bench](const Json& args) -> Json {
             const u32 width = static_cast<u32>(Number(args, "width", 129, 17, kMaxSamples));
             const u32 depth = static_cast<u32>(Number(args, "depth", width, 17, kMaxSamples));
             const f32 cell = static_cast<f32>(Number(args, "cell_size", 1.0, 0.01, 1000.0));
             const std::string kind = args.contains("kind") && args["kind"].is_string() ? args["kind"].get<std::string>() : "flat";
             if (kind != "flat" && kind != "procedural") throw ToolError("\"kind\" must be flat or procedural");
             const u32 chunk = static_cast<u32>(Number(args, "chunk_size", 33, 9, 65));
             if (chunk != 9 && chunk != 17 && chunk != 33 && chunk != 65) throw ToolError("\"chunk_size\" must be 9, 17, 33 or 65");

             auto next = std::make_unique<Workbench>();
             next->settings.vertical_scale = static_cast<f32>(Number(args, "vertical_scale", 1.0, 0.001, 1000.0));
             next->settings.verts_per_chunk = chunk;
             next->settings.chunk_world_size = static_cast<f32>(chunk - 1) * cell;
             tr::Heightmap hm;
             if (kind == "procedural") {
                 hm = tr::CreateProceduralHeightmap(width, depth, cell, static_cast<u32>(Number(args, "octaves", 4, 1, 8)),
                                                    static_cast<f32>(Number(args, "persistence", 0.5, 0.0, 1.0)), static_cast<f32>(Number(args, "scale", 10.0, 0.0, 10000.0)));
             } else {
                 hm.width = width;
                 hm.height = depth;
                 hm.cell_size = cell;
                 hm.heights.assign(static_cast<usize>(width) * depth, static_cast<f32>(Number(args, "base_height", 0.0, -100000.0, 100000.0)));
             }
             next->data.chunk_size = chunk;
             tr::InitTerrainData(next->data, hm, next->settings);

             next->layers = DefaultLayers();
             if (args.contains("layers")) {
                 if (!args["layers"].is_array() || args["layers"].empty() || args["layers"].size() > 4) throw ToolError("\"layers\" must be 1 to 4 layer objects");
                 next->layers.clear();
                 for (const Json& l : args["layers"]) {
                     if (!l.is_object() || !l.contains("name") || !l["name"].is_string()) throw ToolError("Each layer needs a string \"name\"");
                     tr::SplatmapLayer layer;
                     layer.name = l["name"].get<std::string>();
                     layer.albedo_texture_id = 0;
                     layer.normal_texture_id = 0;
                     if (l.contains("albedo")) {
                         const Json& a = l["albedo"];
                         if (!a.is_array() || a.size() != 3 || !a[0].is_number() || !a[1].is_number() || !a[2].is_number()) throw ToolError("\"albedo\" must be [r, g, b]");
                         layer.albedo_tint = Vec3(a[0].get<f32>(), a[1].get<f32>(), a[2].get<f32>());
                     }
                     layer.metallic = static_cast<f32>(Number(l, "metallic", 0.0, 0.0, 1.0));
                     layer.roughness = static_cast<f32>(Number(l, "roughness", 0.85, 0.0, 1.0));
                     next->layers.push_back(layer);
                 }
             }
             next->data.layers.clear();
             for (const tr::SplatmapLayer& l : next->layers) {
                 tr::TerrainLayer tl;
                 tl.name = l.name;
                 tl.albedo = l.albedo_tint;
                 tl.metallic = l.metallic;
                 tl.roughness = l.roughness;
                 next->data.layers.push_back(tl);
             }
             RebuildSplat(*next);
             next->exists = true;
             *bench = std::move(*next);
             return InfoJson(*bench);
         }});

    server.AddTool({"terrain_info", "The workbench terrain: size, height range, chunk layout, LOD distances, paint layers, foliage count and undo depth.",
                    Schema(Json::object()), [bench](const Json&) -> Json {
                        RequireTerrain(*bench);
                        return InfoJson(*bench);
                    }});

    server.AddTool(
        {"terrain_sample",
         "Height, normal, slope (degrees) and paint-layer weights at world points [x, z] (up to 256). The ground plane is x from 0 to world width, z from 0 "
         "to world depth; heights are in world units (stored height x vertical_scale).",
         Schema({{"points", {{"type", "array"}, {"items", {{"type", "array"}}}, {"description", "[[x, z], ...]"}}}}, {"points"}), [bench](const Json& args) -> Json {
             RequireTerrain(*bench);
             const Json& pts = args["points"];
             if (!pts.is_array() || pts.empty() || pts.size() > 256) throw ToolError("\"points\" must be 1 to 256 [x, z] pairs");
             Json out = Json::array();
             for (const Json& p : pts) {
                 if (!p.is_array() || p.size() != 2 || !p[0].is_number() || !p[1].is_number()) throw ToolError("Each point must be [x, z]");
                 const f32 x = p[0].get<f32>(), z = p[1].get<f32>();
                 const Vec3 n = bench->NormalAt(x, z);
                 const auto weights = bench->splat.GetWeights(bench->WorldWidth() > 0 ? x / bench->WorldWidth() : 0.0f, bench->WorldDepth() > 0 ? z / bench->WorldDepth() : 0.0f);
                 Json w = Json::array();
                 for (usize i = 0; i < bench->layers.size() && i < 4; ++i) w.push_back({{"layer", bench->layers[i].name}, {"weight", weights[i]}});
                 out.push_back({{"x", x}, {"z", z}, {"height", bench->HeightAt(x, z)}, {"normal", Vec3Json(n)}, {"slope_degrees", Workbench::SlopeDegrees(n)}, {"layers", w}});
             }
             return {{"points", out}};
         }});

    server.AddTool(
        {"terrain_sculpt",
         "Sculpt or paint with the editor's brushes: tool raise | lower | smooth | flatten | paint, along `strokes` (world points [x, z], applied in order). "
         "The whole call is one undoable step. Raise/lower move heights by up to `amount` world units at full brush weight; flatten pulls toward the "
         "height under each point; paint raises layer `layer` (name or index) at the expense of the others.",
         Schema({{"tool", {{"type", "string"}, {"description", "raise, lower, smooth, flatten or paint"}}},
                 {"strokes", {{"type", "array"}, {"items", {{"type", "array"}}}, {"description", "[[x, z], ...] up to 500"}}},
                 {"radius", {{"type", "number"}, {"description", "World units (default 4)"}}},
                 {"strength", {{"type", "number"}, {"description", "0-1 (default 0.5)"}}},
                 {"falloff", {{"type", "number"}, {"description", "0-1: share of the radius that fades (default 0.5)"}}},
                 {"amount", {{"type", "number"}, {"description", "Raise/lower/smooth/flatten: world units at full weight (default 1)"}}},
                 {"layer", {{"description", "Paint: layer name or index"}}}},
                {"tool", "strokes"}),
         [bench](const Json& args) -> Json {
             RequireTerrain(*bench);
             Workbench& w = *bench;
             const std::string tool = args["tool"].is_string() ? args["tool"].get<std::string>() : "";
             if (tool != "raise" && tool != "lower" && tool != "smooth" && tool != "flatten" && tool != "paint") throw ToolError("\"tool\" must be raise, lower, smooth, flatten or paint");
             const Json& strokes = args["strokes"];
             if (!strokes.is_array() || strokes.empty() || strokes.size() > 500) throw ToolError("\"strokes\" must be 1 to 500 [x, z] pairs");
             tr::TerrainBrush brush;
             brush.radius = static_cast<f32>(Number(args, "radius", 4.0, 0.01, 10000.0));
             brush.strength = static_cast<f32>(Number(args, "strength", 0.5, 0.0, 1.0));
             brush.falloff = static_cast<f32>(Number(args, "falloff", 0.5, 0.0, 1.0));
             const f32 amount = static_cast<f32>(Number(args, "amount", 1.0, 0.0, 100000.0));
             if (tool == "paint") {
                 if (!args.contains("layer")) throw ToolError("Paint needs \"layer\" (a name or index)");
                 if (args["layer"].is_string()) {
                     const auto it = std::find_if(w.layers.begin(), w.layers.end(), [&](const tr::SplatmapLayer& l) { return l.name == args["layer"].get<std::string>(); });
                     if (it == w.layers.end()) throw ToolError("No layer named \"" + args["layer"].get<std::string>() + "\"");
                     brush.layer_index = static_cast<i32>(it - w.layers.begin());
                 } else if (args["layer"].is_number_integer() && args["layer"].get<long long>() >= 0 && args["layer"].get<size_t>() < w.layers.size()) {
                     brush.layer_index = args["layer"].get<i32>();
                 } else {
                     throw ToolError("\"layer\" must be a layer name or a valid index");
                 }
             }
             const f32 W = w.WorldWidth(), D = w.WorldDepth();
             Workbench::Snapshot before = w.Capture();
             for (const Json& s : strokes) {
                 if (!s.is_array() || s.size() != 2 || !s[0].is_number() || !s[1].is_number()) throw ToolError("Each stroke must be [x, z]");
                 const f32 x = s[0].get<f32>(), z = s[1].get<f32>();
                 tr::TerrainBrush b = brush;
                 if (tool == "raise" || tool == "lower") {
                     b.smooth = b.flatten = false;
                     b.strength = std::fabs(brush.strength) * (tool == "lower" ? -1.0f : 1.0f);
                     tr::ApplyHeightBrush(w.data.heightmap, x, z, b, amount);
                 } else if (tool == "smooth") {
                     b.smooth = true;
                     b.flatten = false;
                     tr::ApplyHeightBrush(w.data.heightmap, x, z, b, amount);
                 } else if (tool == "flatten") {
                     b.flatten = true;
                     b.smooth = false;
                     tr::ApplyHeightBrush(w.data.heightmap, x, z, b, amount);
                 } else if (W > 0.0f && D > 0.0f) {
                     b.radius = brush.radius / W * 32.0f; // ApplyBrushStroke's radius is in its own scale (the editor converts the same way)
                     tr::ApplyBrushStroke(w.splat, x / W, z / D, b);
                 }
             }
             const bool changed = w.data.heightmap.heights != before.heights || w.splat.pixels != before.splat;
             if (changed) {
                 w.PushUndo(std::move(before));
                 if (tool != "paint") w.Refresh();
             }
             Json out = {{"changed", changed}, {"strokes", strokes.size()}};
             out["terrain"] = InfoJson(w);
             return out;
         }});

    server.AddTool({"terrain_undo", "Undo the last sculpt or paint call (up to 20 steps).", Schema(Json::object()), [bench](const Json&) -> Json {
                        RequireTerrain(*bench);
                        Workbench& w = *bench;
                        if (w.undo.empty()) throw ToolError("Nothing to undo");
                        w.redo.push_back(w.Capture());
                        w.Restore(w.undo.back());
                        w.undo.pop_back();
                        w.Refresh();
                        return InfoJson(w);
                    }});
    server.AddTool({"terrain_redo", "Redo the last undone sculpt or paint call.", Schema(Json::object()), [bench](const Json&) -> Json {
                        RequireTerrain(*bench);
                        Workbench& w = *bench;
                        if (w.redo.empty()) throw ToolError("Nothing to redo");
                        w.undo.push_back(w.Capture());
                        w.Restore(w.redo.back());
                        w.redo.pop_back();
                        w.Refresh();
                        return InfoJson(w);
                    }});

    server.AddTool(
        {"terrain_preview",
         "Look at the terrain: a top-down picture returned as a PNG image (and saved to `path` if given). mode: shaded (height colours with hillshading, "
         "default), layers (the paint layers' colours), slope (green flat to red steep, 0-60 degrees) or height (a plain ramp). Any foliage is shown as "
         "dark dots.",
         Schema({{"mode", {{"type", "string"}, {"description", "shaded, layers, slope or height"}}},
                 {"size", {{"type", "integer"}, {"description", "Longest side in pixels, 64-1024 (default 512)"}}},
                 {"path", {{"type", "string"}, {"description", "Also save the PNG here"}}}}),
         [bench](const Json& args) -> Json {
             RequireTerrain(*bench);
             const Workbench& w = *bench;
             const std::string mode = args.contains("mode") && args["mode"].is_string() ? args["mode"].get<std::string>() : "shaded";
             if (mode != "shaded" && mode != "layers" && mode != "slope" && mode != "height") throw ToolError("\"mode\" must be shaded, layers, slope or height");
             const u32 size = static_cast<u32>(Number(args, "size", 512, 64, 1024));
             const f32 W = w.WorldWidth(), D = w.WorldDepth();
             if (!(W > 0.0f && D > 0.0f)) throw ToolError("The terrain has no area");
             const u32 pw = W >= D ? size : std::max(16u, static_cast<u32>(size * W / D));
             const u32 ph = D >= W ? size : std::max(16u, static_cast<u32>(size * D / W));
             const HeightStats st = Stats(w);
             const f32 span = std::max(st.max - st.min, 1e-4f);
             const Vec3 light = Vec3(-0.5f, 0.8f, -0.4f);
             const f32 light_len = std::sqrt(light.x * light.x + light.y * light.y + light.z * light.z);
             std::vector<u8> rgba(static_cast<usize>(pw) * ph * 4, 255);
             for (u32 py = 0; py < ph; ++py) {
                 for (u32 px = 0; px < pw; ++px) {
                     const f32 x = static_cast<f32>(px) / static_cast<f32>(pw - 1) * W;
                     const f32 z = static_cast<f32>(py) / static_cast<f32>(ph - 1) * D;
                     const f32 h = w.HeightAt(x, z);
                     const Vec3 n = w.NormalAt(x, z);
                     Vec3 color;
                     bool shade = true;
                     if (mode == "layers") {
                         const auto wt = w.splat.GetWeights(x / W, z / D);
                         f32 total = 0.0f;
                         color = Vec3(0, 0, 0);
                         for (usize i = 0; i < w.layers.size() && i < 4; ++i) {
                             color = color + w.layers[i].albedo_tint * wt[i];
                             total += wt[i];
                         }
                         if (total > 1e-4f) color = color * (1.0f / total);
                     } else if (mode == "slope") {
                         const f32 t = std::clamp(Workbench::SlopeDegrees(n) / 60.0f, 0.0f, 1.0f);
                         color = Vec3(0.2f + 0.8f * t, 0.8f - 0.6f * t, 0.2f);
                     } else {
                         color = HeightColor((h - st.min) / span);
                         shade = mode == "shaded";
                     }
                     if (shade) {
                         const f32 lambert = std::clamp((n.x * light.x + n.y * light.y + n.z * light.z) / light_len, 0.0f, 1.0f);
                         const f32 k = 0.35f + 0.65f * lambert;
                         color = color * k;
                     }
                     const usize o = (static_cast<usize>(py) * pw + px) * 4;
                     rgba[o] = static_cast<u8>(std::clamp(color.x, 0.0f, 1.0f) * 255.0f);
                     rgba[o + 1] = static_cast<u8>(std::clamp(color.y, 0.0f, 1.0f) * 255.0f);
                     rgba[o + 2] = static_cast<u8>(std::clamp(color.z, 0.0f, 1.0f) * 255.0f);
                 }
             }
             for (const tr::FoliageInstance& f : w.foliage.instances) { // dark dots for foliage
                 const i32 px = static_cast<i32>(f.position.x / W * static_cast<f32>(pw - 1) + 0.5f);
                 const i32 py = static_cast<i32>(f.position.z / D * static_cast<f32>(ph - 1) + 0.5f);
                 if (px < 0 || py < 0 || px >= static_cast<i32>(pw) || py >= static_cast<i32>(ph)) continue;
                 const usize o = (static_cast<usize>(py) * pw + static_cast<usize>(px)) * 4;
                 rgba[o] = 20;
                 rgba[o + 1] = 40;
                 rgba[o + 2] = 20;
             }
             const std::vector<u8> png = EncodePngRgba(rgba, pw, ph);
             if (png.empty()) throw ToolError("Could not encode the PNG");
             Json out = {{"mode", mode}, {"width", pw}, {"height", ph}, {"bytes", png.size()}, {"height_range", {{"min", st.min}, {"max", st.max}}},
                         {"world_size", {{"width", W}, {"depth", D}}}};
             if (args.contains("path") && args["path"].is_string()) {
                 WriteBytes(args["path"].get<std::string>(), png.data(), png.size());
                 out["path"] = args["path"];
             }
             out["mcp_content"] = Json::array({{{"type", "image"}, {"data", Base64Encode(png)}, {"mimeType", "image/png"}}});
             return out;
         }});

    server.AddTool(
        {"terrain_chunks",
         "The render plan for a camera: the chunk grid and, for a camera position [x, y, z], which LOD each chunk would use (by its centre's distance to the "
         "camera and the terrain's LOD distances), with the vertex and triangle counts per LOD, and the total triangles drawn.",
         Schema({{"camera", {{"type", "array"}, {"items", {{"type", "number"}}}, {"description", "[x, y, z] (default: above the middle of the terrain)"}}}}),
         [bench](const Json& args) -> Json {
             RequireTerrain(*bench);
             const Workbench& w = *bench;
             Vec3 camera(w.WorldWidth() * 0.5f, w.HeightAt(w.WorldWidth() * 0.5f, w.WorldDepth() * 0.5f) + 50.0f, w.WorldDepth() * 0.5f);
             if (args.contains("camera")) {
                 const Json& c = args["camera"];
                 if (!c.is_array() || c.size() != 3 || !c[0].is_number() || !c[1].is_number() || !c[2].is_number()) throw ToolError("\"camera\" must be [x, y, z]");
                 camera = Vec3(c[0].get<f32>(), c[1].get<f32>(), c[2].get<f32>());
             }
             const std::vector<tr::TerrainChunk> chunks = tr::BuildChunks(w.data, w.settings);
             if (chunks.empty()) throw ToolError("The terrain has no chunks");
             Json per_lod = Json::array();
             std::vector<usize> triangles_at_lod;
             for (u32 lod = 0; lod <= w.data.max_lod; ++lod) {
                 tr::TerrainChunk probe = chunks[0];
                 probe.lod_level = lod;
                 const usize verts = tr::GenerateChunkVertices(w.data, w.settings, probe).size() / 8;
                 const usize tris = tr::GenerateChunkIndices(w.data, probe).size() / 3;
                 triangles_at_lod.push_back(tris);
                 per_lod.push_back({{"lod", lod}, {"vertices", verts}, {"triangles", tris}});
             }
             std::vector<usize> histogram(w.data.max_lod + 1, 0);
             usize total_triangles = 0;
             for (const tr::TerrainChunk& c : chunks) {
                 const Vec3 centre((c.world_min.x + c.world_max.x) * 0.5f, (c.world_min.y + c.world_max.y) * 0.5f, (c.world_min.z + c.world_max.z) * 0.5f);
                 const f32 dx = centre.x - camera.x, dy = centre.y - camera.y, dz = centre.z - camera.z;
                 const u32 lod = std::min(tr::SelectLod(w.data, std::sqrt(dx * dx + dy * dy + dz * dz)), w.data.max_lod);
                 ++histogram[lod];
                 total_triangles += triangles_at_lod[lod];
             }
             return {{"camera", Vec3Json(camera)}, {"chunk_grid", {{"x", w.data.chunk_count_x}, {"z", w.data.chunk_count_z}}}, {"chunks", chunks.size()},
                     {"lod_distances", w.data.lod_distances}, {"per_lod", per_lod}, {"chunks_at_lod", histogram}, {"total_triangles", total_triangles}};
         }});

    server.AddTool(
        {"terrain_scatter_foliage",
         "Scatter foliage over the whole terrain, replacing any placed so far: a sample point every `spacing` metres, kept with probability `density` (times a "
         "noise density map if `noise` is given), only where the height and slope allow, up to max_instances. Each instance gets one of the `types` at random with "
         "its scale and rotation variation. Returns the counts, bounds and a per-type breakdown; terrain_preview shows the instances.",
         Schema({{"types", {{"type", "array"}, {"items", {{"type", "object"}}}, {"description", "[{scale_min?, scale_max?, rotation_range?, align_to_normal?, anchor_offset?}], default one"}}},
                 {"density", {{"type", "number"}, {"description", "0-1 (default 0.5)"}}},
                 {"spacing", {{"type", "number"}, {"description", "Metres between sample points, 0.25-50 (default 2)"}}},
                 {"seed", {{"type", "integer"}}},
                 {"max_instances", {{"type", "integer"}, {"description", "Up to 200000 (default 50000)"}}},
                 {"min_height", {{"type", "number"}}}, {"max_height", {{"type", "number"}}},
                 {"max_slope_degrees", {{"type", "number"}, {"description", "Skip steeper ground (default 90: none)"}}},
                 {"noise", {{"type", "object"}, {"description", "{octaves?, persistence?, seed?}: thin the density with a noise map"}}}}),
         [bench](const Json& args) -> Json {
             RequireTerrain(*bench);
             Workbench& w = *bench;
             tr::FoliageLayer layer;
             layer.name = "Foliage";
             layer.density = static_cast<f32>(Number(args, "density", 0.5, 0.0, 1.0));
             layer.max_instances = static_cast<u32>(Number(args, "max_instances", 50000, 1, 200000));
             layer.seed = static_cast<u32>(Number(args, "seed", 0, 0, 4.0e9));
             if (args.contains("types")) {
                 if (!args["types"].is_array() || args["types"].empty() || args["types"].size() > 16) throw ToolError("\"types\" must be 1 to 16 objects");
                 for (const Json& t : args["types"]) {
                     if (!t.is_object()) throw ToolError("Each type must be an object");
                     tr::FoliageType type;
                     type.scale_min = static_cast<f32>(Number(t, "scale_min", 0.8, 0.01, 1000.0));
                     type.scale_max = static_cast<f32>(Number(t, "scale_max", 1.2, 0.01, 1000.0));
                     if (type.scale_min > type.scale_max) throw ToolError("A type's scale_min is above its scale_max");
                     type.rotation_range = static_cast<f32>(Number(t, "rotation_range", 3.14159, 0.0, 7.0));
                     type.anchor_offset = static_cast<f32>(Number(t, "anchor_offset", 0.0, -1000.0, 1000.0));
                     if (t.contains("align_to_normal")) {
                         if (!t["align_to_normal"].is_boolean()) throw ToolError("\"align_to_normal\" must be true or false");
                         type.align_to_normal = t["align_to_normal"].get<bool>();
                     }
                     layer.types.push_back(type);
                 }
             } else {
                 layer.types.push_back(tr::FoliageType{});
             }
             const f32 spacing = static_cast<f32>(Number(args, "spacing", 2.0, 0.25, 50.0));
             const f32 min_h = static_cast<f32>(Number(args, "min_height", -1.0e9, -1.0e9, 1.0e9));
             const f32 max_h = static_cast<f32>(Number(args, "max_height", 1.0e9, -1.0e9, 1.0e9));
             const f32 max_slope = static_cast<f32>(Number(args, "max_slope_degrees", 90.0, 0.0, 90.0));

             const f32 W = w.WorldWidth(), D = w.WorldDepth();
             std::vector<Vec3> positions, normals;
             const usize cap = 2000000;
             for (f32 z = spacing * 0.5f; z < D && positions.size() < cap; z += spacing) {
                 for (f32 x = spacing * 0.5f; x < W && positions.size() < cap; x += spacing) {
                     const f32 h = w.HeightAt(x, z);
                     const Vec3 n = w.NormalAt(x, z);
                     if (h < min_h || h > max_h || Workbench::SlopeDegrees(n) > max_slope) continue;
                     positions.push_back(Vec3(x, h, z));
                     normals.push_back(n);
                 }
             }
             tr::DensityMap density;
             if (args.contains("noise")) {
                 if (!args["noise"].is_object()) throw ToolError("\"noise\" must be an object");
                 const Json& nz = args["noise"];
                 density = tr::GenerateDensityMap(std::max<u32>(1, static_cast<u32>(W)), std::max<u32>(1, static_cast<u32>(D)), std::max(W, D),
                                                  static_cast<u32>(Number(nz, "octaves", 3, 1, 8)), static_cast<f32>(Number(nz, "persistence", 0.5, 0.0, 1.0)),
                                                  static_cast<f32>(Number(nz, "seed", 1, 0, 1.0e6)));
             }
             // `density` is the share of sample points kept: thin the grid with it before placing.
             std::mt19937 rng(layer.seed);
             std::vector<Vec3> kept_positions, kept_normals;
             std::uniform_real_distribution<f32> unit(0.0f, 1.0f);
             for (usize i = 0; i < positions.size(); ++i) {
                 if (unit(rng) <= layer.density) {
                     kept_positions.push_back(positions[i]);
                     kept_normals.push_back(normals[i]);
                 }
             }
             tr::GenerateInstances(layer, density, kept_positions, kept_normals, rng);

             Json per_type = Json::array();
             for (usize t = 0; t < layer.types.size(); ++t) {
                 usize n = 0;
                 for (const tr::FoliageInstance& i : layer.instances) n += i.type_index == t ? 1 : 0;
                 per_type.push_back({{"type", t}, {"instances", n}});
             }
             Json out = {{"instances", layer.instances.size()}, {"sample_points", positions.size()}, {"kept_after_density", kept_positions.size()}, {"per_type", per_type}};
             if (!layer.instances.empty()) {
                 Vec3 lo, hi;
                 tr::ComputeBounds(layer.instances, &lo, &hi);
                 out["bounds"] = {{"min", Vec3Json(lo)}, {"max", Vec3Json(hi)}};
             }
             out["capped_at_max_instances"] = layer.instances.size() >= layer.max_instances;
             w.foliage = std::move(layer);
             return out;
         }});

    server.AddTool({"terrain_clear_foliage", "Remove all scattered foliage from the workbench.", Schema(Json::object()), [bench](const Json&) -> Json {
                        RequireTerrain(*bench);
                        const usize n = bench->foliage.instances.size();
                        bench->foliage.instances.clear();
                        return {{"removed", n}};
                    }});

    server.AddTool(
        {"terrain_export",
         "Save the terrain as files: <path>.r16 (heights, little-endian 16-bit across the height range), <path>.splat (RGBA8 paint weights) and "
         "<path>.terrain.json (sizes, height range, vertical scale, chunk size, layers). This is the MCP server's own format for keeping the workbench; "
         "nothing else in the engine reads it. Foliage and undo history are not saved.",
         Schema({{"path", {{"type", "string"}, {"description", "File path without extension"}}}}, {"path"}), [bench](const Json& args) -> Json {
             RequireTerrain(*bench);
             const Workbench& w = *bench;
             if (!args["path"].is_string() || args["path"].get<std::string>().empty()) throw ToolError("\"path\" must be a file path without extension");
             const std::string base = args["path"].get<std::string>();
             const std::vector<f32>& h = w.data.heightmap.heights;
             f32 lo = h.empty() ? 0.0f : h[0], hi = lo;
             for (f32 v : h) {
                 lo = std::min(lo, v);
                 hi = std::max(hi, v);
             }
             const f32 range = hi - lo;
             std::vector<u8> r16(h.size() * 2);
             for (usize i = 0; i < h.size(); ++i) {
                 const u16 q = range > 0.0f ? static_cast<u16>(std::lround((h[i] - lo) / range * 65535.0f)) : 0;
                 r16[i * 2] = static_cast<u8>(q & 255);
                 r16[i * 2 + 1] = static_cast<u8>(q >> 8);
             }
             WriteBytes(base + ".r16", r16.data(), r16.size());
             WriteBytes(base + ".splat", w.splat.pixels.data(), w.splat.pixels.size());
             Json meta = {{"format", "aether-mcp-terrain"}, {"version", 1},
                          {"width", w.data.heightmap.width}, {"depth", w.data.heightmap.height}, {"cell_size", w.data.heightmap.cell_size},
                          {"stored_min", lo}, {"stored_max", hi}, {"vertical_scale", w.settings.vertical_scale}, {"chunk_size", w.data.chunk_size},
                          {"splat_resolution", w.splat.width}, {"layers", LayersJson(w)}};
             const std::string text = meta.dump(2) + "\n";
             WriteBytes(base + ".terrain.json", text.data(), text.size());
             return {{"files", {base + ".r16", base + ".splat", base + ".terrain.json"}}, {"bytes", r16.size() + w.splat.pixels.size() + text.size()}};
         }});

    server.AddTool(
        {"terrain_import", "Load a terrain saved by terrain_export (<path>.r16, .splat and .terrain.json), replacing the workbench terrain.",
         Schema({{"path", {{"type", "string"}, {"description", "File path without extension"}}}}, {"path"}), [bench](const Json& args) -> Json {
             if (!args["path"].is_string() || args["path"].get<std::string>().empty()) throw ToolError("\"path\" must be a file path without extension");
             const std::string base = args["path"].get<std::string>();
             const std::vector<u8> meta_bytes = ReadBytes(base + ".terrain.json");
             const Json meta = Json::parse(std::string(meta_bytes.begin(), meta_bytes.end()), nullptr, false);
             if (!meta.is_object() || meta.value("format", std::string()) != "aether-mcp-terrain") throw ToolError(base + ".terrain.json is not a terrain saved by terrain_export");
             const u32 width = meta.value("width", 0u), depth = meta.value("depth", 0u);
             if (width < 2 || depth < 2 || width > kMaxSamples || depth > kMaxSamples) throw ToolError("The terrain size is out of range");
             const std::vector<u8> r16 = ReadBytes(base + ".r16");
             if (r16.size() != static_cast<usize>(width) * depth * 2) throw ToolError(base + ".r16 does not match the terrain size");

             auto next = std::make_unique<Workbench>();
             next->settings.vertical_scale = meta.value("vertical_scale", 1.0f);
             const u32 chunk = meta.value("chunk_size", 33u);
             next->settings.verts_per_chunk = chunk;
             next->data.chunk_size = chunk;
             tr::Heightmap hm;
             hm.width = width;
             hm.height = depth;
             hm.cell_size = meta.value("cell_size", 1.0f);
             next->settings.chunk_world_size = static_cast<f32>(chunk - 1) * hm.cell_size;
             const f32 lo = meta.value("stored_min", 0.0f), hi = meta.value("stored_max", 0.0f);
             hm.heights.resize(static_cast<usize>(width) * depth);
             for (usize i = 0; i < hm.heights.size(); ++i) {
                 const u16 q = static_cast<u16>(r16[i * 2] | (r16[i * 2 + 1] << 8));
                 hm.heights[i] = lo + (hi - lo) * (static_cast<f32>(q) / 65535.0f);
             }
             tr::InitTerrainData(next->data, hm, next->settings);
             next->layers.clear();
             if (meta.contains("layers") && meta["layers"].is_array()) {
                 for (const Json& l : meta["layers"]) {
                     tr::SplatmapLayer layer;
                     layer.name = l.value("name", std::string("Layer"));
                     layer.albedo_texture_id = 0;
                     layer.normal_texture_id = 0;
                     if (l.contains("albedo") && l["albedo"].is_array() && l["albedo"].size() == 3) layer.albedo_tint = Vec3(l["albedo"][0].get<f32>(), l["albedo"][1].get<f32>(), l["albedo"][2].get<f32>());
                     layer.metallic = l.value("metallic", 0.0f);
                     layer.roughness = l.value("roughness", 0.85f);
                     next->layers.push_back(layer);
                 }
             }
             if (next->layers.empty() || next->layers.size() > 4) next->layers = DefaultLayers();
             const u32 res = meta.value("splat_resolution", 0u);
             const std::vector<u8> splat = ReadBytes(base + ".splat");
             if (res > 0 && splat.size() == static_cast<usize>(res) * res * 4) {
                 next->splat.width = next->splat.height = res;
                 next->splat.pixels = splat;
             } else {
                 RebuildSplat(*next);
             }
             next->data.layers.clear();
             for (const tr::SplatmapLayer& l : next->layers) {
                 tr::TerrainLayer tl;
                 tl.name = l.name;
                 tl.albedo = l.albedo_tint;
                 tl.metallic = l.metallic;
                 tl.roughness = l.roughness;
                 next->data.layers.push_back(tl);
             }
             next->exists = true;
             *bench = std::move(*next);
             return InfoJson(*bench);
         }});
}

} // namespace aether::mcp
