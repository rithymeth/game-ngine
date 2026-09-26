#pragma once

#include "aether/assets/asset_database.h"
#include "aether/assets/derived_data_cache.h"

#include <nlohmann/json.hpp>

#include <memory>
#include <string>
#include <vector>

namespace aether::assets {

struct ImportContext {
    const AssetRecord& record;
    std::filesystem::path source;  // absolute path of the source file
    const nlohmann::json& settings; // importer defaults, overridden by the .ameta's settings
};

struct ImportResult {
    bool ok = false;
    std::vector<u8> data; // the processed, engine-ready form of the asset
    std::string error;
    std::vector<std::string> warnings;
};

// Turns one kind of source file into engine-ready data (Phase 8,
// docs/design/PHASE_SPECS.md §8.1). Importers are pure functions of (source
// bytes, settings): that's what makes their output cacheable. Bump Version()
// whenever the output for the same input changes, so cached results from the
// old version aren't reused.
class IAssetImporter {
public:
    virtual ~IAssetImporter() = default;
    virtual const char* Name() const = 0; // matches AssetRecord::importer: "Texture", ...
    virtual u32 Version() const = 0;
    virtual nlohmann::json DefaultSettings() const { return nlohmann::json::object(); }
    virtual ImportResult Import(const ImportContext& context) const = 0;
};

class ImporterRegistry {
public:
    // With the built-in importers registered (currently: Texture).
    static ImporterRegistry WithBuiltins();

    void Register(std::unique_ptr<IAssetImporter> importer); // replaces one with the same name
    const IAssetImporter* Find(const std::string& name) const;

private:
    std::vector<std::unique_ptr<IAssetImporter>> importers_;
};

struct ImportOutput {
    bool ok = false;
    bool from_cache = false;
    std::vector<u8> data;
    std::string error;
    std::vector<std::string> warnings;
};

// Imports one asset: settings = importer defaults merged with the .ameta's
// (the .ameta wins), then the DDC is checked for exactly that input before
// running the importer. On success the .ameta records the imported source
// hash and importer version (AssetDatabase::MarkImported).
ImportOutput ImportAsset(AssetDatabase& database, const AssetGuid& guid, const ImporterRegistry& importers,
                         DerivedDataCache& cache, const std::string& platform = "default");

struct ImportAllResult {
    usize imported = 0;   // ran the importer
    usize from_cache = 0; // served by the DDC
    usize failed = 0;
    usize skipped = 0;    // no importer for its type (yet)
    std::vector<std::string> errors;
};

// Imports every asset marked needs_import.
ImportAllResult ImportAll(AssetDatabase& database, const ImporterRegistry& importers, DerivedDataCache& cache,
                          const std::string& platform = "default");

} // namespace aether::assets
