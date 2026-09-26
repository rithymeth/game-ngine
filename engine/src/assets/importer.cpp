#include "aether/assets/importer.h"

#include "aether/assets/texture_importer.h"

namespace aether::assets {

ImporterRegistry ImporterRegistry::WithBuiltins() {
    ImporterRegistry registry;
    registry.Register(std::make_unique<TextureImporter>());
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

    if (std::optional<std::vector<u8>> cached = cache.Get(key)) {
        output.ok = true;
        output.from_cache = true;
        output.data = std::move(*cached);
    } else {
        ImportContext context{*record, database.SourcePath(guid), settings};
        ImportResult result = importer->Import(context);
        output.warnings = std::move(result.warnings);
        if (!result.ok) {
            output.error = record->path + ": " + result.error;
            return output;
        }
        if (!cache.Put(key, result.data)) {
            output.warnings.push_back("Couldn't write the derived data cache entry for " + record->path);
        }
        output.ok = true;
        output.data = std::move(result.data);
    }
    std::string error;
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
