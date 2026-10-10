#pragma once

// What the project tools (asset_tools.cpp) and the gameplay-kit data tools
// (kit_tools.cpp) share: the open project (settings, asset database, derived-data
// cache, importers) and small argument and JSON helpers.

#include "aether/assets/asset_database.h"
#include "aether/assets/content_browser.h"
#include "aether/assets/derived_data_cache.h"
#include "aether/assets/importer.h"
#include "aether/project/project.h"
#include "mcp_server.h"

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace aether::mcp::detail {

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

inline Json Schema(Json properties, std::vector<std::string> required = {}) {
    Json schema = {{"type", "object"}, {"properties", std::move(properties)}};
    if (!required.empty()) schema["required"] = required;
    return schema;
}

inline std::string RequireString(const Json& args, const char* key) {
    if (!args.contains(key) || !args[key].is_string()) {
        throw ToolError(std::string("Missing string argument \"") + key + "\"");
    }
    return args[key].get<std::string>();
}

inline std::string OptString(const Json& args, const char* key, const std::string& fallback = "") {
    if (!args.contains(key)) return fallback;
    if (!args[key].is_string()) throw ToolError(std::string("\"") + key + "\" must be a string");
    return args[key].get<std::string>();
}

inline bool OptBool(const Json& args, const char* key, bool fallback) {
    if (!args.contains(key)) return fallback;
    if (!args[key].is_boolean()) throw ToolError(std::string("\"") + key + "\" must be a boolean");
    return args[key].get<bool>();
}

inline std::vector<std::string> OptStrings(const Json& args, const char* key) {
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
inline std::string CleanRelative(const std::string& path) {
    if (path.empty()) return {};
    const fs::path p = fs::path(path).lexically_normal();
    if (p.is_absolute() || p.has_root_name() || p.has_root_directory()) throw ToolError("Path must be relative to Content/: " + path);
    std::string s = p.generic_string();
    while (!s.empty() && s.back() == '/') s.pop_back();
    if (s == "." ) return {};
    if (s == ".." || s.compare(0, 3, "../") == 0) throw ToolError("Path must stay inside Content/: " + path);
    return s;
}

inline const AssetRecord& FindAsset(OpenProject& p, const std::string& key) {
    AssetGuid guid;
    if (assets::ParseAssetGuid(key, guid)) {
        if (const AssetRecord* r = p.database->Find(guid)) return *r;
    }
    if (const AssetRecord* r = p.database->FindByPath(CleanRelative(key))) return *r;
    throw ToolError("No asset \"" + key + "\" (a content path like Textures/a.png, or a GUID)");
}

inline std::string PathOf(OpenProject& p, const AssetGuid& guid) {
    const AssetRecord* r = p.database->Find(guid);
    return r ? r->path : assets::ToString(guid);
}

inline Json RecordJson(OpenProject& p, const AssetRecord& r, bool detail = false) {
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

inline Json ScanJson(const assets::ScanResult& s) {
    return {{"assets", s.assets}, {"created_meta", s.created_meta}, {"missing", s.missing},
            {"duplicates_fixed", s.duplicates_fixed}, {"needs_import", s.needs_import}, {"warnings", s.warnings}};
}


} // namespace aether::mcp::detail
