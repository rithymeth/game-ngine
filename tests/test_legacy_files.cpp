#include "test_framework.h"

#include "aether/assets/asset_database.h"
#include "aether/blueprint/compiler.h"
#include "aether/blueprint/graph.h"
#include "aether/ecs/world.h"
#include "aether/input/bindings.h"
#include "aether/platform/filesystem.h"
#include "aether/plugin/plugin.h"
#include "aether/project/project.h"
#include "aether/scene/serialization.h"
#include "aether/sequencer/sequence.h"
#include "aether/ui/layout_file.h"

#include <filesystem>
#include <string>
#include <vector>

// Serialization migrations (Phase 48 step 2): files that earlier tools wrote must still load. The fixtures under
// tests/fixtures/legacy/ are real scenes, Blueprints, input assets, layouts, plugin descriptors, projects and asset
// metadata copied from the games and plugins; every one is loaded with the current loader.

using namespace aether;
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

std::string Read(const stdfs::path& p) {
    std::vector<u8> bytes;
    AETHER_CHECK(fs::ReadFileBytes(p.string(), bytes));
    return std::string(bytes.begin(), bytes.end());
}

std::vector<u8> Bytes(const std::string& text) { return std::vector<u8>(text.begin(), text.end()); }

} // namespace

AETHER_TEST(LegacyFiles_FixturesExist) {
    for (const char* kind : {"scene", "blueprint", "sequence", "input_action", "input_mapping", "layout", "plugin", "project", "assetmeta"})
        AETHER_CHECK(!Files(kind).empty());
}

AETHER_TEST(LegacyFiles_ScenesLoad) {
    for (const auto& p : Files("scene")) {
        World world;
        const std::vector<u8> bytes = Bytes(Read(p));
        AETHER_CHECK(LoadSceneJsonFromMemory(world, bytes, p.filename().string()));
        AETHER_CHECK(world.EntityCount() > 0);
    }
}

AETHER_TEST(LegacyFiles_BlueprintsLoadAndCompile) {
    for (const auto& p : Files("blueprint")) {
        const nlohmann::json doc = nlohmann::json::parse(Read(p), nullptr, false);
        bp::Blueprint blueprint;
        std::string error;
        AETHER_CHECK(bp::BlueprintFromJson(doc, blueprint, &error));
        AETHER_CHECK(bp::CompileBlueprint(blueprint).Ok());
    }
}

AETHER_TEST(LegacyFiles_SequencesLoad) {
    for (const auto& p : Files("sequence")) {
        seq::LevelSequence sequence;
        std::string error;
        AETHER_CHECK(seq::SequenceFromJson(nlohmann::json::parse(Read(p), nullptr, false), sequence, &error));
    }
}

AETHER_TEST(LegacyFiles_InputAssetsLoad) {
    input::InputAssetLibrary library;
    for (const auto& p : Files("input_action")) {
        std::string error;
        AETHER_CHECK(library.AddFromText("InputAction", p.string(), Read(p), &error));
    }
    for (const auto& p : Files("input_mapping")) {
        std::string error;
        AETHER_CHECK(library.AddFromText("InputMapping", p.string(), Read(p), &error));
    }
}

AETHER_TEST(LegacyFiles_LayoutsLoad) {
    for (const auto& p : Files("layout")) {
        ui::LayoutDocument document;
        std::string error;
        AETHER_CHECK(ui::LoadLayout(Read(p), document, {}, &error));
    }
}

AETHER_TEST(LegacyFiles_PluginDescriptorsLoadAndMatchTheEngineAbi) {
    for (const auto& p : Files("plugin")) {
        plugin::PluginDescriptor descriptor;
        std::string error;
        const std::vector<u8> bytes = Bytes(Read(p));
        AETHER_CHECK(plugin::LoadPluginDescriptorFromMemory(bytes, p.filename().string(), descriptor, &error));
        AETHER_CHECK(descriptor.abi == 0 || descriptor.abi == plugin::kPluginAbi); // 0: written before the field existed
    }
}

AETHER_TEST(LegacyFiles_ProjectsLoad) {
    for (const auto& p : Files("project")) {
        ProjectSettings settings;
        std::string error;
        AETHER_CHECK(LoadProject(p, settings, &error));
    }
}

AETHER_TEST(LegacyFiles_AssetMetadataLoads) {
    for (const auto& p : Files("assetmeta")) {
        assets::AssetMeta meta;
        std::string error;
        const std::vector<u8> bytes = Bytes(Read(p));
        AETHER_CHECK(assets::LoadAssetMetaFromMemory(bytes, meta, &error));
    }
}
