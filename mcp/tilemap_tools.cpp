#include "tilemap_tools.h"

#include "image_util.h"
#include "project_host.h"

#include "aether/assets/image.h"
#include "aether/sprite2d/tilemap.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <sstream>

namespace aether::mcp {

namespace {

using namespace detail;
namespace sp = aether::sprite2d;

constexpr u32 kMaxCells = 1000000;
constexpr const char* kSymbols = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
constexpr const char* kAutotileSymbols = "~=+*%@&$^";

std::string WithExtension(std::string rel, const char* ext) {
    if (rel.empty()) throw ToolError("\"path\" must name a file under Content/");
    if (fs::path(rel).extension() != ext) rel += ext;
    return rel;
}

// The asset a tool names, which must be of the given importer.
const AssetRecord& RequireOfType(OpenProject& p, const Json& args, const char* key, const char* importer) {
    const AssetRecord& r = FindAsset(p, RequireString(args, key));
    if (r.importer != importer) throw ToolError(r.path + " is not a " + importer + " (it is a " + r.importer + ")");
    return r;
}

sp::Tileset LoadTilesetAsset(OpenProject& p, const AssetRecord& r) {
    sp::Tileset t;
    std::string error;
    if (!sp::LoadTileset(p.database->SourcePath(r.guid), t, &error)) throw ToolError("Could not read " + r.path + ": " + error);
    return t;
}

sp::TilemapData LoadTilemapAsset(OpenProject& p, const AssetRecord& r) {
    sp::TilemapData m;
    std::string error;
    if (!sp::LoadTilemap(p.database->SourcePath(r.guid), m, &error)) throw ToolError("Could not read " + r.path + ": " + error);
    return m;
}

void SaveWithScan(OpenProject& p, const std::string& rel, const std::function<bool(const fs::path&, std::string*)>& save) {
    const fs::path file = p.paths.content / rel;
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    std::string error;
    if (!save(file, &error)) throw ToolError("Could not write " + rel + ": " + error);
    p.database->Scan();
}

// A cell as JSON: a tile number, -1 for empty, or "autotile:N".
Json CellJson(i32 cell) {
    if (cell == sp::kEmpty) return -1;
    if (sp::IsAutotileCell(cell)) return "autotile:" + std::to_string(sp::AutotileIndex(cell));
    return cell;
}

i32 ParseCell(const Json& j, const std::string& what) {
    if (j.is_number_integer()) {
        const long long v = j.get<long long>();
        if (v < -1 || v > 1000000) throw ToolError(what + " must be a tile number (0 or more), -1 for empty, or \"autotile:N\"");
        return static_cast<i32>(v);
    }
    if (j.is_string()) {
        const std::string s = j.get<std::string>();
        if (s == "empty" || s == ".") return sp::kEmpty;
        if (s.rfind("autotile:", 0) == 0) {
            try {
                const int n = std::stoi(s.substr(9));
                if (n >= 0 && n < 10000) return sp::AutotileCell(static_cast<usize>(n));
            } catch (...) {}
        }
    }
    if (j.is_object() && j.contains("autotile") && j["autotile"].is_number_integer() && j["autotile"].get<long long>() >= 0) {
        return sp::AutotileCell(static_cast<usize>(j["autotile"].get<long long>()));
    }
    throw ToolError(what + " must be a tile number (0 or more), -1 for empty, or \"autotile:N\"");
}

usize LayerIndex(const sp::TilemapData& m, const Json& args) {
    if (!args.contains("layer")) {
        if (m.layers.empty()) throw ToolError("The tilemap has no layers (add one with the add_layer operation)");
        return 0;
    }
    if (args["layer"].is_string()) {
        for (usize i = 0; i < m.layers.size(); ++i) {
            if (m.layers[i].name == args["layer"].get<std::string>()) return i;
        }
        throw ToolError("No layer named \"" + args["layer"].get<std::string>() + "\"");
    }
    if (args["layer"].is_number_integer() && args["layer"].get<long long>() >= 0 && args["layer"].get<usize>() < m.layers.size()) return args["layer"].get<usize>();
    throw ToolError("\"layer\" must be a layer name or a valid index");
}

std::string CellName(i32 cell) { return cell == sp::kEmpty ? "empty" : (sp::IsAutotileCell(cell) ? "autotile:" + std::to_string(sp::AutotileIndex(cell)) : std::to_string(cell)); }

// What is wrong with a map against its tileset (a missing tileset gives no tile checks).
Json MapProblems(const sp::TilemapData& m, const sp::Tileset* tileset) {
    Json problems = Json::array();
    if (m.width == 0 || m.height == 0) problems.push_back("the map has no size");
    for (const sp::TileLayer& layer : m.layers) {
        if (layer.cells.size() != static_cast<usize>(m.width) * m.height) problems.push_back("layer \"" + layer.name + "\" has " + std::to_string(layer.cells.size()) + " cells, not width x height");
    }
    if (tileset == nullptr) return problems;
    const u32 tiles = tileset->TileCount();
    std::set<std::string> reported;
    for (const sp::TileLayer& layer : m.layers) {
        for (const i32 cell : layer.cells) {
            if (cell == sp::kEmpty) continue;
            if (sp::IsAutotileCell(cell)) {
                if (sp::AutotileIndex(cell) >= tileset->autotiles.size() && reported.insert("a" + std::to_string(cell)).second)
                    problems.push_back("layer \"" + layer.name + "\" uses autotile " + std::to_string(sp::AutotileIndex(cell)) + ", but the tileset has " + std::to_string(tileset->autotiles.size()));
            } else if (tiles > 0 && static_cast<u32>(cell) >= tiles && reported.insert("t" + std::to_string(cell)).second) {
                problems.push_back("layer \"" + layer.name + "\" uses tile " + std::to_string(cell) + ", but the tileset has " + std::to_string(tiles) + " tiles (0 to " + std::to_string(tiles - 1) + ")");
            }
        }
    }
    return problems;
}

// The tileset a map points at, if it is in the project.
std::optional<sp::Tileset> TilesetOf(OpenProject& p, const sp::TilemapData& m, std::string* path = nullptr) {
    if (m.tileset.IsNull()) return std::nullopt;
    const AssetRecord* r = p.database->Find(m.tileset);
    if (r == nullptr || r->importer != "Tileset") return std::nullopt;
    sp::Tileset t;
    if (!sp::LoadTileset(p.database->SourcePath(r->guid), t)) return std::nullopt;
    if (path != nullptr) *path = r->path;
    return t;
}

} // namespace

Json TilesetSampleJson() {
    sp::Tileset t;
    t.texture_width = 64;
    t.texture_height = 64;
    t.tile_width = 16;
    t.tile_height = 16;
    t.SetSolid(1, true);
    t.SetSolid(2, true);
    t.AddAutotile("Ground", {5, 5, 6, 4, 5, 5, 7, 8, 9, 9, 10, 11, 9, 9, 12, 1});
    return sp::TilesetToJson(t);
}

Json TilemapSampleJson() {
    sp::TilemapData m;
    m.Resize(8, 4);
    const usize ground = m.AddLayer("Ground");
    m.Fill(ground, 0, 0, 7, 0, sp::AutotileCell(0));
    m.Set(ground, 3, 1, 1);
    m.Set(ground, 4, 1, 1);
    const usize props = m.AddLayer("Props");
    m.layers[props].collides = false;
    m.Set(props, 1, 1, 3);
    return sp::TilemapToJson(m);
}

void RegisterTilemapTools(McpServer& server, std::shared_ptr<AssetHost> host) {
    // ---- tilesets ---------------------------------------------------------------------------------------
    server.AddTool(
        {"tileset_create",
         "Make a tileset (.atileset) from a texture in the open project: the texture is cut into equal tiles, numbered in reading order from 0 at the top left. The "
         "texture's size is read from the file. Returns the grid (columns, rows, tile count). Tiles can be flagged solid and autotile rules added with tileset_set.",
         Schema({{"path", {{"type", "string"}, {"description", "Under Content/, e.g. Tilesets/Ground (.atileset added)"}}},
                 {"texture", {{"type", "string"}, {"description", "A texture asset in the project (png, tga...)"}}},
                 {"tile_width", {{"type", "integer"}}}, {"tile_height", {{"type", "integer"}}},
                 {"margin", {{"type", "integer"}, {"description", "Pixels around the whole sheet"}}}, {"spacing", {{"type", "integer"}, {"description", "Pixels between tiles"}}},
                 {"solid_tiles", {{"type", "array"}, {"items", {{"type", "integer"}}}}}, {"overwrite", {{"type", "boolean"}}}},
                {"path", "texture", "tile_width", "tile_height"}),
         [host](const Json& args) -> Json {
             OpenProject& p = host->Require();
             const std::string rel = WithExtension(CleanRelative(RequireString(args, "path")), ".atileset");
             const AssetRecord& texture_record = RequireOfType(p, args, "texture", "Texture");
             // Copied: a scan after saving rebuilds the database's records, so a reference into it would dangle.
             const std::string texture_path = texture_record.path;
             const AssetGuid texture_guid = texture_record.guid;
             const fs::path texture_file = p.database->SourcePath(texture_guid);
             sp::Tileset t;
             t.texture = texture_guid;
             auto dim = [&](const char* key, long long lo, long long hi) {
                 if (!args.contains(key)) return 0u;
                 if (!args[key].is_number_integer() || args[key].get<long long>() < lo || args[key].get<long long>() > hi) throw ToolError(std::string("\"") + key + "\" must be an integer from " + std::to_string(lo) + " to " + std::to_string(hi));
                 return args[key].get<u32>();
             };
             t.tile_width = dim("tile_width", 1, 4096);
             t.tile_height = dim("tile_height", 1, 4096);
             t.margin = dim("margin", 0, 4096);
             t.spacing = dim("spacing", 0, 4096);
             assets::ImageData image;
             if (!assets::DecodeImageFile(texture_file.string(), image)) throw ToolError("Could not read the texture " + texture_path + " to get its size");
             t.texture_width = image.width;
             t.texture_height = image.height;
             if (t.Columns() == 0 || t.Rows() == 0) throw ToolError("The tile size does not fit the " + std::to_string(image.width) + " x " + std::to_string(image.height) + " texture");
             if (args.contains("solid_tiles")) {
                 if (!args["solid_tiles"].is_array()) throw ToolError("\"solid_tiles\" must be an array of tile numbers");
                 for (const Json& s : args["solid_tiles"]) {
                     if (!s.is_number_integer() || s.get<long long>() < 0 || s.get<usize>() >= t.TileCount()) throw ToolError("\"solid_tiles\" holds tile numbers from 0 to " + std::to_string(t.TileCount() - 1));
                     t.SetSolid(s.get<i32>(), true);
                 }
             }
             std::error_code ec;
             if (fs::exists(p.paths.content / rel, ec) && !OptBool(args, "overwrite", false)) throw ToolError(rel + " already exists (pass overwrite)");
             SaveWithScan(p, rel, [&](const fs::path& f, std::string* e) { return sp::SaveTileset(f, t, e); });
             Json out = {{"path", rel}, {"texture", texture_path}, {"texture_size", {{"width", image.width}, {"height", image.height}}}, {"columns", t.Columns()}, {"rows", t.Rows()}, {"tile_count", t.TileCount()}};
             if (const AssetRecord* r = p.database->FindByPath(rel)) out["guid"] = assets::ToString(r->guid);
             return out;
         }});

    server.AddTool({"tileset_info", "A tileset: its texture, tile size, grid (columns, rows, tile count), which tiles are solid and its autotiles (name and the tile for each neighbour mask).",
                    Schema({{"asset", {{"type", "string"}, {"description", "Content path or GUID of an .atileset"}}}}, {"asset"}), [host](const Json& args) -> Json {
                        OpenProject& p = host->Require();
                        const AssetRecord& r = RequireOfType(p, args, "asset", "Tileset");
                        const sp::Tileset t = LoadTilesetAsset(p, r);
                        Json solid = Json::array(), autotiles = Json::array();
                        for (usize i = 0; i < t.solid.size(); ++i) {
                            if (t.solid[i] != 0) solid.push_back(i);
                        }
                        for (usize i = 0; i < t.autotiles.size(); ++i) autotiles.push_back({{"index", i}, {"name", t.autotiles[i].name}, {"tiles", t.autotiles[i].tiles}});
                        Json out = {{"path", r.path}, {"tile_size", {{"width", t.tile_width}, {"height", t.tile_height}}}, {"margin", t.margin}, {"spacing", t.spacing},
                                    {"texture_size", {{"width", t.texture_width}, {"height", t.texture_height}}}, {"columns", t.Columns()}, {"rows", t.Rows()},
                                    {"tile_count", t.TileCount()}, {"solid_tiles", solid}, {"autotiles", autotiles}};
                        if (const AssetRecord* tex = p.database->Find(t.texture)) out["texture"] = tex->path;
                        else out["texture"] = nullptr;
                        return out;
                    }});

    server.AddTool(
        {"tileset_set",
         "Edit a tileset and save it: `solid_tiles` replaces the set of solid tiles (they block in collision), `add_solid` / `remove_solid` change it, and `autotiles` "
         "replaces the autotile rules: [{name, tiles: [16 tile numbers]}] where the tile for each mask of same-kind neighbours (north 1, east 2, south 4, west 8) "
         "is listed in mask order 0..15 (0 an island, 15 surrounded; -1 draws nothing). Tile numbers must exist in the tileset.",
         Schema({{"asset", {{"type", "string"}}}, {"solid_tiles", {{"type", "array"}, {"items", {{"type", "integer"}}}}}, {"add_solid", {{"type", "array"}, {"items", {{"type", "integer"}}}}},
                 {"remove_solid", {{"type", "array"}, {"items", {{"type", "integer"}}}}}, {"autotiles", {{"type", "array"}}}},
                {"asset"}),
         [host](const Json& args) -> Json {
             OpenProject& p = host->Require();
             const AssetRecord& r = RequireOfType(p, args, "asset", "Tileset");
             const std::string asset_path = r.path; // copied: the scan after saving rebuilds the records
             sp::Tileset t = LoadTilesetAsset(p, r);
             const u32 count = t.TileCount();
             auto tiles_of = [&](const char* key) {
                 std::vector<i32> out;
                 if (!args[key].is_array()) throw ToolError(std::string("\"") + key + "\" must be an array of tile numbers");
                 for (const Json& s : args[key]) {
                     if (!s.is_number_integer() || s.get<long long>() < 0 || (count > 0 && s.get<usize>() >= count)) throw ToolError(std::string("\"") + key + "\" holds tile numbers from 0 to " + std::to_string(count ? count - 1 : 0));
                     out.push_back(s.get<i32>());
                 }
                 return out;
             };
             if (args.contains("solid_tiles")) {
                 t.solid.clear();
                 for (const i32 tile : tiles_of("solid_tiles")) t.SetSolid(tile, true);
             }
             if (args.contains("add_solid")) for (const i32 tile : tiles_of("add_solid")) t.SetSolid(tile, true);
             if (args.contains("remove_solid")) for (const i32 tile : tiles_of("remove_solid")) t.SetSolid(tile, false);
             if (args.contains("autotiles")) {
                 if (!args["autotiles"].is_array()) throw ToolError("\"autotiles\" must be an array of {name, tiles}");
                 std::vector<sp::Autotile> next;
                 for (const Json& a : args["autotiles"]) {
                     if (!a.is_object() || !a.contains("tiles") || !a["tiles"].is_array() || a["tiles"].size() != 16) throw ToolError("Each autotile needs 16 \"tiles\" (one per neighbour mask)");
                     sp::Autotile at;
                     at.name = a.value("name", std::string());
                     for (usize i = 0; i < 16; ++i) {
                         const Json& v = a["tiles"][i];
                         if (!v.is_number_integer() || v.get<long long>() < -1 || (count > 0 && v.get<long long>() >= static_cast<long long>(count))) throw ToolError("Autotile \"" + at.name + "\" tile " + std::to_string(i) + " must be -1 or a tile number from 0 to " + std::to_string(count ? count - 1 : 0));
                         at.tiles[i] = v.get<i32>();
                     }
                     next.push_back(at);
                 }
                 t.autotiles = std::move(next);
             }
             SaveWithScan(p, asset_path, [&](const fs::path& f, std::string* e) { return sp::SaveTileset(f, t, e); });
             Json solid = Json::array();
             for (usize i = 0; i < t.solid.size(); ++i) {
                 if (t.solid[i] != 0) solid.push_back(i);
             }
             return {{"path", asset_path}, {"solid_tiles", solid}, {"autotiles", t.autotiles.size()}};
         }});

    // ---- tilemaps ---------------------------------------------------------------------------------------
    server.AddTool(
        {"tilemap_create",
         "Make a tilemap (.atilemap) over a tileset: width x height cells (row 0 is the bottom row, x grows right) with the named layers (default one, \"Ground\"), all empty.",
         Schema({{"path", {{"type", "string"}, {"description", "Under Content/, e.g. Levels/Cave (.atilemap added)"}}},
                 {"tileset", {{"type", "string"}, {"description", "A tileset asset in the project"}}},
                 {"width", {{"type", "integer"}}}, {"height", {{"type", "integer"}}},
                 {"layers", {{"type", "array"}, {"items", {{"type", "string"}}}}}, {"overwrite", {{"type", "boolean"}}}},
                {"path", "tileset", "width", "height"}),
         [host](const Json& args) -> Json {
             OpenProject& p = host->Require();
             const std::string rel = WithExtension(CleanRelative(RequireString(args, "path")), ".atilemap");
             const AssetRecord& tileset_record = RequireOfType(p, args, "tileset", "Tileset");
             const std::string tileset_path = tileset_record.path; // copied: the scan after saving rebuilds the records
             const AssetGuid tileset_guid = tileset_record.guid;
             if (!args["width"].is_number_integer() || !args["height"].is_number_integer() || args["width"].get<long long>() < 1 || args["height"].get<long long>() < 1) throw ToolError("\"width\" and \"height\" must be integers of at least 1");
             if (static_cast<unsigned long long>(args["width"].get<long long>()) * static_cast<unsigned long long>(args["height"].get<long long>()) > kMaxCells) throw ToolError("The map would have more than a million cells");
             sp::TilemapData m;
             m.tileset = tileset_guid;
             m.Resize(args["width"].get<u32>(), args["height"].get<u32>());
             std::vector<std::string> names = {"Ground"};
             if (args.contains("layers")) {
                 names = OptStrings(args, "layers");
                 if (names.empty() || names.size() > 16) throw ToolError("\"layers\" must name 1 to 16 layers");
             }
             for (const std::string& n : names) m.AddLayer(n);
             std::error_code ec;
             if (fs::exists(p.paths.content / rel, ec) && !OptBool(args, "overwrite", false)) throw ToolError(rel + " already exists (pass overwrite)");
             SaveWithScan(p, rel, [&](const fs::path& f, std::string* e) { return sp::SaveTilemap(f, m, e); });
             Json out = {{"path", rel}, {"tileset", tileset_path}, {"width", m.width}, {"height", m.height}, {"layers", names}};
             if (const AssetRecord* r = p.database->FindByPath(rel)) out["guid"] = assets::ToString(r->guid);
             return out;
         }});

    server.AddTool(
        {"tilemap_info",
         "A tilemap: its size, tileset, layers (cells in use, whether they collide), how often each tile and autotile is used, and problems: a tile number the tileset does not "
         "have, an autotile it lacks, a layer of the wrong size, or a tileset that is not in the project.",
         Schema({{"asset", {{"type", "string"}, {"description", "Content path or GUID of an .atilemap"}}}}, {"asset"}), [host](const Json& args) -> Json {
             OpenProject& p = host->Require();
             const AssetRecord& r = RequireOfType(p, args, "asset", "Tilemap");
             const sp::TilemapData m = LoadTilemapAsset(p, r);
             std::string tileset_path;
             const std::optional<sp::Tileset> tileset = TilesetOf(p, m, &tileset_path);
             Json layers = Json::array();
             for (const sp::TileLayer& layer : m.layers) {
                 std::map<std::string, usize> use;
                 usize used = 0;
                 for (const i32 cell : layer.cells) {
                     if (cell == sp::kEmpty) continue;
                     ++used;
                     ++use[CellName(cell)];
                 }
                 Json histogram = Json::object();
                 for (const auto& [name, n] : use) histogram[name] = n;
                 layers.push_back({{"name", layer.name}, {"collides", layer.collides}, {"cells_used", used}, {"usage", histogram}});
             }
             Json problems = MapProblems(m, tileset ? &*tileset : nullptr);
             if (!tileset) problems.push_back(m.tileset.IsNull() ? "the map has no tileset" : "the map's tileset " + assets::ToString(m.tileset) + " is not a tileset in the project");
             Json out = {{"path", r.path}, {"width", m.width}, {"height", m.height}, {"layers", layers}, {"problems", problems}, {"ok", problems.empty()}};
             out["tileset"] = tileset ? Json(tileset_path) : Json(nullptr);
             if (tileset) out["tileset_tiles"] = tileset->TileCount();
             return out;
         }});

    server.AddTool(
        {"tilemap_ascii",
         "A layer as text, top row first (so it reads like the level looks): one symbol per cell, '.' for empty, a letter or digit per distinct tile (in order of first use) and "
         "~ = + * for autotiles. Returns the rows and the legend (symbol -> cell), so tilemap_paint's ascii operation can write it back. With `solid`, a second grid marks cells "
         "that block ('#') through the tileset's solid tiles on the colliding layers. `legend` fixes symbols you choose: {\"#\": 5, \"~\": \"autotile:0\"}.",
         Schema({{"asset", {{"type", "string"}}}, {"layer", {{"description", "Layer name or index (default: the first)"}}}, {"solid", {{"type", "boolean"}}}, {"legend", {{"type", "object"}}}}, {"asset"}),
         [host](const Json& args) -> Json {
             OpenProject& p = host->Require();
             const AssetRecord& r = RequireOfType(p, args, "asset", "Tilemap");
             const sp::TilemapData m = LoadTilemapAsset(p, r);
             const usize layer = LayerIndex(m, args);
             std::map<i32, char> symbol_of;
             std::set<char> taken = {'.'};
             if (args.contains("legend")) {
                 if (!args["legend"].is_object()) throw ToolError("\"legend\" must be an object of symbol to cell");
                 for (auto& [sym, value] : args["legend"].items()) {
                     if (sym.size() != 1) throw ToolError("Legend symbols are single characters");
                     symbol_of[ParseCell(value, "A legend value")] = sym[0];
                     taken.insert(sym[0]);
                 }
             }
             usize next_tile = 0, next_auto = 0;
             bool overflow = false;
             auto assign = [&](i32 cell) {
                 const auto it = symbol_of.find(cell);
                 if (it != symbol_of.end()) return it->second;
                 const std::string pool = sp::IsAutotileCell(cell) ? std::string(kAutotileSymbols) : std::string(kSymbols);
                 usize& cursor = sp::IsAutotileCell(cell) ? next_auto : next_tile;
                 while (cursor < pool.size() && taken.count(pool[cursor])) ++cursor;
                 char c = '?';
                 if (cursor < pool.size()) c = pool[cursor++];
                 else overflow = true;
                 if (c != '?') taken.insert(c);
                 symbol_of[cell] = c;
                 return c;
             };
             std::vector<std::string> rows;
             for (i32 y = static_cast<i32>(m.height) - 1; y >= 0; --y) {
                 std::string row;
                 for (i32 x = 0; x < static_cast<i32>(m.width); ++x) {
                     const i32 cell = m.Get(layer, x, y);
                     row.push_back(cell == sp::kEmpty ? '.' : assign(cell));
                 }
                 rows.push_back(row);
             }
             Json legend = Json::object();
             for (const auto& [cell, sym] : symbol_of) legend[std::string(1, sym)] = CellJson(cell);
             legend["."] = -1;
             Json out = {{"layer", m.layers[layer].name}, {"width", m.width}, {"height", m.height}, {"rows", rows}, {"legend", legend}, {"origin", "the bottom-left cell is x 0, y 0; the first row is the top row"}};
             if (overflow) out["note"] = "more distinct tiles than symbols: the extras show as '?' (give a legend to choose)";
             if (OptBool(args, "solid", false)) {
                 const std::optional<sp::Tileset> tileset = TilesetOf(p, m);
                 if (!tileset) throw ToolError("\"solid\" needs the map's tileset to be in the project");
                 std::vector<std::string> grid;
                 for (i32 y = static_cast<i32>(m.height) - 1; y >= 0; --y) {
                     std::string row;
                     for (i32 x = 0; x < static_cast<i32>(m.width); ++x) row.push_back(sp::IsSolidAt(m, *tileset, x, y) ? '#' : '.');
                     grid.push_back(row);
                 }
                 out["solid_rows"] = grid;
             }
             return out;
         }});

    server.AddTool(
        {"tilemap_paint",
         "Edit a tilemap and save it: `ops` run in order on a copy, and the file is written only if every one succeeds. A cell is a tile number, -1 for empty or \"autotile:N\". "
         "Operations (one key each): {set: [x, y], cell}; {fill: [x0, y0, x1, y1], cell} (a rectangle, clamped to the map); {line: [x0, y0, x1, y1], cell}; "
         "{replace: {from, to}} (every cell of one kind); {ascii: [rows, top first], legend: {symbol: cell}, at: [x, y]} stamps text with its bottom-left at `at` (default [0, 0]; "
         "symbols missing from the legend are an error; '.' is empty unless the legend says otherwise); {clear: true}; {resize: [width, height]} (keeps what fits); {add_layer: name}. "
         "Returns the cells changed and warnings for tile numbers the tileset lacks.",
         Schema({{"asset", {{"type", "string"}}}, {"layer", {{"description", "Layer name or index for the operations (default: the first)"}}}, {"ops", {{"type", "array"}, {"description", "[{op...}, ...]"}}}}, {"asset", "ops"}),
         [host](const Json& args) -> Json {
             OpenProject& p = host->Require();
             const AssetRecord& r = RequireOfType(p, args, "asset", "Tilemap");
             const std::string asset_path = r.path; // copied: the scan after saving rebuilds the records
             sp::TilemapData m = LoadTilemapAsset(p, r);
             if (!args["ops"].is_array() || args["ops"].empty() || args["ops"].size() > 500) throw ToolError("\"ops\" must be 1 to 500 operations");
             const std::vector<std::vector<i32>> before = [&] {
                 std::vector<std::vector<i32>> v;
                 for (const sp::TileLayer& l : m.layers) v.push_back(l.cells);
                 return v;
             }();
             const u32 old_w = m.width, old_h = m.height, old_layers = static_cast<u32>(m.layers.size());
             usize layer = 0;
             bool have_layer = false;
             auto current = [&]() -> usize {
                 if (!have_layer) {
                     layer = LayerIndex(m, args);
                     have_layer = true;
                 }
                 return layer;
             };
             auto coords = [&](const Json& j, usize n, const char* what) {
                 if (!j.is_array() || j.size() != n) throw ToolError(std::string(what) + " must be an array of " + std::to_string(n) + " integers");
                 std::vector<i32> out;
                 for (const Json& v : j) {
                     if (!v.is_number_integer()) throw ToolError(std::string(what) + " must be integers");
                     out.push_back(v.get<i32>());
                 }
                 return out;
             };
             usize ops_run = 0;
             for (const Json& op : args["ops"]) {
                 if (!op.is_object()) throw ToolError("Each operation must be an object");
                 const std::string where = "operation " + std::to_string(ops_run + 1);
                 if (op.contains("set")) {
                     const std::vector<i32> c = coords(op["set"], 2, "\"set\"");
                     if (!op.contains("cell")) throw ToolError(where + ": \"set\" needs \"cell\"");
                     if (!m.Set(current(), c[0], c[1], ParseCell(op["cell"], "\"cell\""))) throw ToolError(where + ": (" + std::to_string(c[0]) + ", " + std::to_string(c[1]) + ") is outside the " + std::to_string(m.width) + " x " + std::to_string(m.height) + " map");
                 } else if (op.contains("fill")) {
                     const std::vector<i32> c = coords(op["fill"], 4, "\"fill\"");
                     if (!op.contains("cell")) throw ToolError(where + ": \"fill\" needs \"cell\"");
                     m.Fill(current(), std::min(c[0], c[2]), std::min(c[1], c[3]), std::max(c[0], c[2]), std::max(c[1], c[3]), ParseCell(op["cell"], "\"cell\""));
                 } else if (op.contains("line")) {
                     const std::vector<i32> c = coords(op["line"], 4, "\"line\"");
                     if (!op.contains("cell")) throw ToolError(where + ": \"line\" needs \"cell\"");
                     const i32 cell = ParseCell(op["cell"], "\"cell\"");
                     const i32 dx = std::abs(c[2] - c[0]), dy = std::abs(c[3] - c[1]);
                     const i32 sx = c[0] < c[2] ? 1 : -1, sy = c[1] < c[3] ? 1 : -1;
                     i32 x = c[0], y = c[1], err = dx - dy;
                     for (;;) {
                         m.Set(current(), x, y, cell); // cells off the map are skipped
                         if (x == c[2] && y == c[3]) break;
                         const i32 e2 = 2 * err;
                         if (e2 > -dy) { err -= dy; x += sx; }
                         if (e2 < dx) { err += dx; y += sy; }
                     }
                 } else if (op.contains("replace")) {
                     if (!op["replace"].is_object() || !op["replace"].contains("from") || !op["replace"].contains("to")) throw ToolError(where + ": \"replace\" needs {from, to}");
                     const i32 from = ParseCell(op["replace"]["from"], "\"from\""), to = ParseCell(op["replace"]["to"], "\"to\"");
                     for (i32& cell : m.layers[current()].cells) if (cell == from) cell = to;
                 } else if (op.contains("ascii")) {
                     if (!op["ascii"].is_array() || op["ascii"].empty()) throw ToolError(where + ": \"ascii\" must be an array of text rows");
                     std::map<char, i32> legend = {{'.', sp::kEmpty}};
                     if (op.contains("legend")) {
                         if (!op["legend"].is_object()) throw ToolError(where + ": \"legend\" must be an object of symbol to cell");
                         for (auto& [sym, value] : op["legend"].items()) {
                             if (sym.size() != 1) throw ToolError(where + ": legend symbols are single characters");
                             legend[sym[0]] = ParseCell(value, "A legend value");
                         }
                     }
                     const std::vector<i32> at = op.contains("at") ? coords(op["at"], 2, "\"at\"") : std::vector<i32>{0, 0};
                     const i32 rows = static_cast<i32>(op["ascii"].size());
                     for (i32 ri = 0; ri < rows; ++ri) {
                         if (!op["ascii"][static_cast<usize>(ri)].is_string()) throw ToolError(where + ": \"ascii\" rows must be strings");
                         const std::string row = op["ascii"][static_cast<usize>(ri)].get<std::string>();
                         const i32 y = at[1] + (rows - 1 - ri); // the first row is the top
                         for (usize ci = 0; ci < row.size(); ++ci) {
                             if (row[ci] == ' ') continue; // a space leaves the cell alone
                             const auto it = legend.find(row[ci]);
                             if (it == legend.end()) throw ToolError(where + ": the symbol '" + std::string(1, row[ci]) + "' is not in the legend");
                             if (!m.InBounds(at[0] + static_cast<i32>(ci), y)) throw ToolError(where + ": row " + std::to_string(ri + 1) + " reaches (" + std::to_string(at[0] + static_cast<i32>(ci)) + ", " + std::to_string(y) + "), outside the " + std::to_string(m.width) + " x " + std::to_string(m.height) + " map");
                             m.Set(current(), at[0] + static_cast<i32>(ci), y, it->second);
                         }
                     }
                 } else if (op.contains("clear")) {
                     for (i32& cell : m.layers[current()].cells) cell = sp::kEmpty;
                 } else if (op.contains("resize")) {
                     const std::vector<i32> c = coords(op["resize"], 2, "\"resize\"");
                     if (c[0] < 1 || c[1] < 1 || static_cast<unsigned long long>(c[0]) * static_cast<unsigned long long>(c[1]) > kMaxCells) throw ToolError(where + ": the size must be at least 1 x 1 and at most a million cells");
                     m.Resize(static_cast<u32>(c[0]), static_cast<u32>(c[1]));
                 } else if (op.contains("add_layer")) {
                     if (!op["add_layer"].is_string() || op["add_layer"].get<std::string>().empty()) throw ToolError(where + ": \"add_layer\" must be a name");
                     if (m.layers.size() >= 16) throw ToolError("A tilemap has at most 16 layers");
                     m.AddLayer(op["add_layer"].get<std::string>());
                 } else {
                     throw ToolError(where + ": unknown operation (set, fill, line, replace, ascii, clear, resize, add_layer)");
                 }
                 ++ops_run;
             }
             // Cells whose value differs from before, matched by position (so a resize alone changes nothing that was kept).
             usize changed = 0;
             for (usize l = 0; l < m.layers.size(); ++l) {
                 for (u32 y = 0; y < m.height; ++y) {
                     for (u32 x = 0; x < m.width; ++x) {
                         const i32 was = (l < before.size() && x < old_w && y < old_h) ? before[l][static_cast<usize>(y) * old_w + x] : sp::kEmpty;
                         if (was != m.layers[l].cells[static_cast<usize>(y) * m.width + x]) ++changed;
                     }
                 }
             }
             const std::optional<sp::Tileset> tileset = TilesetOf(p, m);
             Json warnings = MapProblems(m, tileset ? &*tileset : nullptr);
             SaveWithScan(p, asset_path, [&](const fs::path& f, std::string* e) { return sp::SaveTilemap(f, m, e); });
             return {{"path", asset_path}, {"ops_run", ops_run}, {"cells_changed", changed}, {"width", m.width}, {"height", m.height}, {"resized", m.width != old_w || m.height != old_h},
                     {"layers_added", m.layers.size() - old_layers}, {"warnings", warnings}};
         }});

    server.AddTool(
        {"tilemap_preview",
         "See a tilemap: a PNG returned as an image (and saved to `path` if given), drawn from the real tile pictures of the tileset's texture, layers in order, autotiles resolved "
         "to the tile each cell would show. With `show_solid`, the cells that block are tinted red. If the texture cannot be read, tiles are drawn as coloured blocks. `scale` "
         "enlarges (default: as large as fits in about 1024 pixels).",
         Schema({{"asset", {{"type", "string"}}}, {"scale", {{"type", "integer"}, {"description", "1-16"}}}, {"show_solid", {{"type", "boolean"}}}, {"layers", {{"type", "array"}, {"items", {{"type", "string"}}}, {"description", "Only these layers"}}},
                 {"path", {{"type", "string"}, {"description", "Also save the PNG here"}}}}, {"asset"}),
         [host](const Json& args) -> Json {
             OpenProject& p = host->Require();
             const AssetRecord& r = RequireOfType(p, args, "asset", "Tilemap");
             const sp::TilemapData m = LoadTilemapAsset(p, r);
             const std::optional<sp::Tileset> tileset = TilesetOf(p, m);
             const u32 tw = tileset ? tileset->tile_width : 16, th = tileset ? tileset->tile_height : 16;
             if (m.width == 0 || m.height == 0) throw ToolError("The map has no size");
             u32 scale = std::clamp<u32>(1024 / std::max(1u, m.width * tw), 1u, 8u);
             if (args.contains("scale")) {
                 if (!args["scale"].is_number_integer() || args["scale"].get<long long>() < 1 || args["scale"].get<long long>() > 16) throw ToolError("\"scale\" must be an integer from 1 to 16");
                 scale = args["scale"].get<u32>();
             }
             const unsigned long long width = static_cast<unsigned long long>(m.width) * tw * scale, height = static_cast<unsigned long long>(m.height) * th * scale;
             if (width * height > 16ull * 1024 * 1024) throw ToolError("The picture would be " + std::to_string(width) + " x " + std::to_string(height) + " pixels: use a smaller scale");
             std::set<std::string> only;
             if (args.contains("layers")) for (const std::string& n : OptStrings(args, "layers")) only.insert(n);

             assets::ImageData image;
             bool have_image = false;
             if (tileset) {
                 if (const AssetRecord* tex = p.database->Find(tileset->texture)) have_image = assets::DecodeImageFile(p.database->SourcePath(tex->guid).string(), image);
             }
             std::vector<u8> rgba(static_cast<usize>(width * height) * 4, 255);
             for (usize py = 0; py < height; ++py) {
                 for (usize px = 0; px < width; ++px) {
                     const bool checker = (((px / (4 * scale)) + (py / (4 * scale))) & 1) != 0;
                     const u8 v = checker ? 44 : 36;
                     const usize o = (py * width + px) * 4;
                     rgba[o] = rgba[o + 1] = v;
                     rgba[o + 2] = static_cast<u8>(v + 6);
                 }
             }
             auto block_color = [](i32 tile) {
                 const u32 h = static_cast<u32>(tile) * 2654435761u;
                 return std::array<u8, 3>{static_cast<u8>(80 + (h & 127)), static_cast<u8>(80 + ((h >> 8) & 127)), static_cast<u8>(80 + ((h >> 16) & 127))};
             };
             Json drawn = Json::array();
             for (const sp::TileLayer& layer : m.layers) {
                 if (!only.empty() && only.count(layer.name) == 0) continue;
                 const usize li = static_cast<usize>(&layer - m.layers.data());
                 drawn.push_back(layer.name);
                 for (i32 y = 0; y < static_cast<i32>(m.height); ++y) {
                     for (i32 x = 0; x < static_cast<i32>(m.width); ++x) {
                         const i32 tile = tileset ? sp::ResolveTile(m, *tileset, li, x, y) : (m.Get(li, x, y) >= 0 ? m.Get(li, x, y) : sp::kEmpty);
                         if (tile < 0) continue;
                         const usize ox = static_cast<usize>(x) * tw * scale, oy = static_cast<usize>(static_cast<i32>(m.height) - 1 - y) * th * scale;
                         u32 sx = 0, sy = 0;
                         bool sample = false;
                         if (have_image && tileset && tileset->Columns() > 0) {
                             sx = tileset->margin + (static_cast<u32>(tile) % tileset->Columns()) * (tw + tileset->spacing);
                             sy = tileset->margin + (static_cast<u32>(tile) / tileset->Columns()) * (th + tileset->spacing);
                             sample = sx + tw <= image.width && sy + th <= image.height;
                         }
                         for (u32 ty = 0; ty < th * scale; ++ty) {
                             for (u32 tx = 0; tx < tw * scale; ++tx) {
                                 std::array<u8, 4> c;
                                 if (sample) {
                                     const usize s = (static_cast<usize>(sy + ty / scale) * image.width + (sx + tx / scale)) * 4;
                                     c = {image.pixels[s], image.pixels[s + 1], image.pixels[s + 2], image.pixels[s + 3]};
                                 } else {
                                     const std::array<u8, 3> b = block_color(tile);
                                     const bool edge = tx < scale || ty < scale;
                                     c = {static_cast<u8>(edge ? b[0] / 2 : b[0]), static_cast<u8>(edge ? b[1] / 2 : b[1]), static_cast<u8>(edge ? b[2] / 2 : b[2]), 255};
                                 }
                                 const usize o = ((oy + ty) * width + (ox + tx)) * 4;
                                 const f32 a = static_cast<f32>(c[3]) / 255.0f;
                                 for (int k = 0; k < 3; ++k) rgba[o + static_cast<usize>(k)] = static_cast<u8>(c[static_cast<usize>(k)] * a + rgba[o + static_cast<usize>(k)] * (1.0f - a));
                             }
                         }
                     }
                 }
             }
             if (OptBool(args, "show_solid", false) && tileset) {
                 for (i32 y = 0; y < static_cast<i32>(m.height); ++y) {
                     for (i32 x = 0; x < static_cast<i32>(m.width); ++x) {
                         if (!sp::IsSolidAt(m, *tileset, x, y)) continue;
                         const usize ox = static_cast<usize>(x) * tw * scale, oy = static_cast<usize>(static_cast<i32>(m.height) - 1 - y) * th * scale;
                         for (u32 ty = 0; ty < th * scale; ++ty) {
                             for (u32 tx = 0; tx < tw * scale; ++tx) {
                                 const usize o = ((oy + ty) * width + (ox + tx)) * 4;
                                 rgba[o] = static_cast<u8>(rgba[o] * 0.6f + 255 * 0.4f);
                                 rgba[o + 1] = static_cast<u8>(rgba[o + 1] * 0.6f);
                                 rgba[o + 2] = static_cast<u8>(rgba[o + 2] * 0.6f);
                             }
                         }
                     }
                 }
             }
             const std::vector<u8> png = EncodePngRgba(rgba, static_cast<u32>(width), static_cast<u32>(height));
             if (png.empty()) throw ToolError("Could not encode the PNG");
             Json out = {{"width", width}, {"height", height}, {"scale", scale}, {"tile_size", {{"width", tw}, {"height", th}}}, {"layers_drawn", drawn}, {"textured", have_image}};
             if (args.contains("path") && args["path"].is_string()) {
                 const fs::path file = args["path"].get<std::string>();
                 std::error_code ec;
                 if (file.has_parent_path()) fs::create_directories(file.parent_path(), ec);
                 std::ofstream f(file, std::ios::binary | std::ios::trunc);
                 f.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
                 if (!f) throw ToolError("Could not write " + file.string());
                 out["path"] = args["path"];
             }
             out["mcp_content"] = Json::array({{{"type", "image"}, {"data", Base64Encode(png)}, {"mimeType", "image/png"}}});
             return out;
         }});
}

} // namespace aether::mcp
