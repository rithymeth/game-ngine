#pragma once

#include "aether/loc/localizer.h"
#include "aether/loc/string_table.h"

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace aether::loc {

// The running game's text (Phase 29 step 2, §29.2): every string table
// loaded so far merged into one, the current language, and who to tell when it
// changes. The player owns one and makes it the active one, which the
// Blueprint and Luau libraries use.
class Localization {
public:
    Localization() { localizer_.SetTable(&table_); }
    ~Localization();
    Localization(const Localization&) = delete;
    Localization& operator=(const Localization&) = delete;

    // Merges a `.astrings` file (CSV, see StringTable::ImportCsv): a later
    // file's entries replace an earlier one's for the same key and language.
    bool AddFromCsv(std::string_view csv, std::vector<std::string>* errors = nullptr) { return table_.ImportCsv(csv, errors); }
    void Clear() { table_.Clear(); }
    const StringTable& Table() const { return table_; }
    StringTable& Table() { return table_; }

    // Sets the language (normalized; an empty one means the default language).
    // Listeners are told only if it changed. Returns whether it did.
    bool SetLanguage(std::string language);
    const std::string& Language() const { return localizer_.Language(); }
    void SetDefaultLanguage(std::string language) { localizer_.SetDefaultLanguage(std::move(language)); }
    const Localizer& GetLocalizer() const { return localizer_; }

    // `fn(now, before)` after every change SetLanguage made.
    using Listener = std::function<void(const std::string& now, const std::string& before)>;
    u64 AddListener(Listener fn);
    void RemoveListener(u64 id);

    // The text for a key: the table along the fallback chain, else `fallback`
    // (the source text), else the key. With `args` it is formatted.
    std::string Text(const std::string& key, const std::string& fallback = {}, const FormatArgs* args = nullptr,
                     std::vector<std::string>* errors = nullptr) const {
        return localizer_.Resolve(LocText{key, fallback}, args, errors);
    }
    bool Has(const std::string& key) const { return localizer_.IsTranslated(LocText{key, {}}); }

    // The one the Blueprint and Luau libraries talk to (null if none).
    static Localization* Active();
    void MakeActive();

private:
    StringTable table_;
    Localizer localizer_;
    std::vector<std::pair<u64, Listener>> listeners_;
    u64 next_id_ = 0;
};

} // namespace aether::loc
