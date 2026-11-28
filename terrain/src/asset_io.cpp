#include "aether/terrain/asset_io.h"

#include <cmath>
#include <limits>
#include <vector>

namespace aether {
namespace terrain {

namespace {

using json = nlohmann::json;

constexpr int kTerrainVersion = 1;
constexpr int kFoliageVersion = 1;

bool Fail(std::string* error, const std::string& why) {
    if (error != nullptr) *error = why;
    return false;
}

bool ReadFloats(const json& array, std::vector<f32>& out) {
    if (!array.is_array()) return false;
    out.clear();
    out.reserve(array.size());
    for (const json& value : array) {
        if (!value.is_number()) return false;
        const f64 number = value.get<f64>();
        if (!std::isfinite(number)) return false;
        out.push_back(static_cast<f32>(number));
    }
    return true;
}

bool ReadBytes(const json& array, std::vector<u8>& out) {
    if (!array.is_array()) return false;
    out.clear();
    out.reserve(array.size());
    for (const json& value : array) {
        if (!value.is_number_integer()) return false;
        const i64 number = value.get<i64>();
        if (number < 0 || number > 255) return false;
        out.push_back(static_cast<u8>(number));
    }
    return true;
}

bool ReadVec3(const json& array, Vec3& out) {
    std::vector<f32> numbers;
    if (!ReadFloats(array, numbers) || numbers.size() != 3) return false;
    out = Vec3(numbers[0], numbers[1], numbers[2]);
    return true;
}

bool ReadQuaternion(const json& array, Quaternion& out) {
    std::vector<f32> numbers;
    if (!ReadFloats(array, numbers) || numbers.size() != 4) return false;
    out = Quaternion(numbers[0], numbers[1], numbers[2], numbers[3]);
    return true;
}

json WriteVec3(const Vec3& v) { return json::array({v.x, v.y, v.z}); }

json WriteQuaternion(const Quaternion& q) { return json::array({q.x, q.y, q.z, q.w}); }

// Numbers for a JSON array: u8 pixels and u8 density cells are written as numbers, not strings, so a file stays plain text.
template <typename T>
json WriteNumbers(const std::vector<T>& values) {
    json array = json::array();
    for (const T value : values) array.push_back(value);
    return array;
}

bool Header(const json& j, const char* format, int version, std::string* error) {
    if (!j.is_object()) return Fail(error, std::string("not a ") + format + " file: it is not a JSON object");
    if (j.value("format", std::string()) != format) {
        return Fail(error, std::string("not a ") + format + " file: its format is not \"" + format + "\"");
    }
    const int file_version = j.value("version", 0);
    if (file_version != version) {
        return Fail(error, std::string(format) + " version " + std::to_string(file_version) + " is not supported; this build reads " +
                              std::to_string(version));
    }
    return true;
}

} // namespace

json TerrainToJson(const TerrainData& data, const TerrainSettings& settings, const SplatmapData& splatmap) {
    json layers = json::array();
    for (const TerrainLayer& layer : data.layers) {
        layers.push_back({
            {"name", layer.name},
            {"albedo", WriteVec3(layer.albedo)},
            {"metallic", layer.metallic},
            {"roughness", layer.roughness},
            {"normal_strength", layer.normal_strength},
        });
    }
    return {
        {"format", "aether.terrain"},
        {"version", kTerrainVersion},
        {"settings",
         {
             {"vertical_scale", settings.vertical_scale},
             {"chunk_world_size", settings.chunk_world_size},
             {"verts_per_chunk", settings.verts_per_chunk},
             {"max_lod", settings.max_lod},
             {"lod_distance", settings.lod_distance},
         }},
        {"data",
         {
             {"chunk_size", data.chunk_size},
             {"max_lod", data.max_lod},
             {"lod_distances", WriteNumbers(data.lod_distances)},
             {"heightmap",
              {
                  {"width", data.heightmap.width},
                  {"height", data.heightmap.height},
                  {"cell_size", data.heightmap.cell_size},
                  {"heights", WriteNumbers(data.heightmap.heights)},
              }},
             {"layers", layers},
         }},
        {"splatmap",
         {
             {"width", splatmap.width},
             {"height", splatmap.height},
             {"pixels", WriteNumbers(splatmap.pixels)},
         }},
    };
}

bool TerrainFromJson(const json& j, TerrainData& data, TerrainSettings& settings, SplatmapData& splatmap, std::string* error) try {
    if (!Header(j, "aether.terrain", kTerrainVersion, error)) return false;
    const auto settings_json = j.find("settings");
    const auto data_json = j.find("data");
    if (settings_json == j.end() || !settings_json->is_object()) return Fail(error, "the terrain has no settings");
    if (data_json == j.end() || !data_json->is_object()) return Fail(error, "the terrain has no data");

    TerrainSettings parsed_settings;
    parsed_settings.vertical_scale = settings_json->value("vertical_scale", parsed_settings.vertical_scale);
    parsed_settings.chunk_world_size = settings_json->value("chunk_world_size", parsed_settings.chunk_world_size);
    parsed_settings.verts_per_chunk = settings_json->value("verts_per_chunk", parsed_settings.verts_per_chunk);
    parsed_settings.max_lod = settings_json->value("max_lod", parsed_settings.max_lod);
    parsed_settings.lod_distance = settings_json->value("lod_distance", parsed_settings.lod_distance);

    const auto heightmap_json = data_json->find("heightmap");
    if (heightmap_json == data_json->end() || !heightmap_json->is_object()) return Fail(error, "the terrain has no heightmap");
    const u32 width = heightmap_json->value("width", 0u);
    const u32 height = heightmap_json->value("height", 0u);
    if (width < 2 || height < 2) return Fail(error, "the heightmap is smaller than two samples a side");
    if (width > 8193 || height > 8193) return Fail(error, "the heightmap is larger than 8193 samples a side");
    std::vector<f32> heights;
    const auto heights_json = heightmap_json->find("heights");
    if (heights_json == heightmap_json->end() || !ReadFloats(*heights_json, heights)) return Fail(error, "the heightmap's heights are not numbers");
    if (heights.size() != static_cast<usize>(width) * height) {
        return Fail(error, "the heightmap has " + std::to_string(heights.size()) + " heights for " + std::to_string(width) + " x " +
                               std::to_string(height) + " samples");
    }

    std::vector<TerrainLayer> layers;
    const auto layers_json = data_json->find("layers");
    if (layers_json != data_json->end() && layers_json->is_array()) {
        for (const json& layer_json : *layers_json) {
            if (!layer_json.is_object()) return Fail(error, "a terrain layer is not an object");
            TerrainLayer layer;
            layer.name = layer_json.value("name", std::string());
            const auto albedo_json = layer_json.find("albedo");
            if (albedo_json != layer_json.end() && !ReadVec3(*albedo_json, layer.albedo)) return Fail(error, "a layer's albedo is not three numbers");
            layer.metallic = layer_json.value("metallic", layer.metallic);
            layer.roughness = layer_json.value("roughness", layer.roughness);
            layer.normal_strength = layer_json.value("normal_strength", layer.normal_strength);
            layers.push_back(std::move(layer));
        }
    }

    SplatmapData parsed_splat;
    const auto splat_json = j.find("splatmap");
    if (splat_json != j.end() && splat_json->is_object()) {
        parsed_splat.width = splat_json->value("width", 0u);
        parsed_splat.height = splat_json->value("height", 0u);
        const auto pixels_json = splat_json->find("pixels");
        if (parsed_splat.width != 0 || parsed_splat.height != 0) {
            if (pixels_json == splat_json->end() || !ReadBytes(*pixels_json, parsed_splat.pixels)) {
                return Fail(error, "the splatmap's pixels are not bytes");
            }
            if (parsed_splat.pixels.size() != static_cast<usize>(parsed_splat.width) * parsed_splat.height * 4) {
                return Fail(error, "the splatmap has " + std::to_string(parsed_splat.pixels.size()) + " bytes for " +
                                       std::to_string(parsed_splat.width) + " x " + std::to_string(parsed_splat.height) + " RGBA pixels");
            }
        }
    }

    std::vector<f32> lod_distances;
    const auto lod_json = data_json->find("lod_distances");
    if (lod_json != data_json->end() && !ReadFloats(*lod_json, lod_distances)) return Fail(error, "the LOD distances are not numbers");

    TerrainData parsed;
    parsed.chunk_size = data_json->value("chunk_size", 0u);
    parsed.max_lod = data_json->value("max_lod", parsed.max_lod);
    if (!lod_distances.empty()) parsed.lod_distances = std::move(lod_distances);
    parsed.layers = std::move(layers);
    Heightmap heightmap;
    heightmap.width = width;
    heightmap.height = height;
    heightmap.cell_size = heightmap_json->value("cell_size", 1.0f);
    if (!(heightmap.cell_size > 0.0f)) return Fail(error, "the heightmap's cell size is not above zero");
    heightmap.heights = std::move(heights);
    // The chunk layout and bounds are derived, not trusted from the file: an edit could have left them stale.
    InitTerrainData(parsed, heightmap, parsed_settings);

    data = std::move(parsed);
    settings = parsed_settings;
    splatmap = std::move(parsed_splat);
    return true;
} catch (const json::exception& e) {
    return Fail(error, std::string("terrain asset: ") + e.what());
}

json FoliageToJson(const FoliageLayer& layer, const DensityMap& density) {
    json types = json::array();
    for (const FoliageType& type : layer.types) {
        types.push_back({
            {"mesh_id", type.mesh_id},
            {"mesh_lod_count", type.mesh_lod_count},
            {"lod_distance", type.lod_distance},
            {"imposter_distance", type.imposter_distance},
            {"impostor_texture_id", type.impostor_texture_id},
            {"scale_min", type.scale_min},
            {"scale_max", type.scale_max},
            {"rotation_range", type.rotation_range},
            {"align_to_normal", type.align_to_normal},
            {"anchor_offset", type.anchor_offset},
        });
    }
    json instances = json::array();
    for (const FoliageInstance& instance : layer.instances) {
        instances.push_back({
            {"position", WriteVec3(instance.position)},
            {"normal", WriteVec3(instance.normal)},
            {"scale", WriteVec3(instance.scale)},
            {"rotation", WriteQuaternion(instance.rotation)},
            {"type_index", instance.type_index},
            {"chunk_x", instance.chunk_x},
            {"chunk_z", instance.chunk_z},
            {"lod_level", instance.lod_level},
            {"hidden", instance.hidden},
        });
    }
    return {
        {"format", "aether.foliage"},
        {"version", kFoliageVersion},
        {"layer",
         {
             {"name", layer.name},
             {"max_instances", layer.max_instances},
             {"density", layer.density},
             {"seed", layer.seed},
             {"types", types},
             {"instances", instances},
         }},
        {"density_map",
         {
             {"width", density.width},
             {"height", density.height},
             {"cells", WriteNumbers(density.cells)},
         }},
    };
}

bool FoliageFromJson(const json& j, FoliageLayer& layer, DensityMap& density, std::string* error) try {
    if (!Header(j, "aether.foliage", kFoliageVersion, error)) return false;
    const auto layer_json = j.find("layer");
    if (layer_json == j.end() || !layer_json->is_object()) return Fail(error, "the foliage asset has no layer");

    FoliageLayer parsed;
    parsed.name = layer_json->value("name", std::string());
    parsed.max_instances = layer_json->value("max_instances", parsed.max_instances);
    parsed.density = layer_json->value("density", parsed.density);
    parsed.seed = layer_json->value("seed", parsed.seed);

    const auto types_json = layer_json->find("types");
    if (types_json != layer_json->end() && types_json->is_array()) {
        for (const json& type_json : *types_json) {
            if (!type_json.is_object()) return Fail(error, "a foliage type is not an object");
            FoliageType type;
            type.mesh_id = type_json.value("mesh_id", type.mesh_id);
            type.mesh_lod_count = type_json.value("mesh_lod_count", type.mesh_lod_count);
            type.lod_distance = type_json.value("lod_distance", type.lod_distance);
            type.imposter_distance = type_json.value("imposter_distance", type.imposter_distance);
            type.impostor_texture_id = type_json.value("impostor_texture_id", type.impostor_texture_id);
            type.scale_min = type_json.value("scale_min", type.scale_min);
            type.scale_max = type_json.value("scale_max", type.scale_max);
            type.rotation_range = type_json.value("rotation_range", type.rotation_range);
            type.align_to_normal = type_json.value("align_to_normal", type.align_to_normal);
            type.anchor_offset = type_json.value("anchor_offset", type.anchor_offset);
            parsed.types.push_back(type);
        }
    }

    const auto instances_json = layer_json->find("instances");
    if (instances_json != layer_json->end() && instances_json->is_array()) {
        for (const json& instance_json : *instances_json) {
            if (!instance_json.is_object()) return Fail(error, "a foliage instance is not an object");
            FoliageInstance instance;
            const auto position = instance_json.find("position");
            const auto normal = instance_json.find("normal");
            const auto scale = instance_json.find("scale");
            const auto rotation = instance_json.find("rotation");
            if (position == instance_json.end() || !ReadVec3(*position, instance.position)) return Fail(error, "an instance's position is not three numbers");
            if (normal == instance_json.end() || !ReadVec3(*normal, instance.normal)) return Fail(error, "an instance's normal is not three numbers");
            if (scale == instance_json.end() || !ReadVec3(*scale, instance.scale)) return Fail(error, "an instance's scale is not three numbers");
            if (rotation == instance_json.end() || !ReadQuaternion(*rotation, instance.rotation)) return Fail(error, "an instance's rotation is not four numbers");
            instance.type_index = instance_json.value("type_index", 0u);
            instance.chunk_x = static_cast<u16>(instance_json.value("chunk_x", 0u));
            instance.chunk_z = static_cast<u16>(instance_json.value("chunk_z", 0u));
            instance.lod_level = instance_json.value("lod_level", 0u);
            instance.hidden = instance_json.value("hidden", false);
            if (parsed.types.empty() || instance.type_index >= parsed.types.size()) {
                return Fail(error, "an instance names type " + std::to_string(instance.type_index) + " of " + std::to_string(parsed.types.size()));
            }
            parsed.instances.push_back(instance);
        }
    }

    DensityMap parsed_density;
    const auto density_json = j.find("density_map");
    if (density_json != j.end() && density_json->is_object()) {
        parsed_density.width = density_json->value("width", 0u);
        parsed_density.height = density_json->value("height", 0u);
        const auto cells_json = density_json->find("cells");
        if (parsed_density.width != 0 || parsed_density.height != 0) {
            if (cells_json == density_json->end() || !ReadBytes(*cells_json, parsed_density.cells)) {
                return Fail(error, "the density map's cells are not bytes");
            }
            if (parsed_density.cells.size() != static_cast<usize>(parsed_density.width) * parsed_density.height) {
                return Fail(error, "the density map has " + std::to_string(parsed_density.cells.size()) + " cells for " +
                                       std::to_string(parsed_density.width) + " x " + std::to_string(parsed_density.height));
            }
        }
    }

    layer = std::move(parsed);
    density = std::move(parsed_density);
    return true;
} catch (const json::exception& e) {
    return Fail(error, std::string("foliage asset: ") + e.what());
}

} // namespace terrain
} // namespace aether
