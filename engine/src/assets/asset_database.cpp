#include "aether/assets/asset_database.h"
#include "aether/assets/asset_ref.h"
#include "aether/assets/gltf_references.h"

#include "aether/core/log.h"
#include "aether/platform/filesystem.h"
#include "aether/reflection/serialize.h"
#include "aether/scene/entity_guid.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <map>
#include <system_error>

namespace aether::assets {

namespace stdfs = std::filesystem;
using reflect::Json;

// ---------------------------------------------------------------------------
// AssetGuid (same generation and text form as EntityGuid)
// ---------------------------------------------------------------------------

AssetGuid NewAssetGuid() {
    EntityGuid g = NewEntityGuid();
    return AssetGuid{g.hi, g.lo};
}

std::string ToString(const AssetGuid& guid) { return aether::ToString(EntityGuid{guid.hi, guid.lo}); }

bool ParseAssetGuid(std::string_view text, AssetGuid& out) {
    EntityGuid g;
    if (!ParseEntityGuid(text, g)) {
        return false;
    }
    out = AssetGuid{g.hi, g.lo};
    return true;
}

namespace detail {
nlohmann::json AssetGuidToJson(const void* object) { return ToString(*static_cast<const AssetGuid*>(object)); }
bool AssetGuidFromJson(const nlohmann::json& data, void* object) {
    return data.is_string() && ParseAssetGuid(data.get_ref<const std::string&>(), *static_cast<AssetGuid*>(object));
}
nlohmann::json AssetRefToJson(const void* object) {
    const AssetGuid& guid = *static_cast<const AssetGuid*>(object);
    return guid.IsNull() ? std::string() : ToString(guid);
}
bool AssetRefFromJson(const nlohmann::json& data, void* object) {
    if (!data.is_string()) {
        return false;
    }
    AssetGuid& guid = *static_cast<AssetGuid*>(object);
    const std::string& text = data.get_ref<const std::string&>();
    if (text.empty()) {
        guid = {};
        return true;
    }
    return ParseAssetGuid(text, guid);
}
} // namespace detail

// ---------------------------------------------------------------------------
// .ameta files
// ---------------------------------------------------------------------------

namespace {

void SetError(std::string* error, std::string message) {
    if (error != nullptr) {
        *error = std::move(message);
    }
}

std::string ToLower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return std::tolower(c); });
    return text;
}

stdfs::path MetaPathFor(const stdfs::path& source) {
    stdfs::path meta = source;
    meta += AssetMeta::kExtension;
    return meta;
}

bool IsHidden(const stdfs::path& relative) {
    for (const stdfs::path& part : relative) {
        std::string name = part.string();
        if (!name.empty() && name.front() == '.') {
            return true;
        }
    }
    return false;
}

std::string RelativeString(const stdfs::path& root, const stdfs::path& absolute) {
    return stdfs::relative(absolute, root).generic_string();
}

} // namespace

bool SaveAssetMeta(const stdfs::path& meta_file, const AssetMeta& meta, std::string* error) {
    Json json = Json::object();
    json["$type"] = "AssetMeta";
    json["$v"] = 1;
    json["guid"] = ToString(meta.guid);
    json["importer"] = meta.importer;
    json["importer_version"] = meta.importer_version;
    json["source_hash"] = meta.source_hash;
    json["settings"] = meta.settings.is_object() ? meta.settings : Json::object();
    json["labels"] = meta.labels;
    if (!meta.sub_assets.empty()) {
        Json subs = Json::object();
        for (const SubAssetMeta& sub : meta.sub_assets) {
            subs[sub.key] = {{"guid", ToString(sub.guid)}, {"importer", sub.importer}};
        }
        json["sub_assets"] = std::move(subs);
    }
    std::string text = json.dump(2);
    text.push_back('\n');
    if (!fs::WriteFileBytes(meta_file.string(), text.data(), text.size())) {
        SetError(error, "Couldn't write " + meta_file.string());
        return false;
    }
    return true;
}

bool LoadAssetMeta(const stdfs::path& meta_file, AssetMeta& out, std::string* error) {
    std::vector<u8> bytes;
    if (!fs::ReadFileBytes(meta_file.string(), bytes)) {
        SetError(error, "Couldn't read " + meta_file.string());
        return false;
    }
    Json json = Json::parse(bytes.begin(), bytes.end(), nullptr, /*allow_exceptions=*/false);
    if (json.is_discarded() || !json.is_object() || json.value("$type", "") != "AssetMeta") {
        SetError(error, meta_file.string() + " isn't an asset metadata file");
        return false;
    }
    AssetMeta meta;
    if (!json.contains("guid") || !json["guid"].is_string() ||
        !ParseAssetGuid(json["guid"].get_ref<const std::string&>(), meta.guid) || meta.guid.IsNull()) {
        SetError(error, meta_file.string() + " has no valid guid");
        return false;
    }
    meta.importer = json.value("importer", "");
    meta.importer_version = json.value("importer_version", 1u);
    meta.source_hash = json.value("source_hash", "");
    if (auto it = json.find("settings"); it != json.end() && it->is_object()) {
        meta.settings = *it;
    }
    if (auto it = json.find("labels"); it != json.end() && it->is_array()) {
        for (const Json& label : *it) {
            if (label.is_string()) {
                meta.labels.push_back(label.get<std::string>());
            }
        }
    }
    if (auto it = json.find("sub_assets"); it != json.end() && it->is_object()) {
        for (const auto& [key, entry] : it->items()) {
            SubAssetMeta sub;
            sub.key = key;
            if (!entry.is_object() || !entry.contains("guid") || !entry["guid"].is_string() ||
                !ParseAssetGuid(entry["guid"].get_ref<const std::string&>(), sub.guid) || sub.guid.IsNull()) {
                continue; // dropped; the next import gives the key a new GUID
            }
            sub.importer = entry.value("importer", "");
            meta.sub_assets.push_back(std::move(sub));
        }
        std::sort(meta.sub_assets.begin(), meta.sub_assets.end(),
                  [](const SubAssetMeta& a, const SubAssetMeta& b) { return a.key < b.key; });
    }
    out = std::move(meta);
    return true;
}

std::string ImporterForExtension(const stdfs::path& file) {
    static const std::map<std::string, std::string> kImporters = {
        {".aesc", "Scene"},   {".png", "Texture"},  {".jpg", "Texture"}, {".jpeg", "Texture"}, {".tga", "Texture"}, {".bmp", "Texture"},
        {".hdr", "Texture"},  {".gltf", "Model"},  {".glb", "Model"},    {".ascene", "Scene"}, {".aprefab", "Prefab"}, {".aaction", "InputAction"}, {".amapping", "InputMapping"},
        {".abp", "Blueprint"}, {".aatlas", "SpriteAtlas"}, {".atileset", "Tileset"}, {".atilemap", "Tilemap"}, {".asequence", "Sequence"}, {".astrings", "StringTable"}, {".aeffect", "GameplayEffect"}, {".aability", "GameplayAbility"}, {".wav", "Sound"},    {".ogg", "Sound"},   {".flac", "Sound"},   {".mp3", "Sound"},    {".luau", "Script"},
        {".hlsl", "Shader"},  {".ttf", "Font"},    {".otf", "Font"},
    };
    auto it = kImporters.find(ToLower(file.extension().string()));
    return it != kImporters.end() ? it->second : std::string();
}

std::string HashFile(const stdfs::path& file) {
    std::vector<u8> bytes;
    if (!fs::ReadFileBytes(file.string(), bytes)) {
        return {};
    }
    u64 hash = 0xcbf29ce484222325ull;
    for (u8 b : bytes) {
        hash ^= b;
        hash *= 0x100000001b3ull;
    }
    char text[32];
    std::snprintf(text, sizeof(text), "fnv1a64:%016llx", static_cast<unsigned long long>(hash));
    return text;
}

// ---------------------------------------------------------------------------
// AssetDatabase
// ---------------------------------------------------------------------------

AssetDatabase::AssetDatabase(stdfs::path content_root) : root_(std::move(content_root)) {}

ScanResult AssetDatabase::Scan() {
    ScanResult result;
    records_.clear();
    by_path_.clear();

    std::error_code ec;
    if (!stdfs::is_directory(root_, ec)) {
        result.warnings.push_back(root_.string() + " isn't a folder");
        return result;
    }

    // 1. Collect every source and every .ameta (sorted, for determinism).
    std::vector<stdfs::path> sources;
    std::vector<stdfs::path> metas;
    for (auto it = stdfs::recursive_directory_iterator(root_, ec); !ec && it != stdfs::recursive_directory_iterator();
         it.increment(ec)) {
        const stdfs::path relative = stdfs::relative(it->path(), root_);
        if (IsHidden(relative)) {
            if (it->is_directory()) {
                it.disable_recursion_pending();
            }
            continue;
        }
        if (!it->is_regular_file()) {
            continue;
        }
        if (ToLower(it->path().extension().string()) == AssetMeta::kExtension) {
            metas.push_back(it->path());
        } else if (!ImporterForExtension(it->path()).empty()) {
            sources.push_back(it->path());
        }
    }
    std::sort(sources.begin(), sources.end());
    std::sort(metas.begin(), metas.end());

    // 2. Load every .ameta; resolve duplicate GUIDs oldest-first.
    struct LoadedMeta {
        stdfs::path meta_file;
        stdfs::path source;
        AssetMeta meta;
        stdfs::file_time_type written;
    };
    std::vector<LoadedMeta> loaded;
    std::unordered_map<std::string, bool> unreadable; // sources whose .ameta couldn't be read
    for (const stdfs::path& meta_file : metas) {
        LoadedMeta entry;
        std::string error;
        if (!LoadAssetMeta(meta_file, entry.meta, &error)) {
            // Never overwrite it: a common cause is a version-control merge
            // conflict, and replacing the file would lose the asset's GUID
            // (breaking every reference to it). Skip the asset until fixed.
            stdfs::path source = meta_file;
            source.replace_extension();
            unreadable[RelativeString(root_, source)] = true;
            result.warnings.push_back(error + "; the asset is skipped until its .ameta is fixed");
            continue;
        }
        entry.meta_file = meta_file;
        entry.source = meta_file;
        entry.source.replace_extension(); // strip ".ameta"
        entry.written = stdfs::last_write_time(meta_file, ec);
        loaded.push_back(std::move(entry));
    }
    std::stable_sort(loaded.begin(), loaded.end(),
                     [](const LoadedMeta& a, const LoadedMeta& b) { return a.written < b.written; });

    std::unordered_map<std::string, bool> has_meta;
    for (LoadedMeta& entry : loaded) {
        bool changed = false;
        if (records_.count(entry.meta.guid) != 0) {
            AssetGuid old = entry.meta.guid;
            entry.meta.guid = NewAssetGuid();
            changed = true;
            result.warnings.push_back(RelativeString(root_, entry.source) + " had the same GUID (" + ToString(old) +
                                      ") as another asset, e.g. copied with its .ameta; gave it a new one");
            ++result.duplicates_fixed;
        }
        // Sub-asset GUIDs are copied along with the .ameta too.
        for (SubAssetMeta& sub : entry.meta.sub_assets) {
            if (records_.count(sub.guid) != 0 || sub.guid == entry.meta.guid) {
                sub.guid = NewAssetGuid();
                changed = true;
                ++result.duplicates_fixed;
            }
        }
        if (changed) {
            SaveAssetMeta(entry.meta_file, entry.meta);
        }
        AssetRecord record;
        record.guid = entry.meta.guid;
        record.path = RelativeString(root_, entry.source);
        record.importer = entry.meta.importer.empty() ? ImporterForExtension(entry.source) : entry.meta.importer;
        record.missing = !stdfs::exists(entry.source, ec);
        if (record.missing) {
            ++result.missing;
        } else {
            record.source_hash = HashFile(entry.source);
            record.needs_import = record.source_hash != entry.meta.source_hash;
        }
        for (const SubAssetMeta& sub : entry.meta.sub_assets) {
            AssetRecord sub_record;
            sub_record.guid = sub.guid;
            sub_record.path = record.path + "#" + sub.key;
            sub_record.importer = sub.importer;
            sub_record.source_hash = record.source_hash;
            sub_record.missing = record.missing;
            sub_record.parent = record.guid;
            sub_record.sub_key = sub.key;
            record.sub_assets.push_back(sub.guid);
            by_path_[sub_record.path] = sub.guid;
            records_[sub.guid] = std::move(sub_record);
        }
        has_meta[record.path] = true;
        by_path_[record.path] = record.guid;
        records_[record.guid] = std::move(record);
    }

    // 3. Sources without an .ameta get one.
    for (const stdfs::path& source : sources) {
        const std::string relative = RelativeString(root_, source);
        if (has_meta.count(relative) != 0 || unreadable.count(relative) != 0) {
            continue;
        }
        AssetMeta meta;
        meta.guid = NewAssetGuid();
        meta.importer = ImporterForExtension(source);
        std::string error;
        if (!SaveAssetMeta(MetaPathFor(source), meta, &error)) {
            result.warnings.push_back(error);
            continue;
        }
        AssetRecord record;
        record.guid = meta.guid;
        record.path = relative;
        record.importer = meta.importer;
        record.source_hash = HashFile(source);
        record.needs_import = true;
        by_path_[relative] = record.guid;
        records_[record.guid] = std::move(record);
        ++result.created_meta;
    }

    RebuildDependencies();

    result.assets = records_.size();
    for (const auto& [guid, record] : records_) {
        result.needs_import += record.needs_import ? 1 : 0;
    }
    for (const std::string& warning : result.warnings) {
        AETHER_LOG_WARN("Assets", "%s", warning.c_str());
    }
    return result;
}

namespace {

bool IsHexDigit(u8 c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

// Every substring shaped like a UUID (8-4-4-4-12 hex digits) in `bytes`. JSON
// and MessagePack both store strings as raw UTF-8, so this finds saved
// AssetRef GUIDs in either scene format without parsing it.
std::vector<AssetGuid> FindGuidStrings(const std::vector<u8>& bytes) {
    std::vector<AssetGuid> found;
    constexpr usize kLength = 36;
    for (usize i = 0; i + kLength <= bytes.size(); ++i) {
        bool shaped = true;
        for (usize k = 0; k < kLength && shaped; ++k) {
            const bool dash = k == 8 || k == 13 || k == 18 || k == 23;
            shaped = dash ? bytes[i + k] == '-' : IsHexDigit(bytes[i + k]);
        }
        if (!shaped) {
            continue;
        }
        AssetGuid guid;
        if (ParseAssetGuid(std::string_view(reinterpret_cast<const char*>(bytes.data() + i), kLength), guid)) {
            found.push_back(guid);
            i += kLength - 1;
        }
    }
    return found;
}

} // namespace

void AssetDatabase::RebuildDependencies() {
    referencers_.clear();
    for (auto& [guid, record] : records_) {
        record.dependencies.clear();
        if (record.missing || record.IsSubAsset()) {
            continue;
        }
        if (record.importer == "Model") {
            // A .gltf names its textures by relative path, not GUID.
            for (const GltfFileReference& ref : GltfFileReferences(root_, record.path)) {
                auto it = by_path_.find(ref.path);
                if (!ref.path.empty() && it != by_path_.end() && it->second != guid) {
                    record.dependencies.push_back(it->second);
                }
            }
        } else if (record.importer != "Scene" && record.importer != "Prefab" && record.importer != "SpriteAtlas" &&
                   record.importer != "Tileset" && record.importer != "Tilemap" && record.importer != "Sequence") {
            continue;
        }
        std::vector<u8> bytes;
        if (record.importer != "Model" && !fs::ReadFileBytes(Absolute(record.path).string(), bytes)) {
            continue;
        }
        for (const AssetGuid& referenced : FindGuidStrings(bytes)) {
            // Entity GUIDs share the format; only known asset GUIDs count.
            if (referenced != guid && records_.count(referenced) != 0) {
                record.dependencies.push_back(referenced);
            }
        }
        std::sort(record.dependencies.begin(), record.dependencies.end());
        record.dependencies.erase(std::unique(record.dependencies.begin(), record.dependencies.end()),
                                  record.dependencies.end());
        for (const AssetGuid& dependency : record.dependencies) {
            referencers_[dependency].push_back(guid);
        }
    }
}

std::vector<const AssetRecord*> AssetDatabase::Referencers(const AssetGuid& guid) const {
    std::vector<const AssetRecord*> result;
    if (auto it = referencers_.find(guid); it != referencers_.end()) {
        for (const AssetGuid& referencer : it->second) {
            if (const AssetRecord* record = Find(referencer)) {
                result.push_back(record);
            }
        }
    }
    std::sort(result.begin(), result.end(), [](const AssetRecord* a, const AssetRecord* b) { return a->path < b->path; });
    return result;
}

bool AssetDatabase::Delete(const AssetGuid& guid_ref, bool force, std::string* error) {
    // A copy: callers commonly pass a reference into the very record this
    // erases (db.Delete(db.FindByPath(p)->guid, ...)).
    const AssetGuid guid = guid_ref;
    auto it = records_.find(guid);
    if (it == records_.end()) {
        SetError(error, "No asset with GUID " + ToString(guid));
        return false;
    }
    if (it->second.IsSubAsset()) {
        SetError(error, it->second.path + " is part of another asset; delete its source instead");
        return false;
    }
    // Users of the asset or any of its sub-assets, from outside it.
    std::vector<const AssetRecord*> users = Referencers(guid);
    for (const AssetGuid& sub : it->second.sub_assets) {
        for (const AssetRecord* user : Referencers(sub)) {
            if (std::find(users.begin(), users.end(), user) == users.end()) {
                users.push_back(user);
            }
        }
    }
    std::sort(users.begin(), users.end(), [](const AssetRecord* a, const AssetRecord* b) { return a->path < b->path; });
    if (!users.empty() && !force) {
        std::string message = it->second.path + " is used by " + std::to_string(users.size()) + " asset(s):";
        for (const AssetRecord* user : users) {
            message += " " + user->path;
        }
        SetError(error, message);
        return false;
    }
    const stdfs::path source = Absolute(it->second.path);
    std::error_code ec;
    stdfs::remove(source, ec);
    stdfs::remove(MetaPathFor(source), ec);
    const std::vector<AssetGuid> subs = it->second.sub_assets;
    for (const AssetGuid& sub : subs) {
        EraseRecord(sub);
    }
    EraseRecord(guid);
    return true;
}

void AssetDatabase::EraseRecord(const AssetGuid& guid_ref) {
    const AssetGuid guid = guid_ref;
    auto it = records_.find(guid);
    if (it == records_.end()) {
        return;
    }
    by_path_.erase(it->second.path);
    records_.erase(it);
    referencers_.erase(guid);
    for (auto& [other, list] : referencers_) {
        list.erase(std::remove(list.begin(), list.end(), guid), list.end());
    }
    for (auto& [other, record] : records_) {
        record.dependencies.erase(std::remove(record.dependencies.begin(), record.dependencies.end(), guid),
                                  record.dependencies.end());
    }
}

const AssetRecord* AssetDatabase::Find(const AssetGuid& guid) const {
    auto it = records_.find(guid);
    return it != records_.end() ? &it->second : nullptr;
}

const AssetRecord* AssetDatabase::FindByPath(const std::string& path) const {
    auto it = by_path_.find(path);
    return it != by_path_.end() ? Find(it->second) : nullptr;
}

std::vector<const AssetRecord*> AssetDatabase::All() const {
    std::vector<const AssetRecord*> all;
    all.reserve(records_.size());
    for (const auto& [guid, record] : records_) {
        all.push_back(&record);
    }
    std::sort(all.begin(), all.end(), [](const AssetRecord* a, const AssetRecord* b) { return a->path < b->path; });
    return all;
}

bool AssetDatabase::Move(const AssetGuid& guid_ref, const std::string& new_path, std::string* error) {
    const AssetGuid guid = guid_ref; // may refer into the record being updated
    auto it = records_.find(guid);
    if (it == records_.end()) {
        SetError(error, "No asset with GUID " + ToString(guid));
        return false;
    }
    AssetRecord& record = it->second;
    if (record.IsSubAsset()) {
        SetError(error, record.path + " is part of another asset; move its source instead");
        return false;
    }
    if (new_path.find('#') != std::string::npos) {
        SetError(error, "Asset paths can't contain '#'");
        return false;
    }
    const stdfs::path from = Absolute(record.path);
    const stdfs::path to = Absolute(new_path);
    std::error_code ec;
    if (stdfs::exists(to, ec) || stdfs::exists(MetaPathFor(to), ec)) {
        SetError(error, new_path + " already exists");
        return false;
    }
    stdfs::create_directories(to.parent_path(), ec);
    if (!record.missing) {
        stdfs::rename(from, to, ec);
        if (ec) {
            SetError(error, "Couldn't move " + record.path + ": " + ec.message());
            return false;
        }
    }
    stdfs::rename(MetaPathFor(from), MetaPathFor(to), ec);
    if (ec) {
        SetError(error, "Couldn't move the .ameta of " + record.path + ": " + ec.message());
        if (!record.missing) {
            std::error_code undo_ec;
            stdfs::rename(to, from, undo_ec); // put the source back so they stay paired
        }
        return false;
    }
    by_path_.erase(record.path);
    record.path = stdfs::path(new_path).generic_string();
    by_path_[record.path] = guid;
    for (const AssetGuid& sub_guid : record.sub_assets) {
        AssetRecord& sub = records_[sub_guid];
        by_path_.erase(sub.path);
        sub.path = record.path + "#" + sub.sub_key;
        by_path_[sub.path] = sub_guid;
    }
    return true;
}

stdfs::path AssetDatabase::SourcePath(const AssetGuid& guid) const {
    const AssetRecord* record = Find(guid);
    if (record != nullptr && record->IsSubAsset()) {
        record = Find(record->parent);
    }
    return record != nullptr ? Absolute(record->path) : stdfs::path();
}

stdfs::path AssetDatabase::MetaPath(const AssetGuid& guid) const {
    const stdfs::path source = SourcePath(guid);
    return source.empty() ? source : MetaPathFor(source);
}

std::vector<AssetGuid> AssetDatabase::SetSubAssets(const AssetGuid& source_ref,
                                                   const std::vector<SubAssetMeta>& sub_assets, std::string* error) {
    const AssetGuid source = source_ref;
    auto it = records_.find(source);
    if (it == records_.end() || it->second.IsSubAsset()) {
        SetError(error, "No source asset with GUID " + ToString(source));
        return {};
    }
    for (usize i = 0; i < sub_assets.size(); ++i) {
        const std::string& key = sub_assets[i].key;
        if (key.empty()) {
            SetError(error, "Sub-asset keys can't be empty");
            return {};
        }
        for (usize k = 0; k < i; ++k) {
            if (sub_assets[k].key == key) {
                SetError(error, "Duplicate sub-asset key \"" + key + "\"");
                return {};
            }
        }
    }
    const stdfs::path meta_file = MetaPath(source);
    AssetMeta meta;
    if (!LoadAssetMeta(meta_file, meta, error)) {
        return {};
    }

    std::unordered_map<std::string, AssetGuid> existing;
    for (const SubAssetMeta& sub : meta.sub_assets) {
        existing[sub.key] = sub.guid;
    }
    std::vector<AssetGuid> guids;
    std::vector<SubAssetMeta> updated;
    for (const SubAssetMeta& wanted : sub_assets) {
        SubAssetMeta sub = wanted;
        auto found = existing.find(sub.key);
        sub.guid = found != existing.end() ? found->second : NewAssetGuid();
        guids.push_back(sub.guid);
        updated.push_back(std::move(sub));
    }
    std::sort(updated.begin(), updated.end(), [](const SubAssetMeta& a, const SubAssetMeta& b) { return a.key < b.key; });
    meta.sub_assets = updated;
    if (!SaveAssetMeta(meta_file, meta, error)) {
        return {};
    }

    // Update the records: drop keys no longer produced, add or refresh the rest.
    const std::vector<AssetGuid> old_subs = it->second.sub_assets;
    for (const AssetGuid& old : old_subs) {
        if (std::find(guids.begin(), guids.end(), old) == guids.end()) {
            EraseRecord(old);
        }
    }
    AssetRecord& record = records_[source];
    record.sub_assets.clear();
    for (const SubAssetMeta& sub : updated) {
        AssetRecord sub_record;
        sub_record.guid = sub.guid;
        sub_record.path = record.path + "#" + sub.key;
        sub_record.importer = sub.importer;
        sub_record.source_hash = record.source_hash;
        sub_record.missing = record.missing;
        sub_record.parent = source;
        sub_record.sub_key = sub.key;
        record.sub_assets.push_back(sub.guid);
        by_path_[sub_record.path] = sub.guid;
        records_[sub.guid] = std::move(sub_record);
    }
    return guids;
}

bool AssetDatabase::MarkImported(const AssetGuid& guid, std::string* error, u32 importer_version) {
    auto it = records_.find(guid);
    if (it == records_.end() || it->second.missing) {
        SetError(error, "No asset source for GUID " + ToString(guid));
        return false;
    }
    AssetRecord& record = it->second;
    if (record.IsSubAsset()) {
        SetError(error, record.path + " is imported with its source");
        return false;
    }
    const stdfs::path meta_file = MetaPathFor(Absolute(record.path));
    AssetMeta meta;
    if (!LoadAssetMeta(meta_file, meta, error)) {
        return false;
    }
    record.source_hash = HashFile(Absolute(record.path));
    meta.source_hash = record.source_hash;
    if (importer_version != 0) {
        meta.importer_version = importer_version;
    }
    if (!SaveAssetMeta(meta_file, meta, error)) {
        return false;
    }
    record.needs_import = false;
    for (const AssetGuid& sub : record.sub_assets) {
        records_[sub].source_hash = record.source_hash;
    }
    return true;
}

} // namespace aether::assets
