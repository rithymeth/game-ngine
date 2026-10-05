#include "aether/loc/language.h"

#include <algorithm>

namespace aether::loc {

namespace {

char Lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }
char Upper(char c) { return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c; }

} // namespace

std::string NormalizeLanguage(std::string_view tag) {
    std::string out;
    usize part = 0;
    std::string current;
    const auto flush = [&] {
        if (current.empty()) return;
        std::string fixed;
        for (usize i = 0; i < current.size(); ++i) {
            if (part == 0) fixed += Lower(current[i]);
            else if (current.size() == 2) fixed += Upper(current[i]);
            else if (current.size() == 4) fixed += i == 0 ? Upper(current[i]) : Lower(current[i]);
            else fixed += Lower(current[i]);
        }
        if (!out.empty()) out += '-';
        out += fixed;
        ++part;
        current.clear();
    };
    for (const char c : tag) {
        if (c == '-' || c == '_') {
            flush();
        } else if (c != ' ') {
            current += c;
        }
    }
    flush();
    return out;
}

std::vector<std::string> FallbackChain(std::string_view language, std::string_view default_language) {
    std::vector<std::string> chain;
    const auto add = [&](const std::string& tag) {
        if (!tag.empty() && std::find(chain.begin(), chain.end(), tag) == chain.end()) chain.push_back(tag);
    };
    std::string tag = NormalizeLanguage(language);
    while (!tag.empty()) {
        add(tag);
        const usize cut = tag.rfind('-');
        if (cut == std::string::npos) break;
        tag.resize(cut);
    }
    add(NormalizeLanguage(default_language));
    return chain;
}

bool IsRtl(std::string_view language) {
    const std::string tag = NormalizeLanguage(language);
    if (tag.empty()) return false;
    // An explicit script decides: Arabic, Hebrew, Thaana, Nko, Syriac, Adlam are right to left.
    for (const char* script : {"Arab", "Hebr", "Thaa", "Nkoo", "Syrc", "Adlm"}) {
        if (tag.find(std::string("-") + script) != std::string::npos) return true;
    }
    if (tag.find("-Latn") != std::string::npos || tag.find("-Cyrl") != std::string::npos) return false;
    const std::string base = tag.substr(0, tag.find('-'));
    for (const char* rtl : {"ar", "he", "iw", "fa", "ur", "ps", "sd", "ug", "yi", "dv", "ckb"}) {
        if (base == rtl) return true;
    }
    return false;
}

} // namespace aether::loc
