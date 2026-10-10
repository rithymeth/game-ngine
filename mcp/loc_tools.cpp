#include "loc_tools.h"

#include "project_host.h"

#include "aether/loc/gather.h"
#include "aether/loc/language.h"
#include "aether/loc/merge.h"
#include "aether/loc/message_format.h"
#include "aether/loc/po.h"
#include "aether/loc/project_sync.h"
#include "aether/loc/pseudo.h"
#include "aether/loc/string_table.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <set>
#include <sstream>

namespace aether::mcp {

namespace {

using namespace detail;
namespace lc = aether::loc;

constexpr const char* kDefaultStrings = "Localization/Strings.astrings";

std::string ReadAll(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void WriteAll(const fs::path& file, const std::string& text) {
    std::error_code ec;
    if (file.has_parent_path()) fs::create_directories(file.parent_path(), ec);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!out) throw ToolError("Could not write " + file.string());
}

std::vector<const AssetRecord*> StringAssets(OpenProject& p) {
    std::vector<const AssetRecord*> out;
    for (const AssetRecord* r : p.database->All()) {
        if (!r->IsSubAsset() && r->importer == "StringTable") out.push_back(r);
    }
    return out;
}

// Which .astrings a tool edits: `strings` (relative to Content/), else the project's only one, else the default path (made on first write).
std::string TargetStrings(OpenProject& p, const Json& args) {
    if (args.contains("strings")) {
        if (!args["strings"].is_string()) throw ToolError("\"strings\" must be a path under Content/");
        std::string rel = CleanRelative(args["strings"].get<std::string>());
        if (rel.empty()) throw ToolError("\"strings\" must name a file under Content/");
        if (fs::path(rel).extension() != ".astrings") rel += ".astrings";
        return rel;
    }
    const std::vector<const AssetRecord*> assets = StringAssets(p);
    if (assets.size() == 1) return assets[0]->path;
    if (assets.empty()) return kDefaultStrings;
    std::string list;
    for (const AssetRecord* r : assets) list += (list.empty() ? "" : ", ") + r->path;
    throw ToolError("The project has several string tables (" + list + "): say which with \"strings\"");
}

lc::StringTable LoadTable(OpenProject& p, const std::string& rel, std::vector<std::string>* errors = nullptr) {
    lc::StringTable table;
    const fs::path file = p.paths.content / rel;
    std::error_code ec;
    if (fs::is_regular_file(file, ec)) table.ImportCsv(ReadAll(file), errors);
    return table;
}

// Every string table in the project merged, as the game does.
lc::StringTable LoadAllTables(OpenProject& p, std::vector<std::string>* errors = nullptr) {
    lc::StringTable merged;
    for (const AssetRecord* r : StringAssets(p)) merged.ImportCsv(ReadAll(p.database->SourcePath(r->guid)), errors);
    return merged;
}

void SaveTable(OpenProject& p, const std::string& rel, const lc::StringTable& table) {
    WriteAll(p.paths.content / rel, table.ExportCsv());
    p.database->Scan();
}

std::string SourceLanguage(const Json& args) {
    const std::string tag = args.contains("source_language") && args["source_language"].is_string() ? args["source_language"].get<std::string>() : "en";
    const std::string normalized = lc::NormalizeLanguage(tag);
    if (normalized.empty()) throw ToolError("\"source_language\" must be a language tag such as en or pt-BR");
    return normalized;
}

std::string RequireLanguage(const Json& args, const char* key) {
    const std::string normalized = lc::NormalizeLanguage(RequireString(args, key));
    if (normalized.empty()) throw ToolError(std::string("\"") + key + "\" must be a language tag such as en or pt-BR");
    return normalized;
}

// The argument names a message uses ({name}, {count|s}, {n|one:x;other:y}), and the first syntax problem if any.
struct Placeholders {
    std::set<std::string> names;
    std::string problem;
};
Placeholders ScanPlaceholders(const std::string& text) {
    Placeholders out;
    for (usize i = 0; i < text.size(); ++i) {
        if (text[i] == '{') {
            if (i + 1 < text.size() && text[i + 1] == '{') {
                ++i;
                continue;
            }
            const usize close = text.find('}', i + 1);
            if (close == std::string::npos) {
                if (out.problem.empty()) out.problem = "an unclosed { at position " + std::to_string(i);
                break;
            }
            std::string name = text.substr(i + 1, close - i - 1);
            const usize bar = name.find('|');
            if (bar != std::string::npos) name = name.substr(0, bar);
            while (!name.empty() && name.front() == ' ') name.erase(name.begin());
            while (!name.empty() && name.back() == ' ') name.pop_back();
            if (name.empty()) {
                if (out.problem.empty()) out.problem = "an empty {} at position " + std::to_string(i);
            } else {
                out.names.insert(name);
            }
            i = close;
        } else if (text[i] == '}' && i + 1 < text.size() && text[i + 1] == '}') {
            ++i;
        }
    }
    return out;
}

Json ListJson(const std::set<std::string>& s) { return Json(std::vector<std::string>(s.begin(), s.end())); }

} // namespace

void RegisterLocTools(McpServer& server, std::shared_ptr<AssetHost> host) {
    server.AddTool(
        {"loc_status",
         "The open project's localization: its string tables (.astrings), and for the merged table the languages, key count, and each language's completeness "
         "with the first few missing keys. Languages are normalized tags (pt_br and PT-br become pt-BR).",
         Schema({{"strings", {{"type", "string"}, {"description", "Only this table (default: all merged, as the game loads them)"}}},
                 {"source_language", {{"type", "string"}, {"description", "Default en"}}},
                 {"missing_limit", {{"type", "integer"}, {"description", "Missing keys listed per language (default 10)"}}}}),
         [host](const Json& args) -> Json {
             OpenProject& p = host->Require();
             const std::string source = SourceLanguage(args);
             std::vector<std::string> errors;
             lc::StringTable table;
             Json tables = Json::array();
             for (const AssetRecord* r : StringAssets(p)) tables.push_back(r->path);
             if (args.contains("strings")) table = LoadTable(p, TargetStrings(p, args), &errors);
             else table = LoadAllTables(p, &errors);
             usize limit = 10;
             if (args.contains("missing_limit") && args["missing_limit"].is_number_integer()) limit = static_cast<usize>(std::max<long long>(0, args["missing_limit"].get<long long>()));
             Json languages = Json::array();
             for (const std::string& lang : table.Languages()) {
                 Json missing = Json::array();
                 usize missing_count = 0;
                 for (const std::string& key : table.Keys()) {
                     if (table.Find(lang, key) != nullptr) continue;
                     ++missing_count;
                     if (missing.size() < limit) missing.push_back(key);
                 }
                 languages.push_back({{"language", lang}, {"translated", table.Count(lang)}, {"missing", missing_count},
                                      {"complete_percent", table.KeyCount() ? 100.0 * static_cast<double>(table.Count(lang)) / static_cast<double>(table.KeyCount()) : 100.0},
                                      {"right_to_left", lc::IsRtl(lang)}, {"fallback_chain", lc::FallbackChain(lang, source)}, {"first_missing", missing}});
             }
             return {{"tables", tables}, {"keys", table.KeyCount()}, {"source_language", source}, {"languages", languages}, {"errors", errors}};
         }});

    server.AddTool(
        {"loc_get", "The text of one key in every language of the merged string table, and which language the game would show for each fallback chain.",
         Schema({{"key", {{"type", "string"}}}}, {"key"}), [host](const Json& args) -> Json {
             OpenProject& p = host->Require();
             const std::string key = RequireString(args, "key");
             const lc::StringTable table = LoadAllTables(p);
             if (!table.HasKey(key)) throw ToolError("No key \"" + key + "\" in the project's string tables");
             Json texts = Json::object();
             for (const std::string& lang : table.Languages()) {
                 if (const std::string* t = table.Find(lang, key)) texts[lang] = *t;
             }
             return {{"key", key}, {"translations", texts}};
         }});

    server.AddTool(
        {"loc_set",
         "Add or change translations of a key: `translations` maps a language tag to its text, e.g. {\"en\": \"Play\", \"fr\": \"Jouer\"}. Writes the .astrings (CSV) "
         "file (made on first use) and scans it. A new language becomes a column. An empty text removes that translation.",
         Schema({{"key", {{"type", "string"}}}, {"translations", {{"type", "object"}}}, {"strings", {{"type", "string"}, {"description", "Table under Content/ (default: the project's only one)"}}}},
                {"key", "translations"}),
         [host](const Json& args) -> Json {
             OpenProject& p = host->Require();
             const std::string key = RequireString(args, "key");
             if (key.empty()) throw ToolError("\"key\" must not be empty");
             if (!args["translations"].is_object() || args["translations"].empty()) throw ToolError("\"translations\" must be an object of language to text");
             const std::string rel = TargetStrings(p, args);
             lc::StringTable table = LoadTable(p, rel);
             Json applied = Json::object();
             for (auto& [lang, text] : args["translations"].items()) {
                 if (!text.is_string()) throw ToolError("The text for \"" + lang + "\" must be a string");
                 const std::string tag = lc::NormalizeLanguage(lang);
                 if (tag.empty()) throw ToolError("\"" + lang + "\" is not a language tag");
                 if (text.get<std::string>().empty()) table.Remove(tag, key);
                 else table.Set(tag, key, text.get<std::string>());
                 applied[tag] = text;
             }
             SaveTable(p, rel, table);
             return {{"strings", rel}, {"key", key}, {"applied", applied}, {"keys", table.KeyCount()}, {"languages", table.Languages()}};
         }});

    server.AddTool({"loc_remove", "Delete a key (every language) from a string table.",
                    Schema({{"key", {{"type", "string"}}}, {"strings", {{"type", "string"}}}}, {"key"}), [host](const Json& args) -> Json {
                        OpenProject& p = host->Require();
                        const std::string key = RequireString(args, "key");
                        const std::string rel = TargetStrings(p, args);
                        lc::StringTable table = LoadTable(p, rel);
                        if (!table.HasKey(key)) throw ToolError("No key \"" + key + "\" in " + rel);
                        for (const std::string& lang : table.Languages()) table.Remove(lang, key);
                        SaveTable(p, rel, table);
                        return {{"strings", rel}, {"removed", key}, {"keys", table.KeyCount()}};
                    }});

    server.AddTool(
        {"loc_gather",
         "The strings the project's content uses: every Text widget's text_key (with its text), LocText values, and Blueprint nodes that call Localize.GetText / "
         "FormatInt / FormatString or UI.SetTextKey with a literal key; from scenes, prefabs, UI layouts and Blueprints. Compared with the merged string table: "
         "keys the content uses that the table lacks (new), and keys the table has that nothing uses (unused). Changes nothing; loc_sync applies it.",
         Schema({{"source_language", {{"type", "string"}, {"description", "Default en"}}}, {"limit", {{"type", "integer"}, {"description", "Entries listed (default 200)"}}}}),
         [host](const Json& args) -> Json {
             OpenProject& p = host->Require();
             const std::string source = SourceLanguage(args);
             const lc::GatherReport report = lc::GatherContent(p.paths.content);
             const lc::StringTable table = LoadAllTables(p);
             usize limit = 200;
             if (args.contains("limit") && args["limit"].is_number_integer()) limit = static_cast<usize>(std::max<long long>(1, args["limit"].get<long long>()));
             Json entries = Json::array(), fresh = Json::array();
             std::set<std::string> used;
             for (const lc::GatheredText& e : report.entries) {
                 used.insert(e.key);
                 const bool is_new = !table.HasKey(e.key);
                 if (entries.size() < limit) entries.push_back({{"key", e.key}, {"source", e.source}, {"where", e.where}, {"in_table", !is_new}});
                 if (is_new && fresh.size() < limit) fresh.push_back(e.key);
             }
             Json unused = Json::array();
             for (const std::string& key : table.Keys()) {
                 if (used.count(key) == 0 && unused.size() < limit) unused.push_back(key);
             }
             return {{"source_language", source}, {"found", report.entries.size()}, {"entries", entries}, {"new_keys", fresh}, {"unused_keys", unused}, {"warnings", report.warnings}};
         }});

    server.AddTool(
        {"loc_sync",
         "Bring a string table up to date with the content (what aether_loc does): gathered keys the table lacks are added with their source text; a changed source "
         "text is updated; no translation is touched and no key is removed (unused ones are reported). Optionally write one .po file per language to `po_out` for "
         "translators, and apply returned .po files from `po_in` first. With write false (the default) it only reports.",
         Schema({{"strings", {{"type", "string"}, {"description", "Table under Content/ (default: the project's only one)"}}},
                 {"source_language", {{"type", "string"}}},
                 {"languages", {{"type", "array"}, {"items", {{"type", "string"}}}, {"description", "Languages to write .po files for (default: all but the source)"}}},
                 {"po_out", {{"type", "string"}, {"description", "Folder for <language>.po files"}}},
                 {"po_in", {{"type", "string"}, {"description", "A .po file or folder of them to apply first"}}},
                 {"write", {{"type", "boolean"}, {"description", "Write the table and .po files (default false: report only)"}}}}),
         [host](const Json& args) -> Json {
             OpenProject& p = host->Require();
             lc::SyncOptions o;
             o.content_dir = p.paths.content;
             o.strings_file = p.paths.content / TargetStrings(p, args);
             o.source_language = SourceLanguage(args);
             if (args.contains("languages")) {
                 if (!args["languages"].is_array()) throw ToolError("\"languages\" must be an array of language tags");
                 for (const Json& l : args["languages"]) {
                     if (!l.is_string() || lc::NormalizeLanguage(l.get<std::string>()).empty()) throw ToolError("\"languages\" holds language tags");
                     o.languages.push_back(lc::NormalizeLanguage(l.get<std::string>()));
                 }
             }
             if (args.contains("po_out") && args["po_out"].is_string()) o.po_out = args["po_out"].get<std::string>();
             if (args.contains("po_in") && args["po_in"].is_string()) o.po_in = args["po_in"].get<std::string>();
             o.write = args.contains("write") && args["write"].is_boolean() && args["write"].get<bool>();
             const lc::SyncReport r = lc::SyncProject(o);
             if (o.write) p.database->Scan();
             return {{"ok", r.ok}, {"written", o.write}, {"found", r.gathered.entries.size()}, {"added", r.merge.added}, {"updated_source", r.merge.updated_source},
                     {"unused", r.merge.unused}, {"po_imported", r.po_imported}, {"po_written", r.po_written}, {"messages", r.messages}, {"gather_warnings", r.gathered.warnings}};
         }});

    server.AddTool(
        {"loc_validate",
         "Check the merged string table: every translation must use the same placeholders as the source text (a missing or extra {name} breaks the formatted "
         "text), every message must be well-formed (no unclosed brace), and each language's completeness is reported. Errors name the key and language.",
         Schema({{"source_language", {{"type", "string"}}}, {"strings", {{"type", "string"}, {"description", "Only this table (default: all merged)"}}}}), [host](const Json& args) -> Json {
             OpenProject& p = host->Require();
             const std::string source = SourceLanguage(args);
             std::vector<std::string> load_errors;
             const lc::StringTable table = args.contains("strings") ? LoadTable(p, TargetStrings(p, args), &load_errors) : LoadAllTables(p, &load_errors);
             Json problems = Json::array(), warnings = Json::array();
             for (const std::string& e : load_errors) problems.push_back({{"problem", e}});
             usize checked = 0;
             for (const std::string& key : table.Keys()) {
                 const std::string* src = table.Find(source, key);
                 Placeholders source_names;
                 if (src != nullptr) {
                     source_names = ScanPlaceholders(*src);
                     if (!source_names.problem.empty()) problems.push_back({{"key", key}, {"language", source}, {"problem", "the source text has " + source_names.problem}});
                 } else {
                     warnings.push_back({{"key", key}, {"warning", "no text in the source language " + source}});
                 }
                 for (const std::string& lang : table.Languages()) {
                     if (lang == source) continue;
                     const std::string* text = table.Find(lang, key);
                     if (text == nullptr) continue;
                     ++checked;
                     const Placeholders names = ScanPlaceholders(*text);
                     if (!names.problem.empty()) {
                         problems.push_back({{"key", key}, {"language", lang}, {"problem", "has " + names.problem}});
                         continue;
                     }
                     if (src == nullptr) continue;
                     std::set<std::string> missing, extra;
                     std::set_difference(source_names.names.begin(), source_names.names.end(), names.names.begin(), names.names.end(), std::inserter(missing, missing.end()));
                     std::set_difference(names.names.begin(), names.names.end(), source_names.names.begin(), source_names.names.end(), std::inserter(extra, extra.end()));
                     if (!missing.empty() || !extra.empty()) {
                         problems.push_back({{"key", key}, {"language", lang}, {"problem", "placeholders differ from the source"}, {"missing", ListJson(missing)}, {"extra", ListJson(extra)}});
                     }
                 }
             }
             Json completeness = Json::array();
             for (const std::string& lang : table.Languages()) {
                 completeness.push_back({{"language", lang}, {"translated", table.Count(lang)}, {"of", table.KeyCount()}});
             }
             return {{"ok", problems.empty()}, {"keys", table.KeyCount()}, {"translations_checked", checked}, {"problems", problems}, {"warnings", warnings}, {"languages", completeness}};
         }});

    server.AddTool(
        {"loc_export_po", "Write a gettext .po file for one language (every key with its source text and the translation, empty if untranslated) for translators.",
         Schema({{"language", {{"type", "string"}}}, {"path", {{"type", "string"}, {"description", "Where to write the .po file"}}}, {"source_language", {{"type", "string"}}}, {"strings", {{"type", "string"}}}},
                {"language", "path"}),
         [host](const Json& args) -> Json {
             OpenProject& p = host->Require();
             const std::string language = RequireLanguage(args, "language");
             const std::string po = lc::ExportPo(args.contains("strings") ? LoadTable(p, TargetStrings(p, args)) : LoadAllTables(p), language, SourceLanguage(args), p.settings.name);
             WriteAll(RequireString(args, "path"), po);
             return {{"path", RequireString(args, "path")}, {"language", language}, {"bytes", po.size()}};
         }});

    server.AddTool(
        {"loc_import_po", "Apply a translator's .po file to a string table as `language` (empty and fuzzy entries are skipped; fuzzy ones are reported) and save the table.",
         Schema({{"language", {{"type", "string"}, {"description", "Default: the .po file's own Language header"}}}, {"path", {{"type", "string"}}}, {"strings", {{"type", "string"}}}}, {"path"}),
         [host](const Json& args) -> Json {
             OpenProject& p = host->Require();
             const std::string file = RequireString(args, "path");
             std::ifstream in(file, std::ios::binary);
             if (!in) throw ToolError("Could not read " + file);
             std::stringstream ss;
             ss << in.rdbuf();
             const std::string po = ss.str();
             std::string language;
             if (args.contains("language")) language = RequireLanguage(args, "language");
             else language = lc::NormalizeLanguage(lc::PoLanguage(po));
             if (language.empty()) throw ToolError("The .po file has no Language header: give \"language\"");
             const std::string rel = TargetStrings(p, args);
             lc::StringTable table = LoadTable(p, rel);
             const usize before = table.Count(language);
             std::vector<std::string> errors;
             const bool ok = lc::ImportPo(table, language, po, &errors);
             if (!ok) throw ToolError("Not a valid .po file: " + (errors.empty() ? std::string("parse error") : errors.back()));
             SaveTable(p, rel, table);
             return {{"strings", rel}, {"language", language}, {"translations_before", before}, {"translations_after", table.Count(language)}, {"messages", errors}};
         }});

    server.AddTool(
        {"loc_format",
         "Preview how a message formats, with no project needed: `pattern` with {name}, {count|s} (an s unless one) and {n|one:coin;other:coins} plurals, filled from "
         "`args` (name -> number or text) using the plural rules of `language`. Returns the text and any formatting problems (an unclosed brace, a missing argument), "
         "plus the plural category each numeric argument falls in.",
         Schema({{"pattern", {{"type", "string"}}}, {"language", {{"type", "string"}, {"description", "Default en"}}}, {"args", {{"type", "object"}}}}, {"pattern"}),
         [](const Json& args) -> Json {
             const std::string pattern = RequireString(args, "pattern");
             const std::string language = args.contains("language") ? RequireLanguage(args, "language") : "en";
             lc::FormatArgs values;
             Json categories = Json::object();
             if (args.contains("args")) {
                 if (!args["args"].is_object()) throw ToolError("\"args\" must be an object of name to number or text");
                 for (auto& [name, v] : args["args"].items()) {
                     if (v.is_number_integer()) values[name] = static_cast<i64>(v.get<long long>());
                     else if (v.is_number()) values[name] = v.get<double>();
                     else if (v.is_string()) values[name] = v.get<std::string>();
                     else throw ToolError("Argument \"" + name + "\" must be a number or text");
                     if (v.is_number()) categories[name] = lc::PluralCategory(language, v.get<double>());
                 }
             }
             std::vector<std::string> errors;
             const std::string text = lc::Format(pattern, language, values, &errors);
             return {{"text", text}, {"language", language}, {"problems", errors}, {"plural_categories", categories}, {"placeholders", ListJson(ScanPlaceholders(pattern).names)}};
         }});

    server.AddTool(
        {"loc_language",
         "About a language tag: its normalized form, the fallback chain the game tries (pt-BR gives pt-BR, pt, en), whether it is written right to left, and the plural "
         "category of some numbers (default 0, 1, 2, 3, 5, 11, 21, 1.5) so you can see how many plural forms a translation needs.",
         Schema({{"language", {{"type", "string"}}}, {"default_language", {{"type", "string"}}}, {"numbers", {{"type", "array"}, {"items", {{"type", "number"}}}}}}, {"language"}),
         [](const Json& args) -> Json {
             const std::string language = RequireLanguage(args, "language");
             const std::string fallback = args.contains("default_language") ? RequireLanguage(args, "default_language") : "en";
             std::vector<double> numbers = {0, 1, 2, 3, 5, 11, 21, 1.5};
             if (args.contains("numbers")) {
                 if (!args["numbers"].is_array() || args["numbers"].size() > 100) throw ToolError("\"numbers\" must be an array of up to 100 numbers");
                 numbers.clear();
                 for (const Json& n : args["numbers"]) {
                     if (!n.is_number()) throw ToolError("\"numbers\" must be an array of numbers");
                     numbers.push_back(n.get<double>());
                 }
             }
             Json plural = Json::array();
             for (double n : numbers) plural.push_back({{"n", n}, {"category", lc::PluralCategory(language, n)}});
             return {{"language", language}, {"fallback_chain", lc::FallbackChain(language, fallback)}, {"right_to_left", lc::IsRtl(language)}, {"plural_categories", plural}};
         }});

    server.AddTool(
        {"loc_pseudo",
         "Pseudo-localize text to find what a real translation would break before there is one: accented letters (a font missing them shows up), about a third longer "
         "(a layout that only fits English shows up), wrapped in [ ] (clipped ends, and text that skipped the string tables, stand out). {placeholders} and line breaks "
         "are kept, so the result still formats. Give `texts`, or a `key` from the project's tables (in `source_language`).",
         Schema({{"texts", {{"type", "array"}, {"items", {{"type", "string"}}}}}, {"key", {{"type", "string"}}}, {"source_language", {{"type", "string"}}},
                 {"expansion", {{"type", "number"}, {"description", "Extra length as a fraction (default 0.35)"}}}, {"accents", {{"type", "boolean"}}}, {"brackets", {{"type", "boolean"}}}}),
         [host](const Json& args) -> Json {
             lc::PseudoOptions options;
             if (args.contains("expansion")) {
                 if (!args["expansion"].is_number() || args["expansion"].get<double>() < 0.0 || args["expansion"].get<double>() > 3.0) throw ToolError("\"expansion\" must be a number from 0 to 3");
                 options.expansion = args["expansion"].get<f32>();
             }
             if (args.contains("accents") && args["accents"].is_boolean()) options.accents = args["accents"].get<bool>();
             if (args.contains("brackets") && args["brackets"].is_boolean()) options.brackets = args["brackets"].get<bool>();
             std::vector<std::string> inputs;
             if (args.contains("texts")) {
                 if (!args["texts"].is_array() || args["texts"].empty() || args["texts"].size() > 200) throw ToolError("\"texts\" must be 1 to 200 strings");
                 for (const Json& t : args["texts"]) {
                     if (!t.is_string()) throw ToolError("\"texts\" must be strings");
                     inputs.push_back(t.get<std::string>());
                 }
             } else if (args.contains("key")) {
                 OpenProject& p = host->Require();
                 const lc::StringTable table = LoadAllTables(p);
                 const std::string source = SourceLanguage(args);
                 const std::string* text = table.Find(source, RequireString(args, "key"));
                 if (text == nullptr) throw ToolError("No " + source + " text for that key in the project's string tables");
                 inputs.push_back(*text);
             } else {
                 throw ToolError("Give \"texts\" or a \"key\"");
             }
             Json out = Json::array();
             for (const std::string& t : inputs) {
                 const std::string pseudo = lc::Pseudo(t, options);
                 out.push_back({{"source", t}, {"pseudo", pseudo}, {"bytes_before", t.size()}, {"bytes_after", pseudo.size()}});
             }
             return {{"language", lc::PseudoLanguage()}, {"results", out}};
         }});
}

} // namespace aether::mcp
