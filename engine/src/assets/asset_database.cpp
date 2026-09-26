#include "aether/assets/asset_database.h"
#include "aether/assets/asset_ref.h"

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
    out = std::move(meta);
    return true;
}

std::string ImporterForExtension(const stdfs::path& file) {
    static const std::map<std::string, std::string> kImporters = {
        {".aesc", "Scene"},   {".png", "Texture"},  {".jpg", "Texture"}, {".jpeg", "Texture"}, {".tga", "Texture"}, {".bmp", "Texture"},
        {".hdr", "Texture"},  {".gltf", "Model"},  {".glb", "Model"},    {".ascene", "Scene"}, {".aprefab", "Prefab"},
        {".wav", "Sound"},    {".ogg", "Sound"},   {".flac", "Sound"},   {".mp3", "Sound"},    {".luau", "Script"},
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
        if (records_.count(entry.meta.guid) != 0) {
            AssetGuid old = entry.meta.guid;
            entry.meta.guid = NewAssetGuid();
            SaveAssetMeta(entry.meta_file, entry.meta);
            result.warnings.push_back(RelativeString(root_, entry.source) + " had the same GUID (" + ToString(old) +
                                      ") as another asset, e.g. copied with its .ameta; gave it a new one");
            ++result.duplicates_fixed;
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
        if (record.missing || (record.importer != "Scene" && record.importer != "Prefab")) {
            continue;
        }
        std::vector<u8> bytes;
        if (!fs::ReadFileBytes(Absolute(record.path).string(), bytes)) {
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
    std::vector<const AssetRecord*> users = Referencers(guid);
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
    by_path_.erase(it->second.path);
    records_.erase(it);
    referencers_.erase(guid);
    for (auto& [other, list] : referencers_) {
        list.erase(std::remove(list.begin(), list.end(), guid), list.end());
    }
    return true;
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
    return true;
}

stdfs::path AssetDatabase::SourcePath(const AssetGuid& guid) const {
    const AssetRecord* record = Find(guid);
    return record != nullptr ? Absolute(record->path) : stdfs::path();
}

stdfs::path AssetDatabase::MetaPath(const AssetGuid& guid) const {
    const AssetRecord* record = Find(guid);
    return record != nullptr ? MetaPathFor(Absolute(record->path)) : stdfs::path();
}

bool AssetDatabase::MarkImported(const AssetGuid& guid, std::string* error, u32 importer_version) {
    auto it = records_.find(guid);
    if (it == records_.end() || it->second.missing) {
        SetError(error, "No asset source for GUID " + ToString(guid));
        return false;
    }
    AssetRecord& record = it->second;
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
    return true;
}

} // namespace aether::assets
