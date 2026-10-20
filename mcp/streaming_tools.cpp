#include "streaming_tools.h"

#include "project_host.h"

#include "aether/scene/components.h"
#include "aether/streaming/partition.h"
#include "aether/streaming/streamer.h"

#include <algorithm>
#include <cmath>
#include <set>

namespace aether::mcp {

namespace {

using namespace detail;
namespace st = aether::streaming;

Json CoordJson(const st::CellCoord& c) { return Json::array({c.x, c.z}); }
Json Vec3Json(const Vec3& v) { return Json::array({v.x, v.y, v.z}); }

Json IndexJson(const st::WorldIndex& index) {
    Json cells = Json::array();
    u32 total = index.persistent_count;
    u32 biggest = 0;
    for (const st::CellInfo& c : index.cells) {
        cells.push_back({{"cell", CoordJson(c.coord)}, {"name", st::CellName(c.coord)}, {"entities", c.entity_count}, {"min", Vec3Json(c.min)}, {"max", Vec3Json(c.max)}});
        total += c.entity_count;
        biggest = std::max(biggest, c.entity_count);
    }
    return {{"cell_size", index.settings.cell_size},
            {"origin", Vec3Json(index.settings.origin)},
            {"cells", cells},
            {"cell_count", index.cells.size()},
            {"persistent_entities", index.persistent_count},
            {"total_entities", total},
            {"largest_cell_entities", biggest}};
}

fs::path RequireDir(const Json& args) {
    const fs::path dir(RequireString(args, "directory"));
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) throw ToolError("No such folder: " + dir.string());
    return dir;
}

Vec3 ReadPoint(const Json& p, const std::string& what) {
    if (!p.is_array() || p.size() != 3 || !p[0].is_number() || !p[1].is_number() || !p[2].is_number()) throw ToolError(what + " must be [x, y, z]");
    return Vec3(p[0].get<f32>(), p[1].get<f32>(), p[2].get<f32>());
}

} // namespace

void RegisterStreamingTools(McpServer& server, EditorSession& session) {
    server.AddTool(
        {"world_partition",
         "Split the editor scene into world-partition cells: square grid cells on the ground plane (x, z), each saved as its own scene. Root entities with a Transform go to the cell "
         "their position is in, with their descendants; entities with no Transform, an AlwaysLoaded, or a StreamingSource stay in the persistent scene. Without `directory` this is a "
         "dry run that reports the cells (entity counts, bounds), the persistent count and the largest cell, so a cell size can be chosen. With `directory` it writes "
         "`world.aworld` (the index), `persistent.aesc` and `cells/<x_z>.aesc` there.",
         Schema({{"cell_size", {{"type", "number"}, {"description", "Cell edge length in metres (default 64)"}}},
                 {"origin", {{"type", "array"}, {"items", {{"type", "number"}}}, {"description", "[x, y, z] where cell (0, 0) starts (default 0,0,0)"}}},
                 {"directory", {{"type", "string"}, {"description", "Write the partition here (made if missing); omit for a dry run"}}}}),
         [&session](const Json& args) -> Json {
             st::PartitionSettings settings;
             if (args.contains("cell_size")) {
                 if (!args["cell_size"].is_number() || !(args["cell_size"].get<f32>() >= 1.0f)) throw ToolError("cell_size must be at least 1");
                 settings.cell_size = args["cell_size"].get<f32>();
             }
             if (args.contains("origin")) settings.origin = ReadPoint(args["origin"], "origin");
             const st::PartitionResult result = st::PartitionWorld(session.world, settings);
             Json out = IndexJson(result.index);
             out["written"] = false;
             if (args.contains("directory")) {
                 const fs::path dir(RequireString(args, "directory"));
                 std::error_code status_error;
                 const fs::file_status cells_status = fs::symlink_status(dir / "cells", status_error);
                 if (!status_error && fs::is_symlink(cells_status)) {
                     throw ToolError("The partition cells folder cannot be a symlink because export removes stale cell files");
                 }
                 if (status_error != std::errc::no_such_file_or_directory && status_error) {
                     throw ToolError("Could not inspect partition cells folder: " + status_error.message());
                 }
                 std::error_code ec;
                 fs::create_directories(dir, ec);
                 std::string error;
                 if (!st::SavePartition(result, dir, &error)) throw ToolError("Couldn't write the partition: " + error);
                 out["written"] = true;
                 out["directory"] = dir.string();
             }
             return out;
         }});

    server.AddTool(
        {"world_partition_info",
         "Read a partitioned world from `directory` (its `world.aworld` index): the cell size and origin, every cell with its entity count and bounds, the persistent count, and whether "
         "each cell's file and the persistent scene exist on disk (with sizes). Problems are listed: a missing file, a file with no index entry.",
         Schema({{"directory", {{"type", "string"}}}}, {"directory"}),
         [](const Json& args) -> Json {
             const fs::path dir = RequireDir(args);
             st::WorldIndex index;
             std::string error;
             if (!st::LoadWorldIndexFile(dir, index, &error)) throw ToolError("Not a partitioned world: " + error);
             Json out = IndexJson(index);
             Json problems = Json::array();
             std::error_code ec;
             const fs::path persistent = dir / "persistent.aesc";
             if (fs::exists(persistent, ec)) out["persistent_bytes"] = fs::file_size(persistent, ec);
             else if (index.persistent_count > 0) problems.push_back("persistent.aesc is missing but the index has " + std::to_string(index.persistent_count) + " persistent entities");
             std::set<std::string> known;
             for (Json& c : out["cells"]) {
                 const fs::path file = dir / "cells" / (c["name"].get<std::string>() + ".aesc");
                 known.insert(file.filename().string());
                 if (fs::exists(file, ec)) {
                     c["bytes"] = fs::file_size(file, ec);
                 } else {
                     c["missing"] = true;
                     problems.push_back("cell " + c["name"].get<std::string>() + " has no file");
                 }
             }
             if (fs::is_directory(dir / "cells", ec)) {
                 for (const auto& entry : fs::directory_iterator(dir / "cells", ec)) {
                     if (entry.path().extension() == ".aesc" && !known.count(entry.path().filename().string())) {
                         problems.push_back(entry.path().filename().string() + " is not in the index");
                     }
                 }
             }
             out["problems"] = problems;
             out["ok"] = problems.empty();
             return out;
         }});

    server.AddTool(
        {"world_stream_simulate",
         "Walk a streaming source along `path` ([[x, y, z], ...], local positions) over a partitioned world in `directory` and see what streams. At each waypoint the streamer updates "
         "until its load queue is empty. For every waypoint: the cells that loaded and unloaded, the cells now loaded, and the world's entity count. `radius` is the source's "
         "(cells within it load; default 128); `unload_margin` (default 1.25) is how far past the radius a cell stays; `max_loads` / `max_unloads` cap per update. The persistent "
         "scene is not loaded. Problems the streamer reports (a cell file that won't load) come back too.",
         Schema({{"directory", {{"type", "string"}}},
                 {"path", {{"type", "array"}, {"items", {{"type", "array"}}}}},
                 {"radius", {{"type", "number"}}},
                 {"unload_margin", {{"type", "number"}}},
                 {"max_loads", {{"type", "integer"}}},
                 {"max_unloads", {{"type", "integer"}}}},
                {"directory", "path"}),
         [](const Json& args) -> Json {
             const fs::path dir = RequireDir(args);
             st::WorldIndex index;
             std::string error;
             if (!st::LoadWorldIndexFile(dir, index, &error)) throw ToolError("Not a partitioned world: " + error);
             if (!args["path"].is_array() || args["path"].empty()) throw ToolError("\"path\" must be a non-empty array of [x, y, z]");
             if (args["path"].size() > 200) throw ToolError("At most 200 waypoints");
             const f32 radius = args.contains("radius") && args["radius"].is_number() ? args["radius"].get<f32>() : 128.0f;
             if (!(radius > 0.0f)) throw ToolError("radius must be above 0");

             World world;
             const Entity source = world.CreateEntity(Transform{}, StreamingSource{radius, true});
             st::WorldStreamer streamer(world, index, st::WorldStreamer::FileLoader(dir));
             if (args.contains("unload_margin") && args["unload_margin"].is_number()) streamer.unload_margin = args["unload_margin"].get<f32>();
             if (args.contains("max_loads") && args["max_loads"].is_number_integer()) streamer.max_loads_per_update = static_cast<usize>(std::max(1, args["max_loads"].get<i32>()));
             if (args.contains("max_unloads") && args["max_unloads"].is_number_integer()) streamer.max_unloads_per_update = static_cast<usize>(std::max(1, args["max_unloads"].get<i32>()));

             Json steps = Json::array();
             usize peak = 0;
             u32 index_of = 0;
             for (const Json& p : args["path"]) {
                 const Vec3 at = ReadPoint(p, "each waypoint");
                 world.GetComponent<Transform>(source)->position = at;
                 Json loaded = Json::array(), unloaded = Json::array();
                 u32 updates = 0;
                 do {
                     streamer.Update();
                     ++updates;
                     for (const st::StreamingEvent& e : streamer.Events()) {
                         (e.loaded ? loaded : unloaded).push_back({{"cell", CoordJson(e.cell)}, {"entities", e.entities}});
                     }
                 } while (streamer.PendingLoads() > 0 && updates < 500);
                 Json now = Json::array();
                 for (const st::CellCoord& c : streamer.LoadedCells()) now.push_back(CoordJson(c));
                 const usize entities = world.EntityCount();
                 peak = std::max(peak, entities);
                 steps.push_back({{"waypoint", index_of++},
                                  {"at", Vec3Json(at)},
                                  {"cell", CoordJson(st::CellOf(at, index.settings))},
                                  {"loaded", loaded},
                                  {"unloaded", unloaded},
                                  {"loaded_cells", now},
                                  {"entities", entities},
                                  {"updates", updates}});
             }
             return {{"steps", steps}, {"peak_entities", peak}, {"problems", streamer.Problems()}};
         }});
}

} // namespace aether::mcp
