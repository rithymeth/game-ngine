#include "aether/loc/merge.h"

#include <set>

namespace aether::loc {

MergeResult MergeKeys(StringTable& table, std::string_view source_language, const std::vector<std::pair<std::string, std::string>>& gathered) {
    MergeResult result;
    std::set<std::string> seen;
    for (const auto& [key, source] : gathered) {
        if (key.empty()) continue;
        seen.insert(key);
        const std::string* current = table.Find(source_language, key);
        if (!table.HasKey(key)) {
            table.Set(source_language, key, source.empty() ? key : source);
            ++result.added;
        } else if (current == nullptr) {
            table.Set(source_language, key, source.empty() ? key : source); // translations exist but not the source language's
            ++result.added;
        } else if (!source.empty() && *current != source) {
            table.Set(source_language, key, source);
            ++result.updated_source;
        }
    }
    for (const std::string& key : table.Keys()) {
        if (!seen.count(key)) result.unused.push_back(key);
    }
    return result;
}

} // namespace aether::loc
