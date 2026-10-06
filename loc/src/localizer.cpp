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

const std::string* Localizer::Lookup(const std::string& key) const {
    if (!table_ || key.empty()) return nullptr;
    for (const std::string& lang : Chain()) {
        if (const std::string* text = table_->Find(lang, key)) return text;
    }
    return nullptr;
}

bool Localizer::IsTranslated(const LocText& text) const { return Lookup(text.key) != nullptr; }

std::string Localizer::Resolve(const LocText& text, const FormatArgs* args, std::vector<std::string>* errors) const {
    const std::string* found = Lookup(text.key);
    const std::string& chosen = found ? *found : (!text.source.empty() ? text.source : text.key);
    return args ? Format(chosen, language_, *args, errors) : chosen;
}

} // namespace aether::loc
