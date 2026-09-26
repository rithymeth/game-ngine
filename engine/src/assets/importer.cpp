#include "aether/assets/importer.h"

#include "aether/assets/model_importer.h"
#include "aether/assets/texture_importer.h"

#include <cstring>

namespace aether::assets {

namespace {

// DDC entries hold the main data and every sub-asset ("AIMP" v1):
//   "AIMP" u32 version, u32 size + main bytes, u32 sub-asset count,
//   then per sub-asset: u32 size + key, u32 size + importer, u32 size + data.
constexpr char kCacheMagic[4] = {'A', 'I', 'M', 'P'};
constexpr u32 kCacheVersion = 1;

void AppendU32(std::vector<u8>& out, u32 value) {
    const u8* b = reinterpret_cast<const u8*>(&value);
    out.insert(out.end(), b, b + 4);
}

template <typename Bytes>
void AppendBlock(std::vector<u8>& out, const Bytes& bytes) {
    AppendU32(out, static_cast<u32>(bytes.size()));
    out.insert(out.end(), bytes.begin(), bytes.end());
}

bool ReadU32(const std::vector<u8>& in, usize& offset, u32& value) {
    if (in.size() - offset < 4) {
        return false;
    }
    std::memcpy(&value, in.data() + offset, 4);
    offset += 4;
    return true;
}

template <typename Bytes>
bool ReadBlock(const std::vector<u8>& in, usize& offset, Bytes& out) {
    u32 size = 0;
    if (!ReadU32(in, offset, size) || in.size() - offset < size) {
        return false;
    }
    out.assign(in.begin() + static_cast<std::ptrdiff_t>(offset),
               in.begin() + static_cast<std::ptrdiff_t>(offset + size));
    offset += size;
    return true;
}

std::vector<u8> EncodeCacheEntry(const std::vector<u8>& data, const std::vector<SubAssetOutput>& sub_assets) {
    std::vector<u8> out(kCacheMagic, kCacheMagic + 4);
    AppendU32(out, kCacheVersion);
    AppendBlock(out, data);
    AppendU32(out, static_cast<u32>(sub_assets.size()));
    for (const SubAssetOutput& sub : sub_assets) {
        AppendBlock(out, sub.key);
        AppendBlock(out, sub.importer);
        AppendBlock(out, sub.data);
    }
    return out;
}

bool DecodeCacheEntry(const std::vector<u8>& in, std::vector<u8>& data, std::vector<SubAssetOutput>& sub_assets) {
    usize offset = 4;
    u32 version = 0;
    u32 count = 0;
    if (in.size() < 4 || std::memcmp(in.data(), kCacheMagic, 4) != 0 || !ReadU32(in, offset, version) ||
        version != kCacheVersion || !ReadBlock(in, offset, data) || !ReadU32(in, offset, count)) {
        return false;
    }
    sub_assets.clear();
    for (u32 i = 0; i < count; ++i) {
        SubAssetOutput sub;
        if (!ReadBlock(in, offset, sub.key) || !ReadBlock(in, offset, sub.importer) || !ReadBlock(in, offset, sub.data)) {
            return false;
        }
        sub_assets.push_back(std::move(sub));
    }
    return offset == in.size();
}

} // namespace

ImporterRegistry ImporterRegistry::WithBuiltins() {
    ImporterRegistry registry;
    registry.Register(std::make_unique<TextureImporter>());
    registry.Register(std::make_unique<ModelImporter>());
    return registry;
}

void ImporterRegistry::Register(std::unique_ptr<IAssetImporter> importer) {
    for (auto& existing : importers_) {
        if (std::string(existing->Name()) == importer->Name()) {
            existing = std::move(importer);
            return;
        }
    }
    importers_.push_back(std::move(importer));
}

const IAssetImporter* ImporterRegistry::Find(const std::string& name) const {
    for (const auto& importer : importers_) {
        if (name == importer->Name()) {
            return importer.get();
        }
    }
    return nullptr;
}

ImportOutput ImportAsset(AssetDatabase& database, const AssetGuid& guid, const ImporterRegistry& importers,
                         DerivedDataCache& cache, const std::string& platform) {
    ImportOutput output;
    const AssetRecord* record = database.Find(guid);
    if (record == nullptr || record->missing) {
        output.error = "No asset source for " + ToString(guid);
        return output;
    }
    if (record->IsSubAsset()) {
        // Import the source; hand back just this piece.
        const std::string key = record->sub_key;
        const std::string path = record->path;
        ImportOutput source = ImportAsset(database, record->parent, importers, cache, platform);
        if (!source.ok) {
            return source;
        }
        for (SubAssetOutput& sub : source.sub_assets) {
            if (sub.key == key) {
                source.data = std::move(sub.data);
                source.sub_assets.clear();
                return source;
            }
        }
        output.error = path + " is no longer produced by its source";
        return output;
    }
    const IAssetImporter* importer = importers.Find(record->importer);
    if (importer == nullptr) {
        output.error = "No importer for \"" + record->importer + "\" assets (" + record->path + ")";
        return output;
    }
    AssetMeta meta;
    if (!LoadAssetMeta(database.MetaPath(guid), meta, &output.error)) {
        return output;
    }

    nlohmann::json settings = importer->DefaultSettings();
    settings.merge_patch(meta.settings); // the .ameta's choices win
    const std::string key =
        DerivedDataCache::MakeKey(importer->Name(), importer->Version(), record->source_hash, settings.dump(), platform);

    std::optional<std::vector<u8>> cached = cache.Get(key);
    if (cached && DecodeCacheEntry(*cached, output.data, output.sub_assets)) {
        output.ok = true;
        output.from_cache = true;
    } else { // a miss, or an entry in an older layout: import again
        ImportContext context{*record, database.SourcePath(guid), settings};
        ImportResult result = importer->Import(context);
        output.warnings = std::move(result.warnings);
        if (!result.ok) {
            output.error = record->path + ": " + result.error;
            return output;
        }
        if (!cache.Put(key, EncodeCacheEntry(result.data, result.sub_assets))) {
            output.warnings.push_back("Couldn't write the derived data cache entry for " + record->path);
        }
        output.ok = true;
        output.data = std::move(result.data);
        output.sub_assets = std::move(result.sub_assets);
    }
    std::string error;
    // Register the sub-assets (even an empty list: removes ones no longer made).
    std::vector<SubAssetMeta> subs;
    for (const SubAssetOutput& sub : output.sub_assets) {
        subs.push_back({sub.key, AssetGuid{}, sub.importer});
    }
    if (!subs.empty() || !record->sub_assets.empty()) {
        std::vector<AssetGuid> guids = database.SetSubAssets(guid, subs, &error);
        if (guids.size() == output.sub_assets.size()) {
            for (usize i = 0; i < guids.size(); ++i) {
                output.sub_assets[i].guid = guids[i];
            }
        } else {
            output.warnings.push_back(error);
            error.clear();
        }
    }
    if (!database.MarkImported(guid, &error, importer->Version())) {
        output.warnings.push_back(error);
    }
    return output;
}

ImportAllResult ImportAll(AssetDatabase& database, const ImporterRegistry& importers, DerivedDataCache& cache,
                          const std::string& platform) {
    ImportAllResult summary;
    std::vector<AssetGuid> pending;
    for (const AssetRecord* record : database.All()) {
        if (record->needs_import && !record->missing) {
            pending.push_back(record->guid);
        }
    }
    for (const AssetGuid& guid : pending) {
        const AssetRecord* record = database.Find(guid);
        if (importers.Find(record->importer) == nullptr) {
            ++summary.skipped;
            continue;
        }
        ImportOutput output = ImportAsset(database, guid, importers, cache, platform);
        if (!output.ok) {
            ++summary.failed;
            summary.errors.push_back(output.error);
        } else if (output.from_cache) {
            ++summary.from_cache;
        } else {
            ++summary.imported;
        }
    }
    return summary;
}

} // namespace aether::assets
