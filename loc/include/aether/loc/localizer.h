#pragma once

#include "aether/loc/loc_text.h"
#include "aether/loc/message_format.h"
#include "aether/loc/string_table.h"

#include <string>
#include <vector>

namespace aether::loc {

// Turns a LocText into the text to show (Phase 29, §29.1). Not a global:
// whoever owns the table and the language owns a Localizer (a later step
// makes the player's one).
class Localizer {
public:
    // The table is borrowed and must outlive the localizer (or be reset).
    void SetTable(const StringTable* table) { table_ = table; }
    // The language to show, and the one to fall back to last (default "en").
    void SetLanguage(std::string language);
    void SetDefaultLanguage(std::string language);
    const std::string& Language() const { return language_; }
    const std::string& DefaultLanguage() const { return default_language_; }
    // The languages tried, most specific first.
    std::vector<std::string> Chain() const;

    // Order: a table entry along the fallback chain, then `text.source`, then
    // the key; so a missing string is never blank. A literal (no key) shows its
    // source as it is. With `args`, the result is run through Format.
    std::string Resolve(const LocText& text, const FormatArgs* args = nullptr, std::vector<std::string>* errors = nullptr) const;
    // By key alone (the key itself if nothing has it).
    std::string Resolve(const std::string& key, const FormatArgs* args = nullptr, std::vector<std::string>* errors = nullptr) const {
        return Resolve(LocText{key, {}}, args, errors);
    }
    // Whether a table (not the source text) has the key in the language or one it falls back to.
    bool IsTranslated(const LocText& text) const;

private:
    const std::string* Lookup(const std::string& key) const;

    const StringTable* table_ = nullptr;
    std::string language_ = "en";
    std::string default_language_ = "en";
};

} // namespace aether::loc
