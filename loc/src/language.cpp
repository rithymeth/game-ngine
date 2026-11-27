#include "aether/loc/language.h"

#include <algorithm>

namespace aether::loc {

namespace {

char Lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }
char Upper(char c) { return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c; }

std::string_view TakeSubtag(std::string_view& tag) {
    const usize cut = tag.find('-');
    const std::string_view subtag = tag.substr(0, cut);
    tag = cut == std::string_view::npos ? std::string_view{} : tag.substr(cut + 1);
    return subtag;
}

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
        // RFC 4647 §3.4: an extension singleton does not stand on its own
        // after its last value is removed. This includes a leading private-use x.
        while (!tag.empty()) {
            const usize separator = tag.rfind('-');
            const usize start = separator == std::string::npos ? 0 : separator + 1;
            if (tag.size() - start != 1) break;
            const char c = Lower(tag[start]);
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) break;
            tag.resize(separator == std::string::npos ? 0 : separator);
        }
    }
    add(NormalizeLanguage(default_language));
    return chain;
}

bool IsRtl(std::string_view language) {
    const std::string normalized = NormalizeLanguage(language);
    std::string_view rest = normalized;
    const std::string_view base = TakeSubtag(rest);
    if (base.size() < 2) return false; // No language script in a private-use-only tag.
    std::string_view script = TakeSubtag(rest);
    // BCP 47 puts the script after the language and up to three optional extlangs.
    // Region, variants, extensions and private use cannot set the writing direction.
    if (base.size() == 2 || base.size() == 3) {
        for (int n = 0; n < 3 && script.size() == 3 &&
                        std::all_of(script.begin(), script.end(), [](char c) { return c >= 'a' && c <= 'z'; }); ++n) {
            script = TakeSubtag(rest);
        }
    }
    for (const char* rtl_script : {"Arab", "Hebr", "Thaa", "Nkoo", "Syrc", "Adlm"}) {
        if (script == rtl_script) return true;
    }
    if (script == "Latn" || script == "Cyrl") return false;
    for (const char* rtl : {"ar", "he", "iw", "fa", "ur", "ps", "sd", "ug", "yi", "dv", "ckb"}) {
        if (base == rtl) return true;
    }
    return false;
}

} // namespace aether::loc
