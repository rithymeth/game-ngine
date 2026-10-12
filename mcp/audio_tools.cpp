#include "audio_tools.h"

#include "project_host.h"

#include "aether/audio/cue.h"
#include "aether/audio/sound.h"

#include <cmath>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>

namespace aether::mcp {

namespace {

using namespace detail;

// The sound assets (.wav, .ogg, ...) the open project has, by content path.
std::set<std::string> ProjectSounds(OpenProject& p) {
    std::set<std::string> sounds;
    for (const AssetRecord* r : p.database->All()) {
        if (!r->IsSubAsset() && r->importer == "Sound") sounds.insert(r->path);
    }
    return sounds;
}

std::string ReadAll(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// A cue from the tool arguments: `definition` (an object or its text), or `cue` naming a cue asset in the open project.
audio::SoundCue ReadCue(AssetHost& host, const Json& args, std::string* from) {
    std::string text;
    if (args.contains("definition")) {
        const Json& d = args["definition"];
        if (d.is_object()) text = d.dump();
        else if (d.is_string()) text = d.get<std::string>();
        else throw ToolError("\"definition\" must be a JSON object or a string of JSON");
        if (from != nullptr) *from = "definition";
    } else if (args.contains("cue")) {
        OpenProject& p = host.Require();
        const AssetRecord& r = FindAsset(p, RequireString(args, "cue"));
        if (r.importer != "SoundCue") throw ToolError(r.path + " is not a sound cue (it is a " + r.importer + ")");
        text = ReadAll(p.database->SourcePath(r.guid));
        if (from != nullptr) *from = r.path;
    } else {
        throw ToolError("Give \"definition\" (the cue as JSON) or \"cue\" (a cue asset in the open project)");
    }
    audio::SoundCue cue;
    std::string error;
    if (!audio::LoadCue(text, cue, &error)) throw ToolError("Not a valid sound cue: " + error);
    return cue;
}

Json DiagnosticsJson(const std::vector<audio::CueDiagnostic>& diagnostics) {
    Json out = Json::array();
    for (const audio::CueDiagnostic& d : diagnostics) {
        out.push_back({{"code", d.code}, {"severity", d.error ? "error" : "warning"}, {"node", d.node}, {"message", d.message}});
    }
    return out;
}

bool HasError(const std::vector<audio::CueDiagnostic>& diagnostics) {
    for (const audio::CueDiagnostic& d : diagnostics) {
        if (d.error) return true;
    }
    return false;
}

} // namespace

void RegisterAudioTools(McpServer& server, std::shared_ptr<AssetHost> host) {
    server.AddTool(
        {"cue_validate",
         "Check a sound cue with the audio kit's own diagnostics: every problem with a code (CU001...), severity, node and message. Give the "
         "cue as `definition`, or as `cue` (an asset path in the open project). With a project open, a Wave node's sound must exist in it "
         "(CU007); without one that is not checked.",
         Schema({{"definition", {{"description", "The cue: a JSON object, or its text"}}}, {"cue", {{"type", "string"}, {"description", "A cue asset path in the open project"}}}}),
         [host](const Json& args) -> Json {
             std::string from;
             const audio::SoundCue cue = ReadCue(*host, args, &from);
             std::function<bool(const std::string&)> exists;
             std::set<std::string> sounds;
             const bool checked = host->project != nullptr;
             if (checked) {
                 sounds = ProjectSounds(*host->project);
                 exists = [&sounds](const std::string& s) { return sounds.count(s) != 0; };
             }
             const std::vector<audio::CueDiagnostic> diagnostics = audio::ValidateCue(cue, exists);
             return {{"ok", !HasError(diagnostics)}, {"cue", from}, {"sounds_checked_against_project", checked}, {"diagnostics", DiagnosticsJson(diagnostics)}};
         }});

    server.AddTool(
        {"cue_preview",
         "What a cue would play, without making a sound: evaluates the cue graph (Random picks, Sequence order, Modulator ranges, Loops, "
         "Delays) and lists each sound with its start offset, volume (dB) and pitch. `plays` evaluates it several times in a row so "
         "Random and Sequence variation shows; `seed` makes the randomness repeatable. Sound lengths are read from the open project's .wav "
         "files; any other sound is assumed to last a second (listed in assumed_length_sounds), which only affects offsets after it.",
         Schema({{"definition", {{"description", "The cue: a JSON object, or its text"}}},
                 {"cue", {{"type", "string"}, {"description", "A cue asset path in the open project"}}},
                 {"plays", {{"type", "integer"}, {"description", "1-50, default 5"}}},
                 {"seed", {{"type", "integer"}, {"description", "Random seed (default fixed)"}}}}),
         [host](const Json& args) -> Json {
             std::string from;
             const audio::SoundCue cue = ReadCue(*host, args, &from);
             const std::vector<audio::CueDiagnostic> diagnostics = audio::ValidateCue(cue);
             if (HasError(diagnostics)) {
                 return {{"ok", false}, {"cue", from}, {"diagnostics", DiagnosticsJson(diagnostics)}};
             }
             long long plays = 5;
             if (args.contains("plays")) {
                 if (!args["plays"].is_number_integer()) throw ToolError("\"plays\" must be an integer");
                 plays = args["plays"].get<long long>();
             }
             if (plays < 1 || plays > 50) throw ToolError("\"plays\" must be between 1 and 50");
             unsigned long long seed = 0x9E3779B97F4A7C15ull;
             if (args.contains("seed")) {
                 if (!args["seed"].is_number_integer()) throw ToolError("\"seed\" must be an integer");
                 seed = static_cast<unsigned long long>(args["seed"].get<long long>());
             }
             audio::CueState state(seed);
             // A cue plays nothing for a sound of unknown length, so give each Wave a length: the real one where the open project has
             // the .wav, else an assumed second (and say so).
             constexpr double kAssumedSeconds = 1.0;
             std::map<std::string, double> lengths;
             std::set<std::string> assumed;
             const audio::SoundDuration unknown = [&](const std::string& sound) -> double {
                 const auto cached = lengths.find(sound);
                 if (cached != lengths.end()) return cached->second;
                 double seconds = kAssumedSeconds;
                 bool known = false;
                 if (host->project != nullptr && fs::path(sound).extension() == ".wav") {
                     if (const AssetRecord* r = host->project->database->FindByPath(sound)) {
                         const std::string text = ReadAll(host->project->database->SourcePath(r->guid));
                         audio::SoundWave wave;
                         if (audio::DecodeWav(std::vector<u8>(text.begin(), text.end()), wave) && wave.Duration() > 0.0f) {
                             seconds = wave.Duration();
                             known = true;
                         }
                     }
                 }
                 if (!known) assumed.insert(sound);
                 lengths[sound] = seconds;
                 return seconds;
             };
             Json runs = Json::array();
             for (long long i = 0; i < plays; ++i) {
                 const audio::CuePlan plan = audio::EvaluateCue(cue, state, unknown);
                 Json items = Json::array();
                 for (const audio::CueItem& item : plan.items) {
                     items.push_back({{"sound", item.sound}, {"offset", item.offset}, {"volume_db", item.volume_db}, {"pitch", item.pitch}, {"loop", item.loop}});
                 }
                 Json run = {{"items", items}, {"endless_loops", plan.tails.size()}};
                 if (std::isfinite(plan.duration)) run["duration"] = plan.duration;
                 runs.push_back(run);
             }
             return {{"ok", true},
                     {"cue", from},
                     {"output", {{"bus", cue.bus}, {"volume_db", cue.volume_db}, {"pitch", cue.pitch}, {"spatial", cue.spatial}, {"priority", cue.priority}}},
                     {"warnings", DiagnosticsJson(diagnostics)},
                     {"sound_lengths", lengths},
                     {"assumed_length_sounds", assumed},
                     {"assumed_length_seconds", kAssumedSeconds},
                     {"plays", runs}};
         }});
}

} // namespace aether::mcp
