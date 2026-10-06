#include "aether/loc/string_table.h"

#include "aether/loc/language.h"

#include <algorithm>

namespace aether::loc {

void StringTable::Set(std::string_view language, const std::string& key, std::string text) {
    const std::string lang = NormalizeLanguage(language);
    if (lang.empty() || key.empty()) return;
    entries_[key][lang] = std::move(text);
    languages_.insert(lang);
}

bool StringTable::AddLanguage(std::string_view language) {
    const std::string lang = NormalizeLanguage(language);
    if (lang.empty()) return false;
    languages_.insert(lang);
    return true;
}

const std::string* StringTable::Find(std::string_view language, const std::string& key) const {
    const auto row = entries_.find(key);
    if (row == entries_.end()) return nullptr;
    const auto cell = row->second.find(NormalizeLanguage(language));
    return cell == row->second.end() ? nullptr : &cell->second;
}

bool StringTable::Remove(std::string_view language, const std::string& key) {
    const auto row = entries_.find(key);
    if (row == entries_.end()) return false;
    const bool removed = row->second.erase(NormalizeLanguage(language)) != 0;
    if (row->second.empty()) entries_.erase(row);
    if (removed && Count(language) == 0) languages_.erase(NormalizeLanguage(language));
    return removed;
}

std::vector<std::string> StringTable::Keys() const {
    std::vector<std::string> keys;
    keys.reserve(entries_.size());
    for (const auto& [key, row] : entries_) keys.push_back(key);
    return keys;
}

void StringTable::Clear() {
    entries_.clear();
    languages_.clear();
}

usize StringTable::Count(std::string_view language) const {
    const std::string lang = NormalizeLanguage(language);
    usize n = 0;
    for (const auto& [key, row] : entries_) n += row.count(lang);
    return n;
}

namespace {

// Splits CSV text into rows of cells. False if a quote never closes.
bool ParseCsv(std::string_view text, std::vector<std::vector<std::string>>& rows) {
    if (text.size() >= 3 && text.substr(0, 3) == "\xEF\xBB\xBF") text.remove_prefix(3);
    std::vector<std::string> row;
    std::string cell;
    bool in_quotes = false, cell_started = false;
    const auto end_cell = [&] {
        row.push_back(std::move(cell));
        cell.clear();
        cell_started = false;
    };
    const auto end_row = [&] {
        end_cell();
        if (!(row.size() == 1 && row[0].empty())) rows.push_back(std::move(row)); // blank lines are skipped
        row.clear();
    };
    for (usize i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (in_quotes) {
            if (c == '"') {
                if (i + 1 < text.size() && text[i + 1] == '"') {
                    cell += '"';
                    ++i;
                } else {
                    in_quotes = false;
                }
            } else {
                cell += c;
            }
        } else if (c == '"' && !cell_started) {
            in_quotes = true;
            cell_started = true;
        } else if (c == ',') {
            end_cell();
        } else if (c == '\n' || c == '\r') {
            if (c == '\r' && i + 1 < text.size() && text[i + 1] == '\n') ++i;
            end_row();
        } else {
            cell += c;
            cell_started = true;
        }
    }
    if (in_quotes) return false;
    if (cell_started || !cell.empty() || !row.empty()) end_row();
    return true;
}

void AppendCell(std::string& out, const std::string& cell) {
    const bool quote = cell.find_first_of(",\"\r\n") != std::string::npos;
    if (!quote) {
        out += cell;
        return;
    }
    out += '"';
    for (const char c : cell) {
        if (c == '"') out += '"';
        out += c;
    }
    out += '"';
}

} // namespace

bool StringTable::ImportCsv(std::string_view csv, std::vector<std::string>* errors) {
    const auto fail = [&](const std::string& message) {
        if (errors) errors->push_back(message);
        return false;
    };
    std::vector<std::vector<std::string>> rows;
    if (!ParseCsv(csv, rows)) return fail("A quoted cell never closes");
    if (rows.empty()) return fail("The CSV is empty: expected a header row starting with 'key'");
    const std::vector<std::string>& header = rows[0];
    if (header.empty() || header[0] != "key") return fail("The first header cell must be 'key'");
    std::vector<std::string> languages(header.size()); // empty: not a language column
    for (usize c = 1; c < header.size(); ++c) {
        if (header[c] == "comment") continue;
        languages[c] = NormalizeLanguage(header[c]);
        if (!languages[c].empty()) languages_.insert(languages[c]); // a column with no cells yet is still a language
        if (languages[c].empty() && errors) errors->push_back("Column " + std::to_string(c + 1) + " has no language name and was skipped");
    }
    std::set<std::string> seen;
    for (usize r = 1; r < rows.size(); ++r) {
        const std::vector<std::string>& row = rows[r];
        const std::string& key = row[0];
        if (key.empty()) {
            if (errors) errors->push_back("Row " + std::to_string(r + 1) + " has no key and was skipped");
            continue;
        }
        if (!seen.insert(key).second) {
            if (errors) errors->push_back("The key '" + key + "' appears twice (row " + std::to_string(r + 1) + "): the later row wins");
        }
        for (usize c = 1; c < row.size() && c < languages.size(); ++c) {
            if (languages[c].empty() || row[c].empty()) continue;
            Set(languages[c], key, row[c]);
        }
    }
    return true;
}

std::string StringTable::ExportCsv() const {
    std::string out = "key";
    for (const std::string& lang : languages_) {
        out += ',';
        AppendCell(out, lang);
    }
    out += "\r\n";
    for (const auto& [key, row] : entries_) {
        AppendCell(out, key);
        for (const std::string& lang : languages_) {
            out += ',';
            const auto cell = row.find(lang);
            if (cell != row.end()) AppendCell(out, cell->second);
        }
        out += "\r\n";
    }
    return out;
}

} // namespace aether::loc
