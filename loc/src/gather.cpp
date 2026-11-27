#include "aether/loc/gather.h"

#include "aether/platform/filesystem.h"

#include <algorithm>
#include <limits>
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

struct WiredPins {
    bool key = false;
    bool default_text = false;
};

u32 ReadNodeId(const Json& value) {
    if (!value.is_number_integer()) return 0;
    const u64 id = value.get<u64>();
    return id <= std::numeric_limits<u32>::max() ? static_cast<u32>(id) : 0;
}

std::map<u32, WiredPins> WiredLocalizationPins(const Json& links) {
    std::map<u32, WiredPins> pins;
    for (const Json& link : links) {
        const auto to = link.find("to");
        if (to == link.end() || !to->is_array() || to->size() != 2 || !(*to)[1].is_string()) continue;
        const u32 id = ReadNodeId((*to)[0]);
        if (id == 0) continue;
        const auto& pin = (*to)[1].get_ref<const std::string&>();
        if (pin == "key") pins[id].key = true;
        if (pin == "default") pins[id].default_text = true;
    }
    return pins;
}

void Walk(const Json& j, const std::string& where, const std::string& path, GatherReport& report, WiredPins pins = {}) {
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
        if (!pins.key && IsText(defaults, "key") && !defaults["key"].get<std::string>().empty()) {
            const bool has_default = !pins.default_text && IsText(defaults, "default");
            add(defaults["key"].get<std::string>(), has_default ? defaults["default"].get<std::string>() : std::string());
        } else {
            report.warnings.push_back(where + ": " + (path.empty() ? "/" : path) + ": a " + j["type"].get<std::string>() +
                                      " node's key isn't a literal, so it can't be gathered");
        }
    }
    for (auto it = j.begin(); it != j.end(); ++it) {
        const std::string child_path = path + "/" + it.key();
        const auto links = it.key() == "nodes" && it->is_array() ? j.find("links") : j.end();
        if (links == j.end() || !links->is_array()) {
            Walk(it.value(), where, child_path, report);
            continue;
        }
        // IDs are local to each Blueprint graph, and incoming links win over
        // the saved defaults that are retained for when the wire is removed.
        const auto wired = WiredLocalizationPins(*links);
        usize index = 0;
        for (const Json& node : *it) {
            const auto id = node.find("id");
            const auto found = id != node.end() ? wired.find(ReadNodeId(*id)) : wired.end();
            Walk(node, where, child_path + "/" + std::to_string(index++), report,
                 found == wired.end() ? WiredPins{} : found->second);
        }
    }
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
            std::string ext = it->path().extension().string();
            // Match the asset database for ASCII extensions, without depending on the system locale.
            for (char& c : ext) {
                if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            }
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
    std::map<std::string, GatheredText> by_key; // first nonempty source wins (files are in path order)
    for (const GatheredText& t : raw.entries) {
        const auto it = by_key.find(t.key);
        if (it == by_key.end()) {
            by_key.emplace(t.key, t);
        } else if (it->second.source != t.source && !t.source.empty() && !it->second.source.empty()) {
            out.warnings.push_back("The key '" + t.key + "' has two different texts: \"" + it->second.source + "\" (" + it->second.where + ") and \"" +
                                   t.source + "\" (" + t.where + "); the first is kept");
        } else if (it->second.source.empty() && !t.source.empty()) {
            it->second = t; // Keep the chosen text and its source location together.
        }
    }
    for (auto& [key, text] : by_key) out.entries.push_back(std::move(text));
    return out;
}

} // namespace aether::loc
