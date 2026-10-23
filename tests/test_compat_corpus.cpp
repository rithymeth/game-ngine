// M8.1 compatibility corpus: scripts written against the engine 0.27 script API (tests/compat/luau) must keep
// loading and running. Data formats are covered by tests/fixtures/legacy (test_legacy_files.cpp) and the C++
// SDK surface by samples/frozen_api_game. Do not edit a corpus file to make it pass after an API change: keep
// the old script working (or record a reviewed break in docs/manual/core_api_inventory.md).

#include "test_framework.h"

#if AETHER_TEST_HAS_SCRIPTING

#include "aether/scene/components.h"
#include "aether/scene/lifecycle.h"
#include "aether/script/script_system.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <variant>
#include <vector>

using namespace aether;
using namespace aether::script;
namespace stdfs = std::filesystem;

namespace {

stdfs::path CorpusRoot() { return stdfs::path(AETHER_REPO_ASSETS_DIR).parent_path() / "tests/compat/luau"; }

std::vector<stdfs::path> Scripts(const stdfs::path& dir) {
    std::vector<stdfs::path> out;
    std::error_code ec;
    for (const auto& entry : stdfs::directory_iterator(dir, ec))
        if (entry.is_regular_file() && entry.path().extension() == ".luau") out.push_back(entry.path());
    std::sort(out.begin(), out.end());
    return out;
}

std::string Read(const stdfs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream s;
    s << in.rdbuf();
    return s.str();
}

bool True(const ScriptResult& r) { return r.ok && !r.values.empty() && std::holds_alternative<bool>(r.values[0]) && std::get<bool>(r.values[0]); }

} // namespace

AETHER_TEST(CompatCorpus_TopLevelScriptsRun) {
    (void)GetComponentId<ScriptComponent>();
    (void)GetComponentId<Transform>();
    const auto scripts = Scripts(CorpusRoot());
    AETHER_CHECK(scripts.size() >= 4);
    for (const auto& path : scripts) {
        World world;
        GuidIndex guids;
        LuauHost host;
        ScriptSystem system(host, world, guids, [](const assets::AssetGuid&, std::string&, std::string&) { return false; }, true);
        host.BindWorld(&world, &guids);
        const std::string name = path.filename().string();
        ScriptResult r = host.Run(Read(path), name);
        if (!True(r)) std::printf("  corpus script %s failed: %s\n", name.c_str(), r.error.c_str());
        AETHER_CHECK(True(r));
        for (int i = 0; i < 10; ++i) system.Tick(0.1f);
        ScriptResult c = host.Run("if CompatCheck == nil then return true end return CompatCheck()", "compat_check");
        if (!True(c)) std::printf("  corpus script %s CompatCheck failed: %s\n", name.c_str(), c.error.c_str());
        AETHER_CHECK(True(c));
        AETHER_CHECK(system.Errors().empty());
    }
}

AETHER_TEST(CompatCorpus_EntityScriptsRun) {
    (void)GetComponentId<ScriptComponent>();
    (void)GetComponentId<Transform>();
    const auto scripts = Scripts(CorpusRoot() / "entity");
    AETHER_CHECK(!scripts.empty());
    for (const auto& path : scripts) {
        World world;
        GuidIndex guids;
        LuauHost host;
        Lifecycle life(world, guids);
        const assets::AssetGuid guid = assets::NewAssetGuid();
        const std::string source = Read(path);
        const std::string name = path.filename().string();
        ScriptSystem system(host, world, guids, [&](const assets::AssetGuid& g, std::string& src, std::string& n) {
            if (g != guid) return false;
            src = source;
            n = name;
            return true;
        }, true);
        system.Register(life);
        ScriptComponent component;
        component.script.guid = guid;
        Entity e = world.CreateEntity(IdComponent{NewEntityGuid()}, std::move(component));
        guids.Add(world.GetComponent<IdComponent>(e)->guid, e);
        life.BeginPlay();
        for (int i = 0; i < 2; ++i) {
            life.Update(0.5f);
            system.Tick(0.5f);
        }
        system.SendEvent(e, "OnHit", {ScriptValue{3.0}});
        system.SendEvent(e, "CompatCheck");
        const ScriptValue ok = system.GetField(e, "compat_ok");
        const bool passed = std::holds_alternative<bool>(ok) && std::get<bool>(ok);
        if (!passed) {
            std::printf("  corpus entity script %s failed\n", name.c_str());
            for (const auto& err : system.Errors()) std::printf("    %s\n", err.c_str());
        }
        AETHER_CHECK(passed);
        AETHER_CHECK(system.Errors().empty());
        life.EndPlay();
    }
}

#endif
