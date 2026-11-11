#include "core/scene_document.h"
#include "editor_tools.h"
#include "aether/blueprint/graph.h"
#include "aether/platform/filesystem.h"
#include "aether/project/project.h"
#include "aether/scene/serialization.h"
#include "aether/sequencer/sequence.h"
#include "test_framework.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

// Roadmap row A04: "load representative projects and saves in the candidate, apply migrations, save, reopen, and compare semantic state". test_legacy_files.cpp proves every fixture LOADS (with the lenient loader,
// which skips what it does not know, so it would pass even if components were dropped). This loads each one the way the editor does (refusing what it cannot fully read), saves it, reopens the saved file, and
// compares: nothing the original said may be lost or changed, and the saved file must be a fixed point (saving what was just loaded changes nothing).

using namespace aether;
using json = nlohmann::json;
namespace stdfs = std::filesystem;

namespace {

stdfs::path LegacyRoot() { return stdfs::path(AETHER_REPO_ASSETS_DIR).parent_path() / "tests/fixtures/legacy"; }

std::vector<stdfs::path> Files(const std::string& kind) {
    std::vector<stdfs::path> out;
    for (const auto& baseline : stdfs::directory_iterator(LegacyRoot())) {
        if (!baseline.is_directory()) continue;
        const stdfs::path dir = baseline.path() / kind;
        if (!stdfs::exists(dir)) continue;
        for (const auto& f : stdfs::directory_iterator(dir)) out.push_back(f.path());
    }
    std::sort(out.begin(), out.end());
    return out;
}

json ReadJson(const stdfs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return json::parse(in, nullptr, false);
}

std::string ReadText(const stdfs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// What a migration may change: a "$v" / "$version" marker is bumped by the loader, and a field the saver now writes that the old file did not have is an addition. What it may not do:
// lose a key the original had, or change a value. Returns what was lost or changed, as paths with the two values; an empty list is a faithful round trip.
void Lost(const json& original, const json& resaved, const std::string& path, std::vector<std::string>& out) {
    if (original.is_object()) {
        if (!resaved.is_object()) { out.push_back(path + ": was an object, is " + resaved.dump().substr(0, 40)); return; }
        for (auto it = original.begin(); it != original.end(); ++it) {
            if (it.key() == "$v" || it.key() == "$version") continue;
            if (!resaved.contains(it.key())) { out.push_back(path + "/" + it.key() + " (" + it.value().dump().substr(0, 50) + ") is gone"); continue; }
            Lost(it.value(), resaved[it.key()], path + "/" + it.key(), out);
        }
        return;
    }
    if (original.is_array()) {
        if (!resaved.is_array() || resaved.size() != original.size()) { out.push_back(path + ": array of " + std::to_string(original.size()) + " is now " + resaved.dump().substr(0, 40)); return; }
        for (usize i = 0; i < original.size(); ++i) Lost(original[i], resaved[i], path + "[" + std::to_string(i) + "]", out);
        return;
    }
    if (original.is_number() && resaved.is_number()) {
        if (std::fabs(original.get<double>() - resaved.get<double>()) > 1e-5 * std::max(1.0, std::fabs(original.get<double>()))) out.push_back(path + ": " + original.dump() + " became " + resaved.dump());
        return;
    }
    if (original != resaved) out.push_back(path + ": " + original.dump().substr(0, 40) + " became " + resaved.dump().substr(0, 40));
}

stdfs::path Scratch() {
    const stdfs::path dir = stdfs::temp_directory_path() / "aether_legacy_roundtrip";
    stdfs::create_directories(dir);
    return dir;
}

} // namespace

AETHER_TEST(LegacyRoundTrip_ScenesOpenInTheEditorSaveAndReopenWithNothingLost) {
    mcp::RegisterBuiltinComponents(); // what the editor has at start-up: every module's components
    usize scenes = 0;
    for (const stdfs::path& fixture : Files("scene")) {
        ++scenes;
        const std::string name = fixture.filename().string();
        // The editor refuses a scene it cannot read in full: that every fixture opens is the real test that no component of an old file is unknown now.
        editor::SceneDocument first;
        std::string error;
        const bool opened = first.Load(fixture, &error);
        if (!opened) std::printf("    %s does not open in the editor: %s\n", name.c_str(), error.c_str());
        AETHER_CHECK(opened);
        if (!opened) continue;
        const stdfs::path saved = Scratch() / ("a_" + name);
        AETHER_CHECK(first.Save(saved, &error));
        // Reopen what was saved; saving that must change nothing (a fixed point), or the saver and the loader disagree about something.
        editor::SceneDocument second;
        AETHER_CHECK(second.Load(saved, &error));
        const stdfs::path saved_again = Scratch() / ("b_" + name);
        AETHER_CHECK(second.Save(saved_again, &error));
        const bool fixed_point = ReadText(saved) == ReadText(saved_again);
        if (!fixed_point) {
            const json one = ReadJson(saved), two = ReadJson(saved_again);
            std::vector<std::string> order_one, order_two;
            std::map<std::string, json> content_one, content_two;
            for (const json& e : one["entities"]) { order_one.push_back(e["guid"]); content_one[e["guid"]] = e; }
            for (const json& e : two["entities"]) { order_two.push_back(e["guid"]); content_two[e["guid"]] = e; }
            usize changed = 0;
            std::string first_change;
            for (const auto& [guid, e] : content_one) {
                if (content_two[guid] != e) {
                    if (changed++ == 0) { std::vector<std::string> diff; Lost(e, content_two[guid], "entity " + guid.substr(0, 8), diff); first_change = diff.empty() ? "(only added keys)" : diff[0]; }
                }
            }
            std::printf("    %s: saving the reopened scene changed the file: %s order, %zu of %zu entities differ in content%s%s\n", name.c_str(), order_one == order_two ? "same" : "different", changed, content_one.size(),
                        first_change.empty() ? "" : "; first: ", first_change.c_str());
        }
        AETHER_CHECK(fixed_point);
        AETHER_CHECK(first.Entities().size() == second.Entities().size());
        // Nothing the original said is lost or changed, entity by entity (by guid: the order may differ).
        const json original = ReadJson(fixture), resaved = ReadJson(saved);
        std::map<std::string, json> by_guid;
        for (const json& entity : resaved["entities"]) by_guid[entity["guid"].get<std::string>()] = entity;
        std::vector<std::string> lost;
        for (const json& entity : original["entities"]) {
            const std::string guid = entity.value("guid", std::string());
            if (guid.empty() || !by_guid.count(guid)) { lost.push_back("entity " + guid + " is gone"); continue; }
            Lost(entity, by_guid[guid], "entity " + guid.substr(0, 8), lost);
        }
        for (usize i = 0; i < lost.size() && i < 6; ++i) std::printf("    %s: %s\n", name.c_str(), lost[i].c_str());
        if (lost.size() > 6) std::printf("    %s: ... and %zu more\n", name.c_str(), lost.size() - 6);
        AETHER_CHECK(lost.empty());
    }
    AETHER_CHECK(scenes >= 3);
}

AETHER_TEST(LegacyRoundTrip_BlueprintsSequencesAndProjectsSaveAndReopenWithNothingLost) {
    mcp::RegisterBuiltinComponents();
    for (const stdfs::path& fixture : Files("blueprint")) {
        bp::Blueprint blueprint;
        std::string error;
        AETHER_CHECK(bp::LoadBlueprint(fixture, blueprint, &error));
        const stdfs::path saved = Scratch() / ("bp_" + fixture.filename().string());
        AETHER_CHECK(bp::SaveBlueprint(blueprint, saved, &error));
        std::vector<std::string> lost;
        Lost(ReadJson(fixture), ReadJson(saved), "blueprint", lost);
        for (usize i = 0; i < lost.size() && i < 6; ++i) std::printf("    %s: %s\n", fixture.filename().string().c_str(), lost[i].c_str());
        AETHER_CHECK(lost.empty());
        bp::Blueprint again;
        AETHER_CHECK(bp::LoadBlueprint(saved, again, &error));
        const stdfs::path saved_twice = Scratch() / ("bp2_" + fixture.filename().string());
        AETHER_CHECK(bp::SaveBlueprint(again, saved_twice, &error));
        AETHER_CHECK(ReadText(saved) == ReadText(saved_twice));
    }
    for (const stdfs::path& fixture : Files("sequence")) {
        seq::LevelSequence sequence;
        std::string error;
        AETHER_CHECK(seq::LoadSequence(fixture.string(), sequence, &error));
        const stdfs::path saved = Scratch() / ("seq_" + fixture.filename().string());
        AETHER_CHECK(seq::SaveSequence(saved.string(), sequence, &error));
        std::vector<std::string> lost;
        Lost(ReadJson(fixture), ReadJson(saved), "sequence", lost);
        for (usize i = 0; i < lost.size() && i < 6; ++i) std::printf("    %s: %s\n", fixture.filename().string().c_str(), lost[i].c_str());
        AETHER_CHECK(lost.empty());
        seq::LevelSequence again;
        AETHER_CHECK(seq::LoadSequence(saved.string(), again, &error));
        const stdfs::path saved_twice = Scratch() / ("seq2_" + fixture.filename().string());
        AETHER_CHECK(seq::SaveSequence(saved_twice.string(), again, &error));
        AETHER_CHECK(ReadText(saved) == ReadText(saved_twice));
    }
    for (const stdfs::path& fixture : Files("project")) {
        ProjectSettings settings;
        std::string error;
        AETHER_CHECK(LoadProject(fixture, settings, &error));
        const stdfs::path saved = Scratch() / ("project_" + fixture.filename().string());
        AETHER_CHECK(SaveProject(saved, settings, &error));
        // The one migration a project is meant to get: it is stamped with the engine version that last saved it.
        json original_project = ReadJson(fixture);
        original_project.erase("engine_version");
        std::vector<std::string> lost;
        Lost(original_project, ReadJson(saved), "project", lost);
        AETHER_CHECK(ReadJson(saved).contains("engine_version"));
        for (usize i = 0; i < lost.size() && i < 6; ++i) std::printf("    %s: %s\n", fixture.filename().string().c_str(), lost[i].c_str());
        AETHER_CHECK(lost.empty());
        ProjectSettings again;
        AETHER_CHECK(LoadProject(saved, again, &error));
        const stdfs::path saved_twice = Scratch() / ("project2_" + fixture.filename().string());
        AETHER_CHECK(SaveProject(saved_twice, again, &error));
        AETHER_CHECK(ReadText(saved) == ReadText(saved_twice));
    }
}
