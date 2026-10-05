#include "aether/loc/gather.h"

#include "aether/platform/filesystem.h"

#include <algorithm>
#include <map>

namespace aether::loc {

namespace stdfs = std::filesystem;
using reflect::Json;

namespace {

bool IsText(const Json& j, const char* key) { return j.contains(key) && j[key].is_string(); }

bool IsLocText(const Json& j) {
    if (!j.is_object() || !IsText(j, "key") || !IsText(j, "source") || j["key"].get<std::string>().empty()) return false;
    for (auto it = j.begin(); it != j.end(); ++it) {
        if (it.key() != "key" && it.key() != "source" && it.key() != "$v") return false;
    }
    return true;
}

bool IsLocalizeNode(const std::string& type) {
    return type == "Call.Native:Localize.GetText" || type == "Call.Native:Localize.FormatInt" || type == "Call.Native:Localize.FormatString" ||
           type == "Call.Native:UI.SetTextKey";
}

void Walk(const Json& j, const std::string& where, const std::string& path, GatherReport& report) {
    if (j.is_array()) {
        usize i = 0;
        for (const Json& item : j) Walk(item, where, path + "/" + std::to_string(i++), report);
        return;
    }
    if (!j.is_object()) return;
    const auto add = [&](const std::string& key, const std::string& source) {
        if (!key.empty()) report.entries.push_back({key, source, where + ": " + (path.empty() ? "/" : path)});
    };
    if (IsLocText(j)) {
        add(j["key"].get<std::string>(), j["source"].get<std::string>());
        return;
    }
    if (IsText(j, "text_key")) add(j["text_key"].get<std::string>(), IsText(j, "text") ? j["text"].get<std::string>() : std::string());
    if (IsText(j, "hint_key")) add(j["hint_key"].get<std::string>(), IsText(j, "hint") ? j["hint"].get<std::string>() : std::string());
    if (IsText(j, "type") && IsLocalizeNode(j["type"].get<std::string>())) {
        const Json defaults = j.contains("defaults") && j["defaults"].is_object() ? j["defaults"] : Json::object();
        if (IsText(defaults, "key") && !defaults["key"].get<std::string>().empty()) {
            add(defaults["key"].get<std::string>(), IsText(defaults, "default") ? defaults["default"].get<std::string>() : std::string());
        } else {
            report.warnings.push_back(where + ": " + (path.empty() ? "/" : path) + ": a " + j["type"].get<std::string>() +
                                      " node's key isn't a literal, so it can't be gathered");
        }
    }
    for (auto it = j.begin(); it != j.end(); ++it) Walk(it.value(), where, path + "/" + it.key(), report);
}

} // namespace

void GatherJson(const Json& document, const std::string& where, GatherReport& report) { Walk(document, where, "", report); }

GatherReport GatherContent(const stdfs::path& content_dir) {
    GatherReport out;
    std::vector<stdfs::path> files;
    std::error_code ec;
    if (stdfs::is_directory(content_dir, ec)) {
        for (stdfs::recursive_directory_iterator it(content_dir, ec), end; !ec && it != end; it.increment(ec)) {
            if (!it->is_regular_file()) continue;
            const std::string ext = it->path().extension().string();
            if (ext == ".ascene" || ext == ".aprefab" || ext == ".aui" || ext == ".abp") files.push_back(it->path());
        }
    }
    std::sort(files.begin(), files.end());
    GatherReport raw;
    for (const stdfs::path& file : files) {
        const std::string rel = stdfs::relative(file, content_dir, ec).generic_string();
        std::string text;
        if (!fs::ReadFileText(file.string(), text)) {
            out.warnings.push_back(rel + ": couldn't be read");
            continue;
        }
        const Json doc = Json::parse(text, nullptr, /*allow_exceptions=*/false);
        if (doc.is_discarded()) {
            out.warnings.push_back(rel + ": isn't valid JSON, so nothing was gathered from it");
            continue;
        }
        GatherJson(doc, rel, raw);
    }
    for (std::string& w : raw.warnings) out.warnings.push_back(std::move(w));
    std::map<std::string, GatheredText> by_key; // first seen wins (files are in path order)
    for (const GatheredText& t : raw.entries) {
        const auto it = by_key.find(t.key);
        if (it == by_key.end()) {
            by_key.emplace(t.key, t);
        } else if (it->second.source != t.source && !t.source.empty() && !it->second.source.empty()) {
            out.warnings.push_back("The key '" + t.key + "' has two different texts: \"" + it->second.source + "\" (" + it->second.where + ") and \"" +
                                   t.source + "\" (" + t.where + "); the first is kept");
        } else if (it->second.source.empty()) {
            it->second.source = t.source; // a use with no text gets the one that has it
        }
    }
    for (auto& [key, text] : by_key) out.entries.push_back(std::move(text));
    return out;
}

} // namespace aether::loc
