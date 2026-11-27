#include "aether/loc/localizer.h"

#include "aether/loc/language.h"

namespace aether::loc {

void Localizer::SetLanguage(std::string language) {
    const std::string normalized = NormalizeLanguage(language);
    language_ = normalized.empty() ? default_language_ : normalized;
}

void Localizer::SetDefaultLanguage(std::string language) {
    const std::string normalized = NormalizeLanguage(language);
    default_language_ = normalized.empty() ? "en" : normalized;
}

std::vector<std::string> Localizer::Chain() const { return FallbackChain(language_, default_language_); }

namespace {

const std::string* FindLocalizedEntry(const StringTable* table, const std::string& key,
                                      std::string_view language, std::string_view default_language,
                                      std::string* entry_language = nullptr) {
    if (!table || key.empty()) return nullptr;
    for (const std::string& tag : FallbackChain(language, default_language)) {
        if (const std::string* text = table->Find(tag, key)) {
            if (entry_language) *entry_language = tag;
            return text;
        }
    }
    return nullptr;
}

} // namespace

const std::string* Localizer::Lookup(const std::string& key) const {
    return FindLocalizedEntry(table_, key, language_, default_language_);
}

bool Localizer::IsTranslated(const LocText& text) const { return Lookup(text.key) != nullptr; }

std::string Localizer::Resolve(const LocText& text, const FormatArgs* args, std::vector<std::string>* errors) const {
    std::string entry_language;
    const std::string* found = FindLocalizedEntry(table_, text.key, language_, default_language_,
                                                   args ? &entry_language : nullptr);
    const std::string& chosen = found ? *found : (!text.source.empty() ? text.source : text.key);
    if (!args) return chosen;
    // Found entries use their own rules. A keyed source was authored in
    // the default language; a keyless literal still follows the requested locale.
    const bool authored_fallback = !text.IsLiteral() && !text.source.empty();
    return Format(chosen, found ? entry_language : (authored_fallback ? default_language_ : language_), *args, errors);
}

} // namespace aether::loc
