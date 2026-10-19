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

inline fs::path CanonicalContentRoot(OpenProject& project) {
    std::error_code ec;
    const fs::file_status status = fs::symlink_status(project.paths.content, ec);
    if (ec) throw ToolError("Could not inspect Content/: " + ec.message());
    if (fs::is_symlink(status)) throw ToolError("Content/ cannot be a symbolic link");
    if (!fs::is_directory(status)) throw ToolError("Content/ is not a folder");
    const fs::path root = fs::canonical(project.paths.content, ec);
    if (ec) throw ToolError("Could not resolve Content/: " + ec.message());
    return root;
}

// Resolve a content-relative path only when every existing component is a
// normal directory or file. Reject symlinks before reads or writes can follow
// them outside the project. A missing suffix is allowed for new paths.
inline fs::path ContentPath(OpenProject& project, const std::string& relative) {
    const std::string rel = CleanRelative(relative);
    if (rel.empty()) throw ToolError("Path must name a file under Content/");
    const fs::path root = CanonicalContentRoot(project);
    std::error_code ec;
    const fs::path relative_path(rel);
    fs::path current = root;
    for (auto part = relative_path.begin(); part != relative_path.end(); ++part) {
        current /= *part;
        auto next = part;
        ++next;
        const fs::file_status status = fs::symlink_status(current, ec);
        if (ec == std::errc::no_such_file_or_directory) return root / relative_path;
        if (ec) throw ToolError("Could not inspect Content path " + current.string() + ": " + ec.message());
        if (fs::is_symlink(status)) throw ToolError("Content paths cannot traverse symbolic links: " + rel);
        if (status.type() == fs::file_type::not_found) return root / relative_path;
        if (next != relative_path.end() && !fs::is_directory(status)) {
            throw ToolError("A parent of the Content path is not a folder: " + rel);
        }
    }
    return root / relative_path;
}

inline fs::path ContentFile(OpenProject& project, const std::string& relative) {
    const fs::path path = ContentPath(project, relative);
    std::error_code ec;
    const fs::file_status status = fs::symlink_status(path, ec);
    if (!ec && status.type() != fs::file_type::not_found && !fs::is_regular_file(status)) {
        throw ToolError("Content path is not a regular file: " + relative);
    }
    if (ec && ec != std::errc::no_such_file_or_directory) throw ToolError("Could not inspect Content path " + path.string() + ": " + ec.message());
    return path;
}

inline fs::path ContentDirectory(OpenProject& project, const std::string& relative) {
    if (CleanRelative(relative).empty()) {
        return CanonicalContentRoot(project);
    }
    const fs::path path = ContentPath(project, relative);
    std::error_code ec;
    const fs::file_status status = fs::symlink_status(path, ec);
    if (!ec && status.type() != fs::file_type::not_found && !fs::is_directory(status)) {
        throw ToolError("Content path is not a folder: " + relative);
    }
    if (ec && ec != std::errc::no_such_file_or_directory) throw ToolError("Could not inspect Content path " + path.string() + ": " + ec.message());
    return path;
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
