#include "aether/loc/localized_path.h"

#include "aether/loc/language.h"

#include <algorithm>

namespace aether::loc {

namespace {

// "dir/name" and ".ext" (the extension of the last path part, with its dot; empty if none).
void SplitExtension(std::string_view path, std::string_view& stem, std::string_view& ext) {
    const usize slash = path.find_last_of('/');
    const usize dot = path.find_last_of('.');
    if (dot == std::string_view::npos || (slash != std::string_view::npos && dot < slash) || dot == 0) {
        stem = path;
        ext = {};
        return;
    }
    stem = path.substr(0, dot);
    ext = path.substr(dot);
}

bool IsLanguageTag(std::string_view s) {
    if (s.size() < 2) return false;
    usize i = 0;
    while (i < s.size() && s[i] != '-') ++i;
    if (i < 2 || i > 3) return false;
    for (usize k = 0; k < i; ++k) {
        if (s[k] < 'a' || s[k] > 'z') return false;
    }
    for (usize k = i; k < s.size();) { // each "-Subtag" is 2-8 letters or digits
        if (s[k] != '-') return false;
        usize end = k + 1;
        while (end < s.size() && s[end] != '-') {
            const char c = s[end];
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) return false;
            ++end;
        }
        if (end - k - 1 < 2 || end - k - 1 > 8) return false;
        k = end;
    }
    return true;
}

} // namespace

std::vector<std::string> LocalizedCandidates(std::string_view path, std::string_view language, std::string_view default_language) {
    std::vector<std::string> out;
    std::string_view stem, ext;
    SplitExtension(path, stem, ext);
    const std::string base_language = NormalizeLanguage(default_language);
    for (const std::string& tag : FallbackChain(language, default_language)) {
        if (tag == base_language) continue;
        out.push_back(std::string(stem) + "." + tag + std::string(ext));
    }
    out.emplace_back(path);
    return out;
}

bool SplitVariant(std::string_view path, std::string* base, std::string* language) {
    std::string_view stem, ext;
    SplitExtension(path, stem, ext);
    const usize slash = stem.find_last_of('/');
    const usize name_start = slash == std::string_view::npos ? 0 : slash + 1;
    const usize dot = stem.find_last_of('.');
    if (dot == std::string_view::npos || dot < name_start || dot == name_start) return false;
    const std::string_view tag = stem.substr(dot + 1);
    if (!IsLanguageTag(tag)) return false;
    if (base) *base = std::string(stem.substr(0, dot)) + std::string(ext);
    if (language) *language = std::string(tag);
    return true;
}

std::vector<std::string> OrphanedVariants(const std::vector<std::string>& paths) {
    std::vector<std::string> out;
    for (const std::string& p : paths) {
        std::string base;
        if (SplitVariant(p, &base) && std::find(paths.begin(), paths.end(), base) == paths.end()) out.push_back(p);
    }
    return out;
}

} // namespace aether::loc
