#include "aether/loc/localize.h"

#include "aether/loc/localization.h"

namespace aether::loc {

std::string Localize::GetText(const std::string& key, const std::string& fallback) {
    const Localization* l = Localization::Active();
    if (!l) return fallback.empty() ? key : fallback;
    return l->Text(key, fallback);
}

std::string Localize::FormatInt(const std::string& key, const std::string& fallback, const std::string& name, i32 value) {
    const Localization* l = Localization::Active();
    FormatArgs args{{name, static_cast<i64>(value)}};
    if (!l) return Format(fallback.empty() ? key : fallback, "en", args);
    return l->Text(key, fallback, &args);
}

std::string Localize::FormatString(const std::string& key, const std::string& fallback, const std::string& name, const std::string& value) {
    const Localization* l = Localization::Active();
    FormatArgs args{{name, value}};
    if (!l) return Format(fallback.empty() ? key : fallback, "en", args);
    return l->Text(key, fallback, &args);
}

bool Localize::HasText(const std::string& key) {
    const Localization* l = Localization::Active();
    return l && l->Has(key);
}

void Localize::SetLanguage(const std::string& language) {
    if (Localization* l = Localization::Active()) l->SetLanguage(language);
}

std::string Localize::GetLanguage() {
    const Localization* l = Localization::Active();
    return l ? l->Language() : std::string("en");
}

} // namespace aether::loc
