#pragma once

#include "aether/core/base.h"

#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace aether::loc {

// Translations by key and language (Phase 29, §29.1). Languages are
// normalized tags ("pt-BR"); a missing entry is simply absent, never "".
class StringTable {
public:
    void Set(std::string_view language, const std::string& key, std::string text);
    // The text, or null if this language has no entry for the key.
    const std::string* Find(std::string_view language, const std::string& key) const;
    bool Remove(std::string_view language, const std::string& key);
    // Makes `language` a column even with no entries yet (so an editor can add one
    // and export it); returns false for an empty tag. Remove drops a language that
    // has no entries left, as before.
    bool AddLanguage(std::string_view language);
    bool HasKey(const std::string& key) const { return entries_.count(key) != 0; }

    std::vector<std::string> Languages() const { return {languages_.begin(), languages_.end()}; }
    std::vector<std::string> Keys() const; // sorted
    usize KeyCount() const { return entries_.size(); }
    bool Empty() const { return entries_.empty(); }
    void Clear();
    // Entries in `language` (for completion: Count / KeyCount).
    usize Count(std::string_view language) const;

    // CSV, one row per key: `key,en,pt-BR,...` (RFC 4180 quoting, so cells
    // may hold commas, quotes and line breaks; a UTF-8 BOM and CRLF are
    // fine). A column named "comment" is ignored; an empty cell means no
    // translation. Merges into the table (an existing key and language is
    // replaced). Returns false (and `errors`) if the header isn't there or a
    // quote never closes; a bad row is skipped with an error and the rest
    // still imports.
    bool ImportCsv(std::string_view csv, std::vector<std::string>* errors = nullptr);
    // Every key and language, keys sorted and languages sorted, so exports
    // diff cleanly.
    std::string ExportCsv() const;

private:
    std::map<std::string, std::map<std::string, std::string>> entries_; // key -> language -> text
    std::set<std::string> languages_;
};

} // namespace aether::loc
