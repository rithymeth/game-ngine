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
    bool has_ctxt = false, has_id = false, has_str = false, fuzzy = false;
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

namespace {

bool IsHeader(const Entry& e) { return e.has_id && e.id.empty() && !e.has_ctxt && e.has_str; }

template <typename Visit>
bool ReadEntries(std::string_view po, std::vector<std::string>* errors, Visit visit) {
    const auto problem = [&](usize line, const std::string& message) {
        if (errors) errors->push_back("line " + std::to_string(line) + ": " + message);
    };
    Entry e;
    enum class Field { None, Ctxt, Id, Str } field = Field::None;
    bool ok = true;
    bool done = false;
    const auto commit = [&] {
        if (e.has_id || e.has_str || e.has_ctxt) done = visit(e);
        e = Entry{};
        field = Field::None;
    };
    usize line_no = 0, pos = 0;
    while (!done && pos <= po.size()) {
        usize end = po.find('\n', pos);
        if (end == std::string_view::npos) end = po.size();
        std::string_view line = Trim(po.substr(pos, end - pos));
        pos = end + 1;
        ++line_no;
        if (line.empty()) {
            commit();
            continue;
        }
        if (line[0] == '#') {
            // Comments introduce the next entry, even without a blank line.
            // Obsolete entries consist of comment lines; don't carry their flags forward.
            const bool obsolete = StartsWith(line, "#~");
            if (e.has_str || obsolete) commit();
            if (done) break;
            if (obsolete) continue;
            if (StartsWith(line, "#,") && line.find("fuzzy") != std::string_view::npos) e.fuzzy = true;
            continue;
        }
        const auto field_of = [&](std::string_view word, Field f) -> bool {
            if (!StartsWith(line, word) || line.size() <= word.size() || (line[word.size()] != ' ' && line[word.size()] != '"')) return false;
            if (f == Field::Ctxt && e.has_str) commit();
            if (f == Field::Id && e.has_str) commit();
            if (done) return true;
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

} // namespace

bool ImportPo(StringTable& table, std::string_view language, std::string_view po, std::vector<std::string>* errors) {
    return ReadEntries(po, errors, [&](const Entry& e) {
        const auto& key = e.has_ctxt ? e.ctxt : e.id;
        if (!IsHeader(e) && e.has_str && !e.str.empty() && !key.empty()) {
            if (e.fuzzy) {
                if (errors) errors->push_back("line " + std::to_string(e.line) + ": '" + key +
                                              "' is marked fuzzy, so its translation was skipped");
            } else {
                table.Set(language, key, e.str);
            }
        }
        return false;
    });
}

std::string PoLanguage(std::string_view po) {
    std::string language;
    ReadEntries(po, nullptr, [&](const Entry& e) {
        if (!IsHeader(e)) return false;
        // Decode continuations first and match the field at the start of a header line.
        std::string_view header = e.str;
        while (!header.empty()) {
            const usize end = header.find('\n');
            const auto line = Trim(header.substr(0, end));
            if (StartsWith(line, "Language:")) {
                language = NormalizeLanguage(Trim(line.substr(9)));
                break;
            }
            if (end == std::string_view::npos) break;
            header.remove_prefix(end + 1);
        }
        return true;
    });
    return language;
}

} // namespace aether::loc
