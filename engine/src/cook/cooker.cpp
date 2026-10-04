#include "aether/cook/cooker.h"

#include "aether/assets/derived_data_cache.h"
#include "aether/assets/gltf_references.h"
#include "aether/assets/importer.h"
#include "aether/core/log.h"
#include "aether/platform/filesystem.h"
#include "aether/project/project.h"
#include "aether/reflection/registry.h"

#include <algorithm>
#include <deque>
#include <set>

namespace aether::cook {

using assets::AssetDatabase;
using assets::AssetGuid;
using assets::AssetRecord;
using nlohmann::json;
namespace stdfs = std::filesystem;

const char* ConfigurationName(BuildConfiguration c) {
    switch (c) {
        case BuildConfiguration::Debug: return "Debug";
        case BuildConfiguration::Development: return "Development";
        case BuildConfiguration::Shipping: return "Shipping";
    }
    return "?";
}

bool ParseConfiguration(const std::string& name, BuildConfiguration& out) {
    std::string lower;
    for (char c : name) lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (lower == "debug") out = BuildConfiguration::Debug;
    else if (lower == "development" || lower == "dev") out = BuildConfiguration::Development;
    else if (lower == "shipping" || lower == "ship") out = BuildConfiguration::Shipping;
    else return false;
    return true;
}

// --- Editor-only data ---------------------------------------------------------

namespace {

usize StripValue(json& value, const reflect::TypeInfo& type) {
    usize removed = 0;
    if (type.kind == reflect::TypeKind::Struct && value.is_object()) {
        for (const reflect::FieldInfo& field : type.fields) {
            const auto it = value.find(field.name);
            if (it == value.end()) continue;
            if (field.HasFlag(reflect::Field_EditorOnly)) {
                value.erase(it);
                ++removed;
            } else if (field.type) {
                removed += StripValue(*it, *field.type);
            }
        }
    } else if (type.kind == reflect::TypeKind::Struct && value.is_array() && type.serialize_as_array) {
        // Positional: nothing can be removed without shifting the rest.
    } else if (type.kind == reflect::TypeKind::Array && value.is_array() && type.element) {
        for (json& element : value) removed += StripValue(element, *type.element);
    }
    return removed;
}

} // namespace

usize StripEditorOnly(json& document) {
    usize removed = 0;
    const auto entities = document.find("entities");
    if (entities == document.end() || !entities->is_array()) return 0;
    for (json& entity : *entities) {
        const auto components = entity.find("components");
        if (components == entity.end() || !components->is_object()) continue;
        for (auto it = components->begin(); it != components->end(); ++it) {
            if (const reflect::TypeInfo* type = reflect::TypeRegistry::Find(it.key())) {
                removed += StripValue(it.value(), *type);
            }
        }
    }
    return removed;
}

// --- What to cook ----------------------------------------------------------------

std::vector<AssetGuid> CollectCookSet(const AssetDatabase& database,
                                      const std::vector<std::pair<std::string, std::string>>& roots,
                                      std::map<AssetGuid, std::string>& reasons, std::vector<std::string>& warnings) {
    std::deque<AssetGuid> queue;
    const auto include = [&](const AssetGuid& guid, const std::string& reason) {
        if (guid.IsNull() || reasons.count(guid)) return;
        reasons[guid] = reason;
        queue.push_back(guid);
    };
    const std::vector<const AssetRecord*> all = database.All();
    for (const auto& [root, reason] : roots) {
        if (!root.empty() && root.back() == '/') {
            usize matched = 0;
            for (const AssetRecord* record : all) {
                if (!record->IsSubAsset() && !record->missing && record->path.compare(0, root.size(), root) == 0) {
                    include(record->guid, reason);
                    ++matched;
                }
            }
            if (matched == 0) warnings.push_back("Nothing to cook in folder " + root);
        } else if (const AssetRecord* record = database.FindByPath(root)) {
            if (record->missing) {
                warnings.push_back(root + " (" + reason + ") is missing its source file");
            } else {
                include(record->guid, reason);
            }
        } else {
            warnings.push_back(root + " (" + reason + ") isn't an asset in the content folder");
        }
    }
    std::vector<AssetGuid> result;
    while (!queue.empty()) {
        const AssetGuid guid = queue.front();
        queue.pop_front();
        const AssetRecord* record = database.Find(guid);
        if (!record) continue;
        if (record->IsSubAsset()) {
            // A sub-asset ships as its source (which brings the rest along).
            include(record->parent, reasons[guid]);
            continue;
        }
        result.push_back(guid);
        for (const AssetGuid& dependency : record->dependencies) {
            const AssetRecord* dep = database.Find(dependency);
            if (!dep || dep->missing) {
                warnings.push_back(record->path + " refers to a missing asset " + assets::ToString(dependency));
                continue;
            }
            include(dependency, "used by " + record->path);
        }
    }
    std::sort(result.begin(), result.end(), [&](const AssetGuid& a, const AssetGuid& b) {
        return database.Find(a)->path < database.Find(b)->path;
    });
    return result;
}

// --- Cook ------------------------------------------------------------------------

CookReport Cook(const CookOptions& options) {
    CookReport report;
    const auto fail = [&](const std::string& message) {
        report.ok = false;
        report.error = message;
        AETHER_LOG_ERROR("Cook", "%s", message.c_str());
        return report;
    };

    ProjectSettings settings;
    std::string error;
    if (!LoadProject(options.project_file, settings, &error, &report.warnings)) {
        return fail("Can't load " + options.project_file.string() + ": " + error);
    }
    const ProjectPaths paths = ProjectPaths::ForFile(options.project_file);
    AssetDatabase database(paths.content);
    const assets::ScanResult scan = database.Scan();
    report.warnings.insert(report.warnings.end(), scan.warnings.begin(), scan.warnings.end());

    std::vector<std::pair<std::string, std::string>> roots;
    if (settings.startup_scene.empty()) {
        report.warnings.push_back("The project has no startup scene");
    } else {
        roots.push_back({settings.startup_scene, "startup scene"});
    }
    for (const std::string& path : settings.always_cook) roots.push_back({path, "always cook"});
    for (const std::string& path : options.always_cook) roots.push_back({path, "always cook"});
    if (roots.empty()) return fail("Nothing to cook: set a startup scene or always_cook");

    std::map<AssetGuid, std::string> reasons;
    const std::vector<AssetGuid> cook_set = CollectCookSet(database, roots, reasons, report.warnings);
    if (cook_set.empty()) return fail("Nothing to cook: none of the roots is an asset");

    usize sources = 0;
    for (const AssetRecord* record : database.All()) {
        if (!record->IsSubAsset() && !record->missing) ++sources;
    }
    report.skipped = sources - cook_set.size();

    pak::PakWriter writer(options.compression);
    const assets::ImporterRegistry importers = assets::ImporterRegistry::WithBuiltins();
    assets::DerivedDataCache cache(paths.intermediate / "DerivedDataCache");
    std::set<std::string> extra;
    json manifest_assets = json::array();

    for (const AssetGuid& guid : cook_set) {
        const AssetRecord& record = *database.Find(guid);
        CookedAsset cooked;
        cooked.guid = guid;
        cooked.path = record.path;
        cooked.importer = record.importer;
        cooked.reason = reasons[guid];

        std::vector<u8> bytes;
        if (!fs::ReadFileBytes(database.SourcePath(guid).string(), bytes)) {
            return fail("Can't read " + record.path);
        }
        if (record.importer == "Scene" || record.importer == "Prefab") {
            json document = json::parse(bytes.begin(), bytes.end(), nullptr, false);
            if (document.is_discarded()) {
                report.warnings.push_back(record.path + " isn't valid JSON; cooked as it is");
            } else {
                report.stripped_fields += StripEditorOnly(document);
                const std::string text = document.dump(); // minified
                bytes.assign(text.begin(), text.end());
            }
        }
        cooked.bytes = bytes.size();
        report.original_bytes += bytes.size();
        writer.Add("Content/" + record.path, bytes);

        // A .gltf's buffers and images that aren't assets themselves.
        if (record.importer == "Model") {
            for (const assets::GltfFileReference& ref : assets::GltfFileReferences(paths.content, record.path)) {
                if (ref.path.empty()) {
                    report.warnings.push_back(record.path + " refers to " + ref.uri + ", outside the content folder");
                } else if (!database.FindByPath(ref.path)) {
                    extra.insert(ref.path);
                }
            }
        }

        json sub_assets = json::array();
        if (options.include_imported && importers.Find(record.importer)) {
            const assets::ImportOutput output = assets::ImportAsset(database, guid, importers, cache, options.platform);
            if (output.ok) {
                cooked.imported = true;
                writer.Add("Imported/" + assets::ToString(guid) + ".bin", output.data);
                report.original_bytes += output.data.size();
                for (const assets::SubAssetOutput& sub : output.sub_assets) {
                    writer.Add("Imported/" + assets::ToString(sub.guid) + ".bin", sub.data);
                    report.original_bytes += sub.data.size();
                    sub_assets.push_back({{"guid", assets::ToString(sub.guid)}, {"key", sub.key}, {"importer", sub.importer}});
                }
            } else {
                report.warnings.push_back("Importing " + record.path + " failed: " + output.error);
            }
        }

        json deps = json::array();
        for (const AssetGuid& d : record.dependencies) deps.push_back(assets::ToString(d));
        manifest_assets.push_back({{"guid", assets::ToString(guid)},
                                   {"path", record.path},
                                   {"importer", record.importer},
                                   {"dependencies", std::move(deps)},
                                   {"imported", cooked.imported},
                                   {"sub_assets", std::move(sub_assets)}});
        report.assets.push_back(std::move(cooked));
    }

    for (const std::string& path : extra) {
        std::vector<u8> bytes;
        if (!fs::ReadFileBytes((paths.content / path).string(), bytes)) {
            report.warnings.push_back("Can't read " + path + ", which a model refers to");
            continue;
        }
        writer.Add("Content/" + path, bytes);
        report.original_bytes += bytes.size();
        report.extra_files.push_back(path);
    }

    json files = json::array();
    for (const std::string& path : extra) files.push_back(path);
    const json manifest = {
        {"$type", "CookManifest"},
        {"$version", 1},
        {"project", settings.name},
        {"configuration", ConfigurationName(options.configuration)},
        {"startup_scene", settings.startup_scene},
        {"fixed_timestep_hz", settings.fixed_timestep_hz},
        {"gravity", {settings.gravity.x, settings.gravity.y, settings.gravity.z}},
        {"layers", settings.layers},
        {"collision_matrix", settings.collision_matrix},
        {"assets", std::move(manifest_assets)},
        {"files", std::move(files)},
    };
    writer.Add("Manifest.json", manifest.dump(options.configuration == BuildConfiguration::Shipping ? -1 : 2));

    std::error_code ec;
    stdfs::create_directories(options.output_dir, ec);
    report.pak_file = options.output_dir / (options.pak_name + ".apak");
    if (!writer.Write(report.pak_file.string(), &error)) return fail(error);
    report.pak_bytes = stdfs::file_size(report.pak_file, ec);
    report.manifest_file = options.output_dir / "CookManifest.json";
    const std::string text = manifest.dump(2);
    if (!fs::WriteFileBytes(report.manifest_file.string(), text.data(), text.size())) {
        return fail("Can't write " + report.manifest_file.string());
    }
    report.ok = true;
    AETHER_LOG_INFO("Cook", "Cooked %zu assets (%zu skipped) into %s: %llu bytes -> %llu bytes",
                    report.assets.size(), report.skipped, report.pak_file.string().c_str(),
                    static_cast<unsigned long long>(report.original_bytes),
                    static_cast<unsigned long long>(report.pak_bytes));
    return report;
}

} // namespace aether::cook
