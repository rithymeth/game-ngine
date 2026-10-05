#include "aether/loc/message_format.h"

#include "aether/loc/language.h"

#include <cmath>
#include <cstdio>

namespace aether::loc {

namespace {

std::string BaseLanguage(std::string_view language) {
    const std::string tag = NormalizeLanguage(language);
    return tag.substr(0, tag.find('-'));
}

std::string Stringify(const FormatValue& v) {
    if (const i64* i = std::get_if<i64>(&v)) return std::to_string(*i);
    if (const double* d = std::get_if<double>(&v)) {
        char buf[48];
        std::snprintf(buf, sizeof buf, "%g", *d);
        return buf;
    }
    return std::get<std::string>(v);
}

bool AsNumber(const FormatValue& v, double& out) {
    if (const i64* i = std::get_if<i64>(&v)) {
        out = static_cast<double>(*i);
        return true;
    }
    if (const double* d = std::get_if<double>(&v)) {
        out = *d;
        return true;
    }
    return false;
}

std::string Trim(std::string_view s) {
    while (!s.empty() && s.front() == ' ') s.remove_prefix(1);
    while (!s.empty() && s.back() == ' ') s.remove_suffix(1);
    return std::string(s);
}

} // namespace

std::string PluralCategory(std::string_view language, double n) {
    const std::string lang = BaseLanguage(language);
    const bool whole = std::floor(n) == n && std::fabs(n) < 1e15;
    const i64 i = whole ? static_cast<i64>(std::fabs(n)) : 0;
    if (lang == "ja" || lang == "zh" || lang == "ko" || lang == "vi" || lang == "th" || lang == "id" || lang == "ms") return "other";
    if (lang == "fr") return (n >= 0 && n < 2) ? "one" : "other"; // 0, 1 and fractions below 2
    if (lang == "pt") {
        const bool pt_pt = NormalizeLanguage(language) == "pt-PT";
        if (pt_pt) return whole && i == 1 ? "one" : "other";
        return (n >= 0 && n < 2) ? "one" : "other"; // pt-BR: 0 and 1
    }
    if (lang == "ru" || lang == "uk") {
        if (!whole) return "other";
        if (i % 10 == 1 && i % 100 != 11) return "one";
        if (i % 10 >= 2 && i % 10 <= 4 && !(i % 100 >= 12 && i % 100 <= 14)) return "few";
        return "many";
    }
    if (lang == "pl") {
        if (!whole) return "other";
        if (i == 1) return "one";
        if (i % 10 >= 2 && i % 10 <= 4 && !(i % 100 >= 12 && i % 100 <= 14)) return "few";
        return "many";
    }
    if (lang == "ar") {
        if (!whole) return "other";
        if (i == 0) return "zero";
        if (i == 1) return "one";
        if (i == 2) return "two";
        if (i % 100 >= 3 && i % 100 <= 10) return "few";
        if (i % 100 >= 11) return "many";
        return "other";
    }
    return whole && i == 1 ? "one" : "other"; // English and the languages that follow it
}

std::string Format(std::string_view pattern, std::string_view language, const FormatArgs& args, std::vector<std::string>* errors) {
    const auto problem = [&](const std::string& message) {
        if (errors) errors->push_back(message);
    };
    std::string out;
    for (usize i = 0; i < pattern.size(); ++i) {
        const char c = pattern[i];
        if (c == '{') {
            if (i + 1 < pattern.size() && pattern[i + 1] == '{') {
                out += '{';
                ++i;
                continue;
            }
            const usize close = pattern.find('}', i + 1);
            if (close == std::string_view::npos) {
                problem("An unclosed '{' (use '{{' for a literal brace)");
                out.append(pattern.substr(i));
                break;
            }
            const std::string_view body = pattern.substr(i + 1, close - i - 1);
            const std::string original(pattern.substr(i, close - i + 1));
            const usize bar = body.find('|');
            const std::string name = Trim(bar == std::string_view::npos ? body : body.substr(0, bar));
            const auto arg = args.find(name);
            if (arg == args.end()) {
                problem("No argument named '" + name + "'");
                out += original;
            } else if (bar == std::string_view::npos) {
                out += Stringify(arg->second);
            } else {
                double number = 0;
                if (!AsNumber(arg->second, number)) {
                    problem("'" + name + "' isn't a number, so it has no plural form");
                    out += original;
                } else {
                    const std::string category = PluralCategory(language, number);
                    const std::string spec(body.substr(bar + 1));
                    if (spec.find(':') == std::string::npos) {
                        if (category != "one") out += spec; // {count|s}
                    } else {
                        std::string chosen, other;
                        bool found = false, found_other = false;
                        usize pos = 0;
                        while (pos <= spec.size()) {
                            usize end = spec.find(';', pos);
                            if (end == std::string::npos) end = spec.size();
                            const std::string form = spec.substr(pos, end - pos);
                            const usize colon = form.find(':');
                            if (colon != std::string::npos) {
                                const std::string cat = Trim(form.substr(0, colon));
                                if (cat == category && !found) {
                                    chosen = form.substr(colon + 1);
                                    found = true;
                                } else if (cat == "other") {
                                    other = form.substr(colon + 1);
                                    found_other = true;
                                }
                            }
                            pos = end + 1;
                        }
                        out += found ? chosen : (found_other ? other : std::string());
                    }
                }
            }
            i = close;
        } else if (c == '}') {
            if (i + 1 < pattern.size() && pattern[i + 1] == '}') {
                ++i;
            } else {
                problem("A stray '}' (use '}}' for a literal brace)");
            }
            out += '}';
        } else {
            out += c;
        }
    }
    return out;
}

} // namespace aether::loc
