#pragma once

#include "aether/core/base.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace aether::assets {

// The derived data cache (DDC, docs/design/PHASE_SPECS.md §8.1): processed
// import results stored under Intermediate/DDC/, keyed by everything that
// determines them — so reopening a project, switching branches back and
// forth, or undoing a settings change never re-imports what was already
// produced once. Entries are immutable files: a key never changes meaning.
class DerivedDataCache {
public:
    explicit DerivedDataCache(std::filesystem::path directory);

    // The key for one import: importer name and version, source hash, the
    // importer settings actually used, and the target platform.
    static std::string MakeKey(std::string_view importer, u32 importer_version, std::string_view source_hash,
                               std::string_view settings_json, std::string_view platform);

    std::optional<std::vector<u8>> Get(const std::string& key);
    // Atomic: written to a temporary file, then renamed into place, so a crash
    // mid-write never leaves a truncated entry behind.
    bool Put(const std::string& key, const std::vector<u8>& data);

    u64 Hits() const { return hits_; }
    u64 Misses() const { return misses_; }
    const std::filesystem::path& Directory() const { return dir_; }

private:
    std::filesystem::path PathFor(const std::string& key) const;

    std::filesystem::path dir_;
    u64 hits_ = 0;
    u64 misses_ = 0;
};

} // namespace aether::assets
