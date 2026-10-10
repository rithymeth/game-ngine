#include "asset_tools.h"

#include "aether/assets/asset_database.h"
#include "aether/assets/content_browser.h"
#include "aether/assets/derived_data_cache.h"
#include "aether/assets/importer.h"
#include "aether/cook/cooker.h"
#include "aether/project/project.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>

namespace aether::mcp {

namespace {

namespace fs = std::filesystem;
using assets::AssetDatabase;
using assets::AssetGuid;
using assets::AssetRecord;

constexpr std::size_t kMaxTextBytes = 2 * 1024 * 1024;

struct OpenProject {
    ProjectSettings settings;
    ProjectPaths paths;
    std::unique_ptr<AssetDatabase> database;
    std::unique_ptr<assets::DerivedDataCache> cache;
    assets::ImporterRegistry importers = assets::ImporterRegistry::WithBuiltins();
};

struct AssetHost {
    std::unique_ptr<OpenProject> project;

    OpenProject& Require() const {
        if (!project) throw ToolError("No project open: call project_open first");
        return *project;
    }
};

Json Schema(Json properties, std::vector<std::string> required = {}) {
    Json schema = {{"type", "object"}, {"properties", std::move(properties)}};
    if (!required.empty()) schema["required"] = required;
    return schema;
}

std::string RequireString(const Json& args, const char* key) {
    if (!args.contains(key) || !args[key].is_string()) {
        throw ToolError(std::string("Missing string argument \"") + key + "\"");
    }
    return args[key].get<std::string>();
}

std::string OptString(const Json& args, const char* key, const std::string& fallback = "") {
    if (!args.contains(key)) return fallback;
    if (!args[key].is_string()) throw ToolError(std::string("\"") + key + "\" must be a string");
    return args[key].get<std::string>();
}

bool OptBool(const Json& args, const char* key, bool fallback) {
    if (!args.contains(key)) return fallback;
    if (!args[key].is_boolean()) throw ToolError(std::string("\"") + key + "\" must be a boolean");
    return args[key].get<bool>();
}

std::vector<std::string> OptStrings(const Json& args, const char* key) {
    std::vector<std::string> out;
    if (!args.contains(key)) return out;
    if (!args[key].is_array()) throw ToolError(std::string("\"") + key + "\" must be an array of strings");
    for (const Json& v : args[key]) {
        if (!v.is_string()) throw ToolError(std::string("\"") + key + "\" must be an array of strings");
        out.push_back(v.get<std::string>());
    }
    return out;
}

// A content-relative path ('/'-separated) that cannot leave the content root.
std::string CleanRelative(const std::string& path) {
    if (path.empty()) return {};
    const fs::path p = fs::path(path).lexically_normal();
    if (p.is_absolute() || p.has_root_name() || p.has_root_directory()) throw ToolError("Path must be relative to Content/: " + path);
    std::string s = p.generic_string();
    while (!s.empty() && s.back() == '/') s.pop_back();
    if (s == "." ) return {};
    if (s == ".." || s.compare(0, 3, "../") == 0) throw ToolError("Path must stay inside Content/: " + path);
    return s;
}

const AssetRecord& FindAsset(OpenProject& p, const std::string& key) {
    AssetGuid guid;
    if (assets::ParseAssetGuid(key, guid)) {
        if (const AssetRecord* r = p.database->Find(guid)) return *r;
    }
    if (const AssetRecord* r = p.database->FindByPath(CleanRelative(key))) return *r;
    throw ToolError("No asset \"" + key + "\" (a content path like Textures/a.png, or a GUID)");
}

std::string PathOf(OpenProject& p, const AssetGuid& guid) {
    const AssetRecord* r = p.database->Find(guid);
    return r ? r->path : assets::ToString(guid);
}

Json RecordJson(OpenProject& p, const AssetRecord& r, bool detail = false) {
    Json j = {{"guid", assets::ToString(r.guid)}, {"path", r.path}, {"importer", r.importer}, {"needs_import", r.needs_import}, {"missing", r.missing}};
    if (r.IsSubAsset()) j["parent"] = PathOf(p, r.parent);
    if (detail) {
        Json deps = Json::array(), subs = Json::array(), refs = Json::array();
        for (const AssetGuid& d : r.dependencies) deps.push_back(PathOf(p, d));
        for (const AssetGuid& s : r.sub_assets) subs.push_back(PathOf(p, s));
        for (const AssetRecord* ref : p.database->Referencers(r.guid)) refs.push_back(ref->path);
        j["dependencies"] = deps;
        j["referenced_by"] = refs;
        if (!subs.empty()) j["sub_assets"] = subs;
        assets::AssetMeta meta;
        if (!r.IsSubAsset() && assets::LoadAssetMeta(p.database->MetaPath(r.guid), meta)) {
            j["settings"] = meta.settings;
            j["labels"] = meta.labels;
            j["importer_version"] = meta.importer_version;
        }
    }
    return j;
}

Json ScanJson(const assets::ScanResult& s) {
    return {{"assets", s.assets}, {"created_meta", s.created_meta}, {"missing", s.missing},
            {"duplicates_fixed", s.duplicates_fixed}, {"needs_import", s.needs_import}, {"warnings", s.warnings}};
}

Json ProjectJson(OpenProject& p) {
    usize sources = 0;
    for (const AssetRecord* r : p.database->All()) {
        if (!r->IsSubAsset()) ++sources;
    }
    return {{"name", p.settings.name},
            {"project_file", p.paths.file.string()},
            {"content", p.paths.content.string()},
            {"engine_version", p.settings.engine_version},
            {"startup_scene", p.settings.startup_scene},
            {"always_cook", p.settings.always_cook},
            {"plugins", p.settings.plugins},
            {"assets", sources}};
}

// A file inside Content/ for reading or writing text.
fs::path ContentFile(OpenProject& p, const std::string& relative) {
    const std::string rel = CleanRelative(relative);
    if (rel.empty()) throw ToolError("\"path\" must name a file under Content/");
    return p.paths.content / rel;
}

std::string KindOfCookFailure(const cook::CookReport& r) { return r.error.empty() ? "The cook failed" : r.error; }

} // namespace

void RegisterAssetTools(McpServer& server) {
    auto host = std::make_shared<AssetHost>();

    server.AddTool(
        {"project_open",
         "Open a project (.aproject) for the asset and cook tools: reads its settings and scans Content/ (new source files get an "
         ".ameta with a GUID). Replaces any open project.",
         Schema({{"project_file", {{"type", "string"}, {"description", "Path to the .aproject file"}}}}, {"project_file"}),
         [host](const Json& args) -> Json {
             const fs::path file = RequireString(args, "project_file");
             std::error_code ec;
             if (!fs::is_regular_file(file, ec)) throw ToolError("No such project file: " + file.string());
             auto project = std::make_unique<OpenProject>();
             std::string error;
             std::vector<std::string> warnings;
             if (!LoadProject(file, project->settings, &error, &warnings)) throw ToolError(error);
             project->paths = ProjectPaths::ForFile(fs::absolute(file, ec));
             if (!EnsureProjectFolders(project->paths, &error)) throw ToolError(error);
             project->database = std::make_unique<AssetDatabase>(project->paths.content);
             const assets::ScanResult scan = project->database->Scan();
             project->cache = std::make_unique<assets::DerivedDataCache>(project->paths.intermediate / "DerivedDataCache");
             host->project = std::move(project);
             Json out = ProjectJson(*host->project);
             out["scan"] = ScanJson(scan);
             out["warnings"] = warnings;
             return out;
         }});

    server.AddTool({"project_info", "Settings and asset count of the open project.", Schema(Json::object()),
                    [host](const Json&) -> Json {
                        OpenProject& p = host->Require();
                        Json out = ProjectJson(p);
                        out["layers"] = p.settings.layers;
                        out["fixed_timestep_hz"] = p.settings.fixed_timestep_hz;
                        out["window"] = {{"title", p.settings.window_title}, {"width", p.settings.window_width}, {"height", p.settings.window_height}};
                        return out;
                    }});

    server.AddTool(
        {"project_set",
         "Change project settings and save the .aproject: startup_scene, always_cook (asset paths or folders ending in '/'), plugins.",
         Schema({{"startup_scene", {{"type", "string"}}},
                 {"always_cook", {{"type", "array"}, {"items", {{"type", "string"}}}}},
                 {"plugins", {{"type", "array"}, {"items", {{"type", "string"}}}}}}),
         [host](const Json& args) -> Json {
             OpenProject& p = host->Require();
             ProjectSettings next = p.settings;
             if (args.contains("startup_scene")) next.startup_scene = OptString(args, "startup_scene");
             if (args.contains("always_cook")) next.always_cook = OptStrings(args, "always_cook");
             if (args.contains("plugins")) next.plugins = OptStrings(args, "plugins");
             std::string error;
             if (!SaveProject(p.paths.file, next, &error)) throw ToolError(error);
             p.settings = std::move(next);
             return ProjectJson(p);
         }});

    server.AddTool(
        {"asset_list",
         "List content: assets (and folders) under a folder, filtered by name and type. Types are importer names such as "
         "Texture, Model, Scene, Prefab.",
         Schema({{"folder", {{"type", "string"}, {"description", "Relative to Content/ (default: the root)"}}},
                 {"search", {{"type", "string"}, {"description", "Case-insensitive part of the name; searches subfolders"}}},
                 {"types", {{"type", "array"}, {"items", {{"type", "string"}}}}},
                 {"recursive", {{"type", "boolean"}}},
                 {"include_sub_assets", {{"type", "boolean"}}},
                 {"limit", {{"type", "integer"}, {"description", "Default 200"}}}}),
         [host](const Json& args) -> Json {
             OpenProject& p = host->Require();
             assets::ContentQuery q;
             q.folder = CleanRelative(OptString(args, "folder"));
             q.search = OptString(args, "search");
             q.types = OptStrings(args, "types");
             q.recursive = OptBool(args, "recursive", false);
             q.include_sub_assets = OptBool(args, "include_sub_assets", false);
             usize limit = 200;
             if (args.contains("limit") && args["limit"].is_number_integer()) limit = static_cast<usize>(std::max<long long>(1, args["limit"].get<long long>()));
             const std::vector<assets::ContentEntry> entries = assets::ListContent(*p.database, q);
             Json folders = Json::array(), list = Json::array();
             for (const assets::ContentEntry& e : entries) {
                 if (e.is_folder) {
                     folders.push_back(e.path);
                 } else if (e.asset && list.size() < limit) {
                     list.push_back(RecordJson(p, *e.asset));
                 }
             }
             return {{"total", entries.size()}, {"folders", folders}, {"assets", list}};
         }});

    server.AddTool({"asset_info",
                    "Everything about one asset: importer, import settings, dependencies, what references it, sub-assets, import state.",
                    Schema({{"asset", {{"type", "string"}, {"description", "Content path or GUID"}}}}, {"asset"}),
                    [host](const Json& args) -> Json {
                        OpenProject& p = host->Require();
                        return RecordJson(p, FindAsset(p, RequireString(args, "asset")), true);
                    }});

    server.AddTool({"asset_references",
                    "The reference graph of an asset: what it uses (dependencies) or what uses it (referencers), to a depth.",
                    Schema({{"asset", {{"type", "string"}}},
                            {"direction", {{"type", "string"}, {"description", "dependencies (default) or referencers"}}},
                            {"max_depth", {{"type", "integer"}, {"description", "Default 4, max 16"}}}},
                           {"asset"}),
                    [host](const Json& args) -> Json {
                        OpenProject& p = host->Require();
                        const AssetRecord& r = FindAsset(p, RequireString(args, "asset"));
                        const std::string dir = OptString(args, "direction", "dependencies");
                        if (dir != "dependencies" && dir != "referencers") throw ToolError("\"direction\" must be dependencies or referencers");
                        long long depth = 4;
                        if (args.contains("max_depth") && args["max_depth"].is_number_integer()) depth = std::clamp<long long>(args["max_depth"].get<long long>(), 1, 16);
                        Json nodes = Json::array();
                        for (const assets::ReferenceNode& n : assets::CollectReferences(
                                 *p.database, r.guid, dir == "referencers" ? assets::ReferenceDirection::Referencers : assets::ReferenceDirection::Dependencies,
                                 static_cast<u32>(depth))) {
                            nodes.push_back({{"path", n.path}, {"depth", n.depth}, {"repeated", n.repeated}});
                        }
                        return {{"asset", r.path}, {"direction", dir}, {"nodes", nodes}};
                    }});

    server.AddTool({"asset_scan",
                    "Rescan Content/: new files get GUIDs, changed sources are flagged needs_import, missing ones are reported.",
                    Schema(Json::object()), [host](const Json&) -> Json { return ScanJson(host->Require().database->Scan()); }});

    server.AddTool(
        {"asset_add_files",
         "Copy source files (textures, models, audio...) into Content/<folder>, then scan and import them. Files keep their names "
         "(a glTF's .bin and textures should be added together, in the same folder). Existing files are not overwritten unless "
         "`overwrite`.",
         Schema({{"sources", {{"type", "array"}, {"items", {{"type", "string"}}}, {"description", "Absolute paths of files to copy in"}}},
                 {"folder", {{"type", "string"}, {"description", "Destination folder under Content/ (created if needed)"}}},
                 {"overwrite", {{"type", "boolean"}}},
                 {"import", {{"type", "boolean"}, {"description", "Import them now (default true)"}}}},
                {"sources"}),
         [host](const Json& args) -> Json {
             OpenProject& p = host->Require();
             const std::vector<std::string> sources = OptStrings(args, "sources");
             if (sources.empty()) throw ToolError("\"sources\" must be a non-empty array of file paths");
             const std::string folder = CleanRelative(OptString(args, "folder"));
             const bool overwrite = OptBool(args, "overwrite", false);
             const fs::path dest_dir = folder.empty() ? p.paths.content : p.paths.content / folder;
             std::error_code ec;
             fs::create_directories(dest_dir, ec);
             Json copied = Json::array();
             for (const std::string& src : sources) {
                 const fs::path from = src;
                 if (!fs::is_regular_file(from, ec)) throw ToolError("Not a file: " + src);
                 std::string why;
                 if (!assets::IsValidAssetName(from.filename().string(), &why)) throw ToolError("Bad file name \"" + from.filename().string() + "\": " + why);
                 const fs::path to = dest_dir / from.filename();
                 if (fs::exists(to, ec) && !overwrite) throw ToolError(to.filename().string() + " already exists in " + (folder.empty() ? "Content/" : folder) + " (pass overwrite)");
                 fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
                 if (ec) throw ToolError("Could not copy " + src + ": " + ec.message());
                 copied.push_back((folder.empty() ? "" : folder + "/") + from.filename().generic_string());
             }
             const assets::ScanResult scan = p.database->Scan();
             Json out = {{"copied", copied}, {"scan", ScanJson(scan)}};
             if (OptBool(args, "import", true)) {
                 const assets::ImportAllResult r = assets::ImportAll(*p.database, p.importers, *p.cache);
                 out["import"] = {{"imported", r.imported}, {"from_cache", r.from_cache}, {"failed", r.failed}, {"skipped", r.skipped}, {"errors", r.errors}};
             }
             Json records = Json::array();
             for (const Json& c : copied) {
                 if (const AssetRecord* r = p.database->FindByPath(c.get<std::string>())) records.push_back(RecordJson(p, *r));
             }
             out["assets"] = records;
             return out;
         }});

    server.AddTool(
        {"asset_import",
         "Run the importers. With `asset`, (re)import that one (even if unchanged); without, import everything flagged needs_import. "
         "Results come from the derived-data cache when the input is unchanged.",
         Schema({{"asset", {{"type", "string"}, {"description", "Content path or GUID; omit to import all pending"}}}}),
         [host](const Json& args) -> Json {
             OpenProject& p = host->Require();
             if (!args.contains("asset")) {
                 const assets::ImportAllResult r = assets::ImportAll(*p.database, p.importers, *p.cache);
                 return {{"imported", r.imported}, {"from_cache", r.from_cache}, {"failed", r.failed}, {"skipped", r.skipped}, {"errors", r.errors}};
             }
             const AssetRecord& rec = FindAsset(p, RequireString(args, "asset"));
             assets::ImportOutput out = assets::ImportAsset(*p.database, rec.guid, p.importers, *p.cache);
             Json j = {{"ok", out.ok}, {"asset", rec.path}, {"from_cache", out.from_cache}, {"bytes", out.data.size()}, {"warnings", out.warnings}};
             Json subs = Json::array();
             for (const assets::SubAssetOutput& s : out.sub_assets) subs.push_back({{"key", s.key}, {"importer", s.importer}, {"guid", assets::ToString(s.guid)}});
             if (!subs.empty()) j["sub_assets"] = subs;
             if (!out.ok) j["error"] = out.error;
             return j;
         }});

    server.AddTool(
        {"asset_set_import_settings",
         "Merge importer settings into an asset's .ameta (e.g. a texture's {\"cook_format\": \"bc7\", \"srgb\": false}) and mark it "
         "for reimport; with `import` (default true) reimport it now. `labels` replaces its labels.",
         Schema({{"asset", {{"type", "string"}}},
                 {"settings", {{"type", "object"}, {"description", "Merged over the current settings (null removes a key)"}}},
                 {"labels", {{"type", "array"}, {"items", {{"type", "string"}}}}},
                 {"import", {{"type", "boolean"}}}},
                {"asset"}),
         [host](const Json& args) -> Json {
             OpenProject& p = host->Require();
             const AssetRecord& rec = FindAsset(p, RequireString(args, "asset"));
             if (rec.IsSubAsset()) throw ToolError("Sub-assets take their source's settings: change " + PathOf(p, rec.parent));
             const AssetGuid guid = rec.guid;
             assets::AssetMeta meta;
             std::string error;
             if (!assets::LoadAssetMeta(p.database->MetaPath(guid), meta, &error)) throw ToolError(error);
             if (args.contains("settings")) {
                 if (!args["settings"].is_object()) throw ToolError("\"settings\" must be an object");
                 meta.settings.merge_patch(args["settings"]);
             }
             if (args.contains("labels")) meta.labels = OptStrings(args, "labels");
             meta.source_hash.clear(); // never imported with these settings
             if (!assets::SaveAssetMeta(p.database->MetaPath(guid), meta, &error)) throw ToolError(error);
             p.database->Scan();
             Json out = {{"settings", meta.settings}, {"labels", meta.labels}};
             if (OptBool(args, "import", true)) {
                 assets::ImportOutput r = assets::ImportAsset(*p.database, guid, p.importers, *p.cache);
                 out["imported"] = r.ok;
                 out["from_cache"] = r.from_cache;
                 out["warnings"] = r.warnings;
                 if (!r.ok) out["error"] = r.error;
             }
             return out;
         }});

    server.AddTool({"asset_move",
                    "Move or rename an asset or a whole folder (both relative to Content/). GUIDs, so scene and prefab references, are "
                    "unchanged; glTF relative URIs are rewritten.",
                    Schema({{"from", {{"type", "string"}}}, {"to", {{"type", "string"}}}}, {"from", "to"}), [host](const Json& args) -> Json {
                        OpenProject& p = host->Require();
                        const assets::MoveResult r = assets::MoveContent(*p.database, CleanRelative(RequireString(args, "from")), CleanRelative(RequireString(args, "to")));
                        if (!r.ok) throw ToolError(r.error);
                        return {{"moved", true}, {"rewritten", r.rewritten}};
                    }});

    server.AddTool({"asset_create_folder", "Create a folder under Content/.",
                    Schema({{"parent", {{"type", "string"}}}, {"name", {{"type", "string"}}}}, {"name"}), [host](const Json& args) -> Json {
                        OpenProject& p = host->Require();
                        std::string error;
                        if (!assets::CreateContentFolder(*p.database, CleanRelative(OptString(args, "parent")), RequireString(args, "name"), &error)) throw ToolError(error);
                        return {{"created", true}};
                    }});

    server.AddTool({"asset_delete",
                    "Delete an asset's source and .ameta (with its sub-assets). Refused while other assets reference it, unless `force`.",
                    Schema({{"asset", {{"type", "string"}}}, {"force", {{"type", "boolean"}}}}, {"asset"}), [host](const Json& args) -> Json {
                        OpenProject& p = host->Require();
                        const AssetRecord& rec = FindAsset(p, RequireString(args, "asset"));
                        const std::string path = rec.path;
                        std::string error;
                        if (!p.database->Delete(rec.guid, OptBool(args, "force", false), &error)) throw ToolError(error);
                        return {{"deleted", path}};
                    }});

    server.AddTool({"content_read", "Read a text file under Content/ (scenes, prefabs, materials, .ameta, JSON assets), up to 2 MB.",
                    Schema({{"path", {{"type", "string"}, {"description", "Relative to Content/"}}}}, {"path"}), [host](const Json& args) -> Json {
                        OpenProject& p = host->Require();
                        const fs::path file = ContentFile(p, RequireString(args, "path"));
                        std::error_code ec;
                        if (!fs::is_regular_file(file, ec)) throw ToolError("No such file: " + RequireString(args, "path"));
                        if (fs::file_size(file, ec) > kMaxTextBytes) throw ToolError("File is larger than 2 MB");
                        std::ifstream in(file, std::ios::binary);
                        std::stringstream ss;
                        ss << in.rdbuf();
                        const std::string text = ss.str();
                        if (text.find('\0') != std::string::npos) throw ToolError("Not a text file (contains NUL bytes)");
                        return {{"path", RequireString(args, "path")}, {"text", text}};
                    }});

    server.AddTool(
        {"content_write",
         "Write a text file under Content/ (a scene, prefab, material or any JSON asset), creating folders; then rescan so a new "
         "file gets a GUID. Refuses .ameta files (use asset_set_import_settings). Overwrites an existing file.",
         Schema({{"path", {{"type", "string"}}}, {"text", {{"type", "string"}}}}, {"path", "text"}), [host](const Json& args) -> Json {
             OpenProject& p = host->Require();
             const std::string rel = RequireString(args, "path");
             const fs::path file = ContentFile(p, rel);
             if (file.extension() == assets::AssetMeta::kExtension) throw ToolError(".ameta files are managed by the asset tools");
             const std::string text = RequireString(args, "text");
             if (text.size() > kMaxTextBytes) throw ToolError("Text is larger than 2 MB");
             std::error_code ec;
             fs::create_directories(file.parent_path(), ec);
             {
                 std::ofstream out(file, std::ios::binary | std::ios::trunc);
                 out.write(text.data(), static_cast<std::streamsize>(text.size()));
                 if (!out) throw ToolError("Could not write " + rel);
             }
             const assets::ScanResult scan = p.database->Scan();
             Json j = {{"path", CleanRelative(rel)}, {"bytes", text.size()}, {"scan", ScanJson(scan)}};
             if (const AssetRecord* r = p.database->FindByPath(CleanRelative(rel))) j["asset"] = RecordJson(p, *r);
             return j;
         }});

    server.AddTool(
        {"asset_cook",
         "Cook the open project into an .apak archive (only what the startup scene, always_cook and `always_cook` reach, with "
         "textures block-compressed and meshes given LODs). Runs the importers as needed. The result's pak_file can be given to "
         "game_load. Blocks until done.",
         Schema({{"output_dir", {{"type", "string"}, {"description", "Gets <pak_name>.apak and CookManifest.json"}}},
                 {"configuration", {{"type", "string"}, {"description", "debug, development (default) or shipping"}}},
                 {"always_cook", {{"type", "array"}, {"items", {{"type", "string"}}}, {"description", "Extra roots: asset paths or folders ending in /"}}},
                 {"pak_name", {{"type", "string"}, {"description", "Default Game"}}},
                 {"compression", {{"type", "string"}, {"description", "auto (default), lz4, zstd or none"}}},
                 {"strict", {{"type", "boolean"}, {"description", "Refuse to write the archive if anything warns"}}},
                 {"include_imported", {{"type", "boolean"}}},
                 {"cook_textures", {{"type", "boolean"}}},
                 {"cook_meshes", {{"type", "boolean"}}},
                 {"key", {{"type", "string"}, {"description", "64 hex digits: encrypt the archive"}}},
                 {"patch_of", {{"type", "string"}, {"description", "Earlier .apak: write only what changed since"}}},
                 {"max_assets", {{"type", "integer"}, {"description", "How many cooked assets to list (default 100)"}}}},
                {"output_dir"}),
         [host](const Json& args) -> Json {
             OpenProject& p = host->Require();
             cook::CookOptions o;
             o.project_file = p.paths.file;
             o.output_dir = RequireString(args, "output_dir");
             if (args.contains("configuration") && !cook::ParseConfiguration(OptString(args, "configuration"), o.configuration)) {
                 throw ToolError("\"configuration\" must be debug, development or shipping");
             }
             o.always_cook = OptStrings(args, "always_cook");
             o.pak_name = OptString(args, "pak_name", "Game");
             const std::string c = OptString(args, "compression", "auto");
             if (c == "lz4") o.compression = pak::CompressionPolicy::LZ4;
             else if (c == "zstd") o.compression = pak::CompressionPolicy::Zstd;
             else if (c == "none") o.compression = pak::CompressionPolicy::None;
             else if (c != "auto") throw ToolError("\"compression\" must be auto, lz4, zstd or none");
             o.strict_validation = OptBool(args, "strict", false);
             o.include_imported = OptBool(args, "include_imported", true);
             o.cook_textures = OptBool(args, "cook_textures", true);
             o.cook_meshes = OptBool(args, "cook_meshes", true);
             if (args.contains("key")) {
                 pak::PakKey key;
                 if (!pak::PakKey::FromHex(OptString(args, "key"), key)) throw ToolError("\"key\" must be 64 hex digits");
                 o.encryption_key = key;
             }
             if (args.contains("patch_of")) o.patch_base = OptString(args, "patch_of");

             const cook::CookReport r = cook::Cook(o);
             p.database->Scan(); // the cook may have made .ameta files
             if (!r.ok) {
                 Json fail = {{"ok", false}, {"error", KindOfCookFailure(r)}, {"warnings", r.warnings}};
                 return fail;
             }
             usize limit = 100;
             if (args.contains("max_assets") && args["max_assets"].is_number_integer()) limit = static_cast<usize>(std::max<long long>(0, args["max_assets"].get<long long>()));
             Json listed = Json::array();
             for (const cook::CookedAsset& a : r.assets) {
                 if (listed.size() >= limit) break;
                 listed.push_back({{"path", a.path}, {"importer", a.importer}, {"reason", a.reason}, {"bytes", a.bytes}, {"cooked", a.cooked}});
             }
             return {{"ok", true},
                     {"pak_file", r.pak_file.string()},
                     {"manifest_file", r.manifest_file.string()},
                     {"assets_cooked", r.assets.size()},
                     {"assets_skipped", r.skipped},
                     {"original_bytes", r.original_bytes},
                     {"pak_bytes", r.pak_bytes},
                     {"stripped_fields", r.stripped_fields},
                     {"texture_cache_hits", r.texture_cache_hits},
                     {"texture_cache_misses", r.texture_cache_misses},
                     {"plugins", r.plugins},
                     {"modules", r.modules},
                     {"warnings", r.warnings},
                     {"assets", listed},
                     {"is_patch", r.is_patch}};
         }});
}

} // namespace aether::mcp
