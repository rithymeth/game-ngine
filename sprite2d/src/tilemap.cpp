#include "aether/sprite2d/tilemap.h"

#include "aether/platform/filesystem.h"

#include <algorithm>
#include <cmath>
#include <fstream>

namespace aether::sprite2d {

u32 Tileset::Columns() const {
    if (tile_width == 0 || texture_width < 2 * margin) return 0;
    return (texture_width - 2 * margin + spacing) / (tile_width + spacing);
}
u32 Tileset::Rows() const {
    if (tile_height == 0 || texture_height < 2 * margin) return 0;
    return (texture_height - 2 * margin + spacing) / (tile_height + spacing);
}

bool Tileset::Uv(i32 tile, f32 out[4]) const {
    const u32 columns = Columns();
    if (tile < 0 || columns == 0 || static_cast<u32>(tile) >= TileCount()) return false;
    const u32 col = static_cast<u32>(tile) % columns;
    const u32 row = static_cast<u32>(tile) / columns;
    const f32 x = static_cast<f32>(margin + col * (tile_width + spacing));
    const f32 y = static_cast<f32>(margin + row * (tile_height + spacing));
    out[0] = x / static_cast<f32>(texture_width);
    out[1] = y / static_cast<f32>(texture_height);
    out[2] = (x + static_cast<f32>(tile_width)) / static_cast<f32>(texture_width);
    out[3] = (y + static_cast<f32>(tile_height)) / static_cast<f32>(texture_height);
    return true;
}

void Tileset::SetSolid(i32 tile, bool value) {
    if (tile < 0) return;
    if (static_cast<usize>(tile) >= solid.size()) solid.resize(static_cast<usize>(tile) + 1, 0);
    solid[static_cast<usize>(tile)] = value ? 1 : 0;
}

usize Tileset::AddAutotile(const std::string& name, const std::array<i32, 16>& tiles) {
    Autotile a;
    a.name = name;
    a.tiles = tiles;
    autotiles.push_back(std::move(a));
    return autotiles.size() - 1;
}

namespace {

bool ReadJsonFile(const std::filesystem::path& file, nlohmann::json& out, std::string* error) {
    std::string text;
    if (!fs::ReadFileText(file.string(), text)) {
        if (error) *error = "Couldn't read " + file.string();
        return false;
    }
    out = nlohmann::json::parse(text, nullptr, false);
    if (out.is_discarded()) {
        if (error) *error = file.string() + " isn't valid JSON";
        return false;
    }
    return true;
}

bool WriteJsonFile(const std::filesystem::path& file, const nlohmann::json& data, std::string* error) {
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    std::ofstream out(file, std::ios::binary);
    if (!out) {
        if (error) *error = "Couldn't write " + file.string();
        return false;
    }
    out << data.dump(2);
    return true;
}

} // namespace

nlohmann::json TilesetToJson(const Tileset& t) {
    nlohmann::json autotiles = nlohmann::json::array();
    for (const Autotile& a : t.autotiles) autotiles.push_back({{"name", a.name}, {"tiles", a.tiles}});
    nlohmann::json solid = nlohmann::json::array();
    for (usize i = 0; i < t.solid.size(); ++i) {
        if (t.solid[i]) solid.push_back(i);
    }
    return {{"$type", "Tileset"},
            {"$version", 1},
            {"texture", assets::ToString(t.texture)},
            {"texture_size", {t.texture_width, t.texture_height}},
            {"tile_size", {t.tile_width, t.tile_height}},
            {"margin", t.margin},
            {"spacing", t.spacing},
            {"solid", solid},
            {"autotiles", autotiles}};
}

bool TilesetFromJson(const nlohmann::json& data, Tileset& out, std::string* error) {
    const auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    if (!data.is_object() || data.value("$type", "") != "Tileset") return fail("not a tileset");
    Tileset t;
    const std::string texture = data.value("texture", "");
    if (!texture.empty() && !assets::ParseAssetGuid(texture, t.texture)) return fail("bad texture GUID");
    if (data.contains("texture_size")) {
        t.texture_width = data["texture_size"][0].get<u32>();
        t.texture_height = data["texture_size"][1].get<u32>();
    }
    if (data.contains("tile_size")) {
        t.tile_width = data["tile_size"][0].get<u32>();
        t.tile_height = data["tile_size"][1].get<u32>();
    }
    if (t.tile_width == 0 || t.tile_height == 0) return fail("the tile size is zero");
    t.margin = data.value("margin", 0u);
    t.spacing = data.value("spacing", 0u);
    for (const nlohmann::json& s : data.value("solid", nlohmann::json::array())) t.SetSolid(s.get<i32>(), true);
    for (const nlohmann::json& a : data.value("autotiles", nlohmann::json::array())) {
        Autotile at;
        at.name = a.value("name", "");
        if (!a.contains("tiles") || !a["tiles"].is_array() || a["tiles"].size() != 16) {
            return fail("autotile '" + at.name + "' needs 16 tiles");
        }
        for (usize i = 0; i < 16; ++i) at.tiles[i] = a["tiles"][i].get<i32>();
        t.autotiles.push_back(std::move(at));
    }
    out = std::move(t);
    return true;
}

bool LoadTileset(const std::filesystem::path& file, Tileset& out, std::string* error) {
    nlohmann::json data;
    return ReadJsonFile(file, data, error) && TilesetFromJson(data, out, error);
}
bool SaveTileset(const std::filesystem::path& file, const Tileset& t, std::string* error) {
    return WriteJsonFile(file, TilesetToJson(t), error);
}

void TilemapData::Resize(u32 new_width, u32 new_height) {
    for (TileLayer& layer : layers) {
        std::vector<i32> cells(static_cast<usize>(new_width) * new_height, kEmpty);
        for (u32 y = 0; y < std::min(height, new_height); ++y) {
            for (u32 x = 0; x < std::min(width, new_width); ++x) {
                cells[static_cast<usize>(y) * new_width + x] = layer.cells[static_cast<usize>(y) * width + x];
            }
        }
        layer.cells = std::move(cells);
    }
    width = new_width;
    height = new_height;
}

usize TilemapData::AddLayer(const std::string& name) {
    TileLayer layer;
    layer.name = name;
    layer.cells.assign(static_cast<usize>(width) * height, kEmpty);
    layers.push_back(std::move(layer));
    return layers.size() - 1;
}

i32 TilemapData::Get(usize layer, i32 x, i32 y) const {
    if (layer >= layers.size() || !InBounds(x, y)) return kEmpty;
    return layers[layer].cells[static_cast<usize>(y) * width + static_cast<usize>(x)];
}

bool TilemapData::Set(usize layer, i32 x, i32 y, i32 cell) {
    if (layer >= layers.size() || !InBounds(x, y)) return false;
    layers[layer].cells[static_cast<usize>(y) * width + static_cast<usize>(x)] = cell;
    return true;
}

usize TilemapData::Fill(usize layer, i32 x0, i32 y0, i32 x1, i32 y1, i32 cell) {
    usize n = 0;
    for (i32 y = std::min(y0, y1); y <= std::max(y0, y1); ++y) {
        for (i32 x = std::min(x0, x1); x <= std::max(x0, x1); ++x) {
            if (Set(layer, x, y, cell)) ++n;
        }
    }
    return n;
}

nlohmann::json TilemapToJson(const TilemapData& m) {
    nlohmann::json layers = nlohmann::json::array();
    for (const TileLayer& l : m.layers) {
        layers.push_back({{"name", l.name}, {"collides", l.collides}, {"cells", l.cells}});
    }
    return {{"$type", "Tilemap"}, {"$version", 1}, {"tileset", assets::ToString(m.tileset)},
            {"size", {m.width, m.height}}, {"layers", layers}};
}

bool TilemapFromJson(const nlohmann::json& data, TilemapData& out, std::string* error) {
    const auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    if (!data.is_object() || data.value("$type", "") != "Tilemap") return fail("not a tilemap");
    TilemapData m;
    const std::string tileset = data.value("tileset", "");
    if (!tileset.empty() && !assets::ParseAssetGuid(tileset, m.tileset)) return fail("bad tileset GUID");
    if (!data.contains("size") || !data["size"].is_array() || data["size"].size() != 2) return fail("no size");
    m.width = data["size"][0].get<u32>();
    m.height = data["size"][1].get<u32>();
    if (static_cast<u64>(m.width) * m.height > 16u * 1024u * 1024u) return fail("the map is too large");
    for (const nlohmann::json& l : data.value("layers", nlohmann::json::array())) {
        TileLayer layer;
        layer.name = l.value("name", "");
        layer.collides = l.value("collides", true);
        layer.cells = l.value("cells", std::vector<i32>{});
        if (layer.cells.size() != static_cast<usize>(m.width) * m.height) {
            return fail("layer '" + layer.name + "' has " + std::to_string(layer.cells.size()) + " cells, not " +
                        std::to_string(static_cast<usize>(m.width) * m.height));
        }
        m.layers.push_back(std::move(layer));
    }
    out = std::move(m);
    return true;
}

bool LoadTilemap(const std::filesystem::path& file, TilemapData& out, std::string* error) {
    nlohmann::json data;
    return ReadJsonFile(file, data, error) && TilemapFromJson(data, out, error);
}
bool SaveTilemap(const std::filesystem::path& file, const TilemapData& m, std::string* error) {
    return WriteJsonFile(file, TilemapToJson(m), error);
}

u8 NeighbourMask(const TilemapData& map, usize layer, i32 x, i32 y) {
    const i32 self = map.Get(layer, x, y);
    if (self == kEmpty) return 0;
    u8 mask = 0;
    if (map.Get(layer, x, y + 1) == self) mask |= kNorth;
    if (map.Get(layer, x + 1, y) == self) mask |= kEast;
    if (map.Get(layer, x, y - 1) == self) mask |= kSouth;
    if (map.Get(layer, x - 1, y) == self) mask |= kWest;
    return mask;
}

i32 ResolveTile(const TilemapData& map, const Tileset& tileset, usize layer, i32 x, i32 y) {
    const i32 cell = map.Get(layer, x, y);
    if (!IsAutotileCell(cell)) return cell;
    const usize index = AutotileIndex(cell);
    if (index >= tileset.autotiles.size()) return kEmpty;
    return tileset.autotiles[index].tiles[NeighbourMask(map, layer, x, y)];
}

bool IsSolidAt(const TilemapData& map, const Tileset& tileset, i32 x, i32 y) {
    for (usize l = 0; l < map.layers.size(); ++l) {
        if (map.layers[l].collides && tileset.IsSolid(ResolveTile(map, tileset, l, x, y))) return true;
    }
    return false;
}

void LocalToCell(f32 local_x, f32 local_y, f32 tile_units, i32& out_x, i32& out_y) {
    if (tile_units <= 0.0f) {
        out_x = out_y = 0;
        return;
    }
    out_x = static_cast<i32>(std::floor(local_x / tile_units));
    out_y = static_cast<i32>(std::floor(local_y / tile_units));
}

} // namespace aether::sprite2d
