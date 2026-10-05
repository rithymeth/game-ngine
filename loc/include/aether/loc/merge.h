#pragma once

#include "aether/core/base.h"
#include "aether/loc/string_table.h"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace aether::loc {

struct MergeResult {
    usize added = 0;                 // keys that had no source-language text yet
    usize updated_source = 0;        // source-language texts that changed
    std::vector<std::string> unused; // keys in the table that weren't gathered (never removed)
};

// Brings a table up to date with the keys found in the project (§29.4): a new
// key gets its source text in `source_language`; a key whose source-language
// text changed is updated (the other languages are left for translators to
// revisit); no other translation is ever touched, and no key is ever removed:
// keys nothing uses any more are only reported.
MergeResult MergeKeys(StringTable& table, std::string_view source_language, const std::vector<std::pair<std::string, std::string>>& gathered);

} // namespace aether::loc
