#include "aether/cook/cooker.h"

#include "aether/assets/derived_data_cache.h"
#include "aether/assets/gltf_references.h"
#include "aether/assets/image.h"
#include "aether/assets/importer.h"
#include "aether/cook/mesh_cook.h"
#include "aether/cook/texture_cook.h"
#include "aether/core/log.h"
#include "aether/platform/filesystem.h"
#include "aether/plugin/plugin.h"
#include "aether/project/project.h"
#include "aether/reflection/registry.h"
#include "aether/reflection/serialize.h"

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
    // String tables are referenced by no asset, so without this they'd have
    // to be listed by hand and a forgotten one would silently lose its text.
    for (const AssetRecord* record : all) {
        if (!record->IsSubAsset() && !record->missing && record->importer == "StringTable") include(record->guid, "localization");
        // Gameplay effects are applied by name from Blueprints, so nothing references them either.
        if (!record->IsSubAsset() && !record->missing && record->importer == "GameplayEffect") include(record->guid, "gameplay");
        if (!record->IsSubAsset() && !record->missing && record->importer == "GameplayAbility") include(record->guid, "gameplay");
        if (!record->IsSubAsset() && !record->missing && record->importer == "ItemDefinition") include(record->guid, "gameplay");
        if (!record->IsSubAsset() && !record->missing && record->importer == "QuestDefinition") include(record->guid, "gameplay");
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

    const auto progress = [&](f32 fraction, const std::string& stage) {
        if (options.progress) options.progress(fraction, stage);
    };
    const auto cancelled = [&] { return options.cancel && options.cancel->load(); };

    // pak_name is a filename stem, not an output path. A relative traversal
    // here would bypass Package's staging directory before verification.
    if (options.pak_name.empty() || options.pak_name.find_first_of("/\\:") != std::string::npos ||
        options.pak_name.find('\0') != std::string::npos) {
        return fail("Invalid pak name: expected a single filename stem without path separators, ':' or NUL");
    }

    progress(0.0f, "Scanning the project");
    ProjectSettings settings;
    std::string error;
    if (!LoadProject(options.project_file, settings, &error, &report.warnings)) {
        return fail("Can't load " + options.project_file.string() + ": " + error);
    }
    const ProjectPaths paths = ProjectPaths::ForFile(options.project_file);
    AssetDatabase database(paths.content);
    const assets::ScanResult scan = database.Scan();
    report.warnings.insert(report.warnings.end(), scan.warnings.begin(), scan.warnings.end());

    // The project's plugins: what the game starts, and their content.
    plugin::PluginManager plugins;
    if (!plugin::ResolveProjectPlugins(options.project_file, plugins, &error, &report.warnings)) {
        return fail("Plugins: " + error);
    }
    for (const plugin::PluginInfo* p : plugins.Enabled()) report.plugins.push_back(p->descriptor.name);
    report.modules = plugins.ModuleOrder(/*runtime=*/true, /*editor=*/false);

    const bool dlc = !options.dlc_name.empty();
    if (dlc && pak::NormalizePath(options.dlc_name) != options.dlc_name) return fail("Invalid DLC name '" + options.dlc_name + "'");
    std::vector<pak::PakKey> keys;
    if (options.encryption_key) keys.push_back(*options.encryption_key);

    std::vector<std::pair<std::string, std::string>> roots;
    if (dlc) {
        for (const std::string& path : options.always_cook) roots.push_back({path, "DLC " + options.dlc_name});
        if (roots.empty()) return fail("Nothing to cook: a DLC cooks its always_cook roots, and none were given");
    } else {
        if (settings.startup_scene.empty()) {
            report.warnings.push_back("The project has no startup scene");
        } else {
            roots.push_back({settings.startup_scene, "startup scene"});
        }
        for (const std::string& path : settings.always_cook) roots.push_back({path, "always cook"});
        for (const std::string& path : options.always_cook) roots.push_back({path, "always cook"});
        if (roots.empty()) return fail("Nothing to cook: set a startup scene or always_cook");
    }

    std::map<AssetGuid, std::string> reasons;
    std::vector<AssetGuid> cook_set = CollectCookSet(database, roots, reasons, report.warnings);
    if (dlc && !options.dlc_base.empty()) {
        // What the base game ships already stays out of the DLC.
        pak::PakReader base;
        if (!base.Open(options.dlc_base.string(), &error, keys)) return fail("Can't open the base archive: " + error);
        std::vector<AssetGuid> kept;
        for (const AssetGuid& guid : cook_set) {
            if (base.Contains("Content/" + database.Find(guid)->path)) {
                ++report.in_base;
            } else {
                kept.push_back(guid);
            }
        }
        cook_set = std::move(kept);
    }
    if (cook_set.empty()) return fail("Nothing to cook: none of the roots is an asset" + std::string(report.in_base ? " the base doesn't have" : ""));

    usize sources = 0;
    for (const AssetRecord* record : database.All()) {
        if (!record->IsSubAsset() && !record->missing) ++sources;
    }
    report.skipped = sources - cook_set.size();

    pak::PakWriter writer(options.compression);
    if (options.encryption_key) writer.SetEncryption(*options.encryption_key);
    const assets::ImporterRegistry importers = assets::ImporterRegistry::WithBuiltins();
    assets::DerivedDataCache cache(paths.intermediate / "DerivedDataCache");
    std::set<std::string> extra;
    json manifest_assets = json::array();

    if (!settings.quality_presets.empty() && !FindQualityPreset(settings, settings.default_quality)) {
        report.warnings.push_back("The default quality '" + settings.default_quality + "' isn't a preset; the game starts on '" +
                                  settings.quality_presets.front().name + "'");
    }

    for (usize index = 0; index < cook_set.size(); ++index) {
        const AssetGuid& guid = cook_set[index];
        const AssetRecord& record = *database.Find(guid);
        if (cancelled()) return fail("Cancelled");
        progress(0.05f + 0.85f * static_cast<f32>(index) / static_cast<f32>(cook_set.size()), "Cooking " + record.path);
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
            for (const std::string& warning : output.warnings) {
                report.warnings.push_back(record.path + ": " + warning);
            }
            if (output.ok) {
                cooked.imported = true;
                writer.Add("Imported/" + assets::ToString(guid) + ".bin", output.data);
                report.original_bytes += output.data.size();
                for (const assets::SubAssetOutput& sub : output.sub_assets) {
                    writer.Add("Imported/" + assets::ToString(sub.guid) + ".bin", sub.data);
                    report.original_bytes += sub.data.size();
                    sub_assets.push_back({{"guid", assets::ToString(sub.guid)}, {"key", sub.key}, {"importer", sub.importer}});
                    if (options.cook_meshes && sub.importer == "Mesh") {
                        assets::MeshData mesh_data;
                        CookedMesh cooked_mesh;
                        std::string mesh_error;
                        if (!assets::DecodeMeshData(sub.data, mesh_data) ||
                            !CookMesh(mesh_data, MeshCookSettings{}, cooked_mesh, &mesh_error)) {
                            report.warnings.push_back(record.path + " (" + sub.key + "): can't cook mesh" +
                                                      (mesh_error.empty() ? "" : ": " + mesh_error));
                        } else {
                            // cooked_mesh.warnings (skinned primitives) stay out of the report so strict_validation projects keep cooking.
                            const std::vector<u8> amesh = SaveAmesh(cooked_mesh);
                            writer.Add("Cooked/" + assets::ToString(sub.guid) + ".amesh", amesh);
                            report.original_bytes += amesh.size();
                        }
                    }
                }
            } else {
                report.warnings.push_back("Importing " + record.path + " failed: " + output.error);
            }
        }

        // Textures: the GPU-ready form.
        std::string cooked_format;
        if (options.cook_textures && record.importer == "Texture") {
            assets::AssetMeta meta;
            json settings_json = json::object();
            if (assets::LoadAssetMeta(database.MetaPath(guid), meta)) settings_json = meta.settings;
            TextureCookSettings tex;
            tex.quality = options.texture_quality;
            const auto bool_setting = [&](const char* name, bool fallback) {
                const auto it = settings_json.find(name);
                if (it == settings_json.end()) return fallback;
                if (it->is_boolean()) return it->get<bool>();
                report.warnings.push_back(record.path + ": setting '" + name + "' must be a boolean; using the default");
                return fallback;
            };
            tex.normal_map = bool_setting("normal_map", false);
            tex.srgb = bool_setting("srgb", !tex.normal_map);
            tex.mips = bool_setting("mips", true);
            std::string format = "auto";
            if (const auto it = settings_json.find("cook_format"); it != settings_json.end()) {
                if (it->is_string()) format = it->get<std::string>();
                else report.warnings.push_back(record.path + ": setting 'cook_format' must be a string; using auto");
            }
            if (format != "auto") {
                if (ParseTextureFormat(format, tex.format)) {
                    tex.auto_format = false;
                } else {
                    report.warnings.push_back(record.path + ": unknown cook_format '" + format + "', using auto");
                }
            }
            const json texture_settings = {{"quality", tex.quality},
                                           {"auto_format", tex.auto_format},
                                           {"format", TextureFormatName(tex.format)},
                                           {"mips", tex.mips},
                                           {"srgb", tex.srgb},
                                           {"normal_map", tex.normal_map}};
            const std::string key = assets::DerivedDataCache::MakeKey(
                "CookedTexture", 1, record.source_hash, texture_settings.dump(), options.platform);
            std::optional<std::vector<u8>> cached = cache.Get(key);
            CookedTexture cooked_texture;
            const bool cache_hit = cached && LoadAtex(*cached, cooked_texture);
            std::vector<u8> atex;
            if (cache_hit) {
                ++report.texture_cache_hits;
                atex = std::move(*cached);
            } else {
                ++report.texture_cache_misses;
                assets::ImageData image;
                if (assets::DecodeImageFile(database.SourcePath(guid).string(), image)) {
                    cooked_texture = CookTexture(image.pixels, image.width, image.height, tex);
                    atex = SaveAtex(cooked_texture);
                    if (!cache.Put(key, atex)) report.warnings.push_back("Can't cache cooked texture " + record.path);
                } else {
                    report.warnings.push_back("Can't decode " + record.path + " to cook it");
                }
            }
            if (!atex.empty()) {
                cooked.cooked = "Cooked/" + assets::ToString(guid) + ".atex";
                cooked.cooked_bytes = atex.size();
                cooked_format = TextureFormatName(cooked_texture.format);
                writer.Add(cooked.cooked, atex);
                report.original_bytes += atex.size();
            }
        }

        json deps = json::array();
        for (const AssetGuid& d : record.dependencies) deps.push_back(assets::ToString(d));
        manifest_assets.push_back({{"guid", assets::ToString(guid)},
                                   {"path", record.path},
                                   {"importer", record.importer},
                                   {"dependencies", std::move(deps)},
                                   {"imported", cooked.imported},
                                   {"cooked", cooked.cooked},
                                   {"cooked_format", cooked_format},
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

    if (!dlc) {
        for (const auto& [dir, mount] : plugins.ContentMounts()) {
            const usize added = writer.AddDirectory(dir.string(), "Content/" + mount.substr(0, mount.size() - 1));
            if (added == 0) report.warnings.push_back("The plugin content folder " + dir.string() + " is empty");
        }
    }
    if (options.strict_validation && !report.warnings.empty()) {
        return fail("Cook validation failed with " + std::to_string(report.warnings.size()) + " warning(s); first: " +
                    report.warnings.front());
    }
    json files = json::array();
    for (const std::string& path : extra) files.push_back(path);
    json presets = json::array();
    for (const QualityPreset& p : settings.quality_presets) presets.push_back(reflect::ToJson(p));
    if (cancelled()) return fail("Cancelled");
    progress(0.92f, "Writing " + options.pak_name + ".apak");
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
        {"window",
         {{"title", settings.window_title.empty() ? settings.name : settings.window_title},
          {"width", settings.window_width},
          {"height", settings.window_height},
          {"vsync", settings.vsync}}},
        {"quality_presets", std::move(presets)},
        {"default_quality", settings.default_quality},
        {"plugins", report.plugins},
        {"modules", report.modules},
        {"assets", std::move(manifest_assets)},
        {"files", std::move(files)},
    };
    const int indent = options.configuration == BuildConfiguration::Shipping ? -1 : 2;
    if (dlc) {
        const json dlc_manifest = {{"$type", "DlcManifest"},
                                   {"$version", 1},
                                   {"name", options.dlc_name},
                                   {"project", settings.name},
                                   {"assets", manifest["assets"]},
                                   {"files", manifest["files"]}};
        writer.Add("DLC/" + options.dlc_name + ".json", dlc_manifest.dump(indent));
    } else {
        writer.Add("Manifest.json", manifest.dump(indent));
    }

    std::error_code ec;
    stdfs::create_directories(options.output_dir, ec);
    if (ec) return fail("Can't create " + options.output_dir.string() + ": " + ec.message());
    report.pak_file = options.output_dir / (options.pak_name + ".apak");
    if (!options.patch_base.empty()) {
        // Only what differs from the earlier archive.
        pak::PakReader base, full;
        if (!base.Open(options.patch_base.string(), &error, keys)) return fail("Can't open the patch base: " + error);
        if (!full.OpenMemory(writer.Build(), &error, keys)) return fail(error);
        pak::PakWriter patch(options.compression);
        if (options.encryption_key) patch.SetEncryption(*options.encryption_key);
        report.patch = pak::MakePatch(base, full, patch);
        report.is_patch = true;
        if (!patch.Write(report.pak_file.string(), &error)) return fail(error);
    } else if (!writer.Write(report.pak_file.string(), &error)) {
        return fail(error);
    }
    report.pak_bytes = stdfs::file_size(report.pak_file, ec);
    report.manifest_file = options.output_dir / "CookManifest.json";
    const std::string text = manifest.dump(2);
    if (!fs::WriteFileBytes(report.manifest_file.string(), text.data(), text.size())) {
        return fail("Can't write " + report.manifest_file.string());
    }
    report.ok = true;
    progress(1.0f, "Done");
    AETHER_LOG_INFO("Cook", "Cooked %zu assets (%zu skipped) into %s: %llu bytes -> %llu bytes",
                    report.assets.size(), report.skipped, report.pak_file.string().c_str(),
                    static_cast<unsigned long long>(report.original_bytes),
                    static_cast<unsigned long long>(report.pak_bytes));
    return report;
}

} // namespace aether::cook
