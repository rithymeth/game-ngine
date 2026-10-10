#include "save_tools.h"

#include "project_host.h"

#include "aether/reflection/serialize.h"
#include "aether/save/envelope.h"
#include "aether/save/save_system.h"
#include "aether/save/settings.h"

#include <algorithm>

namespace aether::mcp {

namespace {

using namespace detail;
namespace sv = aether::save;

fs::path RequireDirectory(const Json& args) {
    const fs::path dir(RequireString(args, "directory"));
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) throw ToolError("No such folder: " + dir.string());
    return dir;
}

const char* ErrorName(sv::SaveError e) {
    switch (e) {
    case sv::SaveError::None: return "none";
    case sv::SaveError::InvalidSlot: return "invalid_slot";
    case sv::SaveError::IoError: return "io_error";
    case sv::SaveError::Corrupt: return "corrupt";
    case sv::SaveError::WrongType: return "wrong_type";
    case sv::SaveError::FutureVersion: return "future_version";
    case sv::SaveError::NotFound: return "not_found";
    }
    return "none";
}

Json ResultJson(const sv::SaveResult& r) {
    Json j = {{"ok", r.ok}};
    if (!r.ok) {
        j["error"] = ErrorName(r.error);
        j["message"] = r.message;
    }
    if (!r.warnings.empty()) j["warnings"] = r.warnings;
    return j;
}

Json InfoJson(const sv::envelope::EnvelopeInfo& info, usize max_data) {
    Json j = {{"readable", info.ok}, {"bytes", info.bytes}};
    if (!info.ok) {
        j["error"] = info.error;
        return j;
    }
    j["kind"] = info.kind;
    j["format"] = info.format;
    j["type"] = info.type;
    j["type_version"] = info.type_version;
    j["slot"] = info.slot;
    j["timestamp"] = info.timestamp;
    j["checksum_ok"] = info.checksum_ok;
    const std::string text = info.data.dump();
    if (text.size() > max_data) {
        j["data_truncated"] = true;
        j["data_bytes"] = text.size();
    } else {
        j["data"] = info.data;
    }
    return j;
}

fs::path SlotFile(const fs::path& dir, const std::string& slot) {
    if (!sv::SaveSystem::ValidSlotName(slot)) throw ToolError("A slot name is 1-64 of A-Z a-z 0-9 _ -");
    return dir / (slot + ".asav");
}

} // namespace

void RegisterSaveTools(McpServer& server) {
    server.AddTool(
        {"save_list",
         "The save slots in a folder (`<slot>.asav`): each one's struct type, schema version, time saved (unix seconds), size, whether it reads, whether a `.bak` backup "
         "sits beside it, and whether the folder has a settings file.",
         Schema({{"directory", {{"type", "string"}, {"description", "The save folder"}}}}, {"directory"}),
         [](const Json& args) -> Json {
             const fs::path dir = RequireDirectory(args);
             sv::SaveSystem saves(dir);
             Json slots = Json::array();
             for (const sv::SlotInfo& s : saves.ListSlots()) {
                 std::error_code ec;
                 slots.push_back({{"slot", s.slot}, {"type", s.type}, {"version", s.version}, {"timestamp", s.timestamp}, {"bytes", s.bytes}, {"valid", s.valid},
                                  {"has_backup", fs::exists(dir / (s.slot + ".asav.bak"), ec)}});
             }
             std::error_code ec;
             return {{"directory", dir.string()}, {"count", slots.size()}, {"slots", slots}, {"has_settings", fs::exists(dir / "settings.asettings", ec)}};
         }});

    server.AddTool(
        {"save_inspect",
         "What a save (or settings) file claims to be, without loading it into a game: its kind (aether.save / aether.settings), struct type and version, slot, time, whether "
         "the checksum matches the data, and the saved data itself as JSON (left out above `max_data` bytes, default 200000). Give `directory` and `slot` (or `file` for "
         "a path, e.g. settings.asettings). With `backup` true it reads the `.bak` beside it instead, which is what loading falls back to when the file is damaged.",
         Schema({{"directory", {{"type", "string"}}},
                 {"slot", {{"type", "string"}}},
                 {"file", {{"type", "string"}, {"description", "A file path, instead of directory + slot"}}},
                 {"backup", {{"type", "boolean"}}},
                 {"max_data", {{"type", "integer"}}}}),
         [](const Json& args) -> Json {
             fs::path file;
             if (args.contains("file")) {
                 file = fs::path(RequireString(args, "file"));
             } else {
                 file = SlotFile(RequireDirectory(args), RequireString(args, "slot"));
             }
             if (OptBool(args, "backup", false)) file += ".bak";
             std::error_code ec;
             if (!fs::exists(file, ec)) throw ToolError("No such file: " + file.string());
             const usize max_data = args.contains("max_data") && args["max_data"].is_number_integer() ? static_cast<usize>(std::max(0, args["max_data"].get<i32>())) : 200000;
             Json j = InfoJson(sv::envelope::Inspect(file), max_data);
             j["file"] = file.string();
             return j;
         }});

    server.AddTool(
        {"save_delete", "Deletes a save slot and its `.bak` backup. Fails if there was neither.",
         Schema({{"directory", {{"type", "string"}}}, {"slot", {{"type", "string"}}}}, {"directory", "slot"}),
         [](const Json& args) -> Json {
             const fs::path dir = RequireDirectory(args);
             const std::string slot = RequireString(args, "slot");
             sv::SaveSystem saves(dir);
             const sv::SaveResult r = saves.DeleteSlot(slot);
             if (!r.ok) throw ToolError(std::string("Couldn't delete ") + slot + ": " + r.message);
             return {{"deleted", slot}};
         }});

    server.AddTool(
        {"save_settings",
         "The player's settings file (`settings.asettings` in `directory`): quality preset, window size, fullscreen, vsync, master / music / sfx / voice volumes (0..1), language "
         "and key rebinds. A missing file gives the defaults. Without `set` or `reset` it only reads. `set` ({field: value}, top-level fields) is merged in, validated the way the "
         "game does (volumes clamped to 0..1, window size to 320..16384, empty names back to default; each fix is a warning) and written atomically with a backup; `reset` writes "
         "the defaults. A damaged file reads as the defaults with the reason in `load_problem`.",
         Schema({{"directory", {{"type", "string"}}}, {"set", {{"type", "object"}}}, {"reset", {{"type", "boolean"}}}}, {"directory"}),
         [](const Json& args) -> Json {
             const fs::path dir = RequireDirectory(args);
             sv::SettingsStore<sv::GameSettings> store(dir);
             const sv::SaveResult loaded = store.Load();
             Json out = {{"file", store.Path().string()}};
             if (!loaded.ok || loaded.error != sv::SaveError::None) {
                 out["load_problem"] = loaded.message;
             }
             std::vector<std::string> warnings = loaded.warnings;
             const bool reset = OptBool(args, "reset", false);
             const bool change = reset || args.contains("set");
             if (change) {
                 sv::GameSettings next;
                 if (!reset) {
                     next = store.Get();
                     Json j = reflect::ToJson(next);
                     if (!args["set"].is_object()) throw ToolError("\"set\" must be an object of field -> value");
                     for (const auto& item : args["set"].items()) {
                         if (!j.contains(item.key())) throw ToolError("GameSettings has no field \"" + item.key() + "\"");
                         j[item.key()] = item.value();
                     }
                     reflect::LoadReport report;
                     if (!reflect::FromJson(next, j, &report)) throw ToolError("Those values don't fit the settings");
                     for (const std::string& w : report.warnings) warnings.push_back(w);
                 }
                 sv::Validate(next, &warnings); // Set() clamps silently; this reports what it changed
                 store.Set(next);
                 const sv::SaveResult saved = store.Save();
                 if (!saved.ok) throw ToolError("Couldn't write the settings: " + saved.message);
                 for (const std::string& w : saved.warnings) warnings.push_back(w);
                 out["written"] = true;
             }
             out["settings"] = reflect::ToJson(store.Get());
             if (!warnings.empty()) out["warnings"] = warnings;
             return out;
         }});
}

} // namespace aether::mcp
