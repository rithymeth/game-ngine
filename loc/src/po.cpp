#include "aether/loc/po.h"

#include "aether/loc/language.h"

namespace aether::loc {

namespace {

std::string Escape(std::string_view s) {
    std::string out;
    for (const char c : s) {
        switch (c) {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\t': out += "\\t"; break;
        case '\r': out += "\\r"; break;
        default: out += c;
        }
    }
    return out;
}

// The text between the quotes of a `"..."` line; false if it isn't one.
bool Unquote(std::string_view s, std::string& out) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\r' || s.back() == '\t')) s.remove_suffix(1);
    if (s.size() < 2 || s.front() != '"' || s.back() != '"') return false;
    s.remove_prefix(1);
    s.remove_suffix(1);
    out.clear();
    for (usize i = 0; i < s.size(); ++i) {
        if (s[i] != '\\') {
            if (s[i] == '"') return false; // an unescaped quote
            out += s[i];
            continue;
        }
        if (++i >= s.size()) return false;
        switch (s[i]) {
        case '\\': out += '\\'; break;
        case '"': out += '"'; break;
        case 'n': out += '\n'; break;
        case 't': out += '\t'; break;
        case 'r': out += '\r'; break;
        default: return false;
        }
    }
    return true;
}

std::string_view Trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

bool StartsWith(std::string_view s, std::string_view prefix) { return s.substr(0, prefix.size()) == prefix; }

struct Entry {
    std::string ctxt, id, str;
    bool has_ctxt = false, has_id = false, has_str = false, fuzzy = false, obsolete = false;
    usize line = 0;
};

} // namespace

std::string ExportPo(const StringTable& table, std::string_view language, std::string_view source_language, const std::string& project_name) {
    std::string out = "msgid \"\"\nmsgstr \"\"\n";
    if (!project_name.empty()) out += "\"Project-Id-Version: " + Escape(project_name) + "\\n\"\n";
    out += "\"MIME-Version: 1.0\\n\"\n\"Content-Type: text/plain; charset=UTF-8\\n\"\n\"Content-Transfer-Encoding: 8bit\\n\"\n";
    out += "\"Language: " + NormalizeLanguage(language) + "\\n\"\n";
    for (const std::string& key : table.Keys()) {
        const std::string* source = table.Find(source_language, key);
        const std::string* translation = table.Find(language, key);
        out += "\nmsgctxt \"" + Escape(key) + "\"\n";
        out += "msgid \"" + Escape(source ? *source : key) + "\"\n";
        out += "msgstr \"" + Escape(translation ? *translation : std::string()) + "\"\n";
    }
    return out;
}

bool ImportPo(StringTable& table, std::string_view language, std::string_view po, std::vector<std::string>* errors) {
    const auto problem = [&](usize line, const std::string& message) {
        if (errors) errors->push_back("line " + std::to_string(line) + ": " + message);
    };
    Entry e;
    enum class Field { None, Ctxt, Id, Str } field = Field::None;
    bool ok = true;
    const auto commit = [&] {
        if (e.has_id || e.has_str || e.has_ctxt) {
            const bool header = !e.has_ctxt && e.id.empty();
            const std::string key = e.has_ctxt ? e.ctxt : e.id;
            if (!header && !e.obsolete && e.has_str && !e.str.empty() && !key.empty()) {
                if (e.fuzzy) problem(e.line, "'" + key + "' is marked fuzzy, so its translation was skipped");
                else table.Set(language, key, e.str);
            }
        }
        e = Entry{};
        field = Field::None;
    };
    usize line_no = 0, pos = 0;
    while (pos <= po.size()) {
        usize end = po.find('\n', pos);
        if (end == std::string_view::npos) end = po.size();
        std::string_view line = Trim(po.substr(pos, end - pos));
        pos = end + 1;
        ++line_no;
        if (line.empty()) {
            commit();
            continue;
        }
        if (StartsWith(line, "#~")) {
            e.obsolete = true;
            continue;
        }
        if (line[0] == '#') {
            if (StartsWith(line, "#,") && line.find("fuzzy") != std::string_view::npos) e.fuzzy = true;
            continue;
        }
        const auto field_of = [&](std::string_view word, Field f) -> bool {
            if (!StartsWith(line, word) || line.size() <= word.size() || (line[word.size()] != ' ' && line[word.size()] != '"')) return false;
            if (f == Field::Ctxt && e.has_str) commit();
            if (f == Field::Id && e.has_str) commit();
            if (e.line == 0) e.line = line_no;
            std::string text;
            if (!Unquote(Trim(line.substr(word.size())), text)) {
                problem(line_no, "expected a quoted string after " + std::string(word));
                ok = false;
                return true;
            }
            field = f;
            (f == Field::Ctxt ? e.ctxt : f == Field::Id ? e.id : e.str) = std::move(text);
            (f == Field::Ctxt ? e.has_ctxt : f == Field::Id ? e.has_id : e.has_str) = true;
            return true;
        };
        if (field_of("msgctxt", Field::Ctxt) || field_of("msgid", Field::Id) || field_of("msgstr", Field::Str)) {
            if (!ok) return false;
            continue;
        }
        if (line[0] == '"') { // a continuation of the field before it
            std::string text;
            if (field == Field::None || !Unquote(line, text)) {
                problem(line_no, field == Field::None ? "a string with no field before it" : "a malformed string");
                return false;
            }
            (field == Field::Ctxt ? e.ctxt : field == Field::Id ? e.id : e.str) += text;
            continue;
        }
        problem(line_no, "not valid .po: '" + std::string(line.substr(0, 24)) + "'");
        commit();
        return false;
    }
    commit();
    return ok;
}

std::string PoLanguage(std::string_view po) {
    std::vector<std::string> ignore;
    // The header is the entry with an empty msgid; read its msgstr's lines.
    const usize at = po.find("Language:");
    if (at == std::string_view::npos) return {};
    usize end = at + 9;
    while (end < po.size() && po[end] == ' ') ++end;
    const usize start = end;
    while (end < po.size() && po[end] != '\\' && po[end] != '"' && po[end] != '\n' && po[end] != '\r') ++end;
    return NormalizeLanguage(Trim(po.substr(start, end - start)));
}

} // namespace aether::loc
