#include "aether/scene/components.h"
#include "aether/script/script_system.h"
#include "test_framework.h"

#include <chrono>
#include <filesystem>
#include <fstream>

using namespace aether;
using namespace aether::script;
namespace stdfs = std::filesystem;
using namespace std::chrono_literals;

namespace {

const char* kV1 = R"(
local Mover = {}
Mover.speed = 1
Mover.tag = "v1"
function Mover:OnStart() self.moved = 0; self.reloads = 0 end
function Mover:OnUpdate(dt) self.moved += self.speed * dt end
function Mover:Version() return 1 end
return Mover
)";

const char* kV2 = R"(
local Mover = {}
Mover.speed = 10          -- a new default
Mover.tag = "v2"
function Mover:OnStart() self.moved = 0; self.reloads = 0 end
function Mover:OnUpdate(dt) self.moved += self.speed * dt * 2 end
function Mover:Version() return 2 end
function Mover:OnReload() self.reloads += 1 end
return Mover
)";

const char* kBrokenSyntax = "local Mover = {\nfunction Mover:OnUpdate(dt) end\nreturn Mover\n";

f64 Number(const ScriptValue& v) { return std::holds_alternative<f64>(v) ? std::get<f64>(v) : -1.0; }

struct Project {
    stdfs::path dir = stdfs::temp_directory_path() / "aether_test_script_reload";
    std::unique_ptr<assets::AssetDatabase> db;
    World world;
    GuidIndex guids;
    LuauHost host;

    Project() {
        stdfs::remove_all(dir);
        stdfs::create_directories(dir / "Content/Scripts");
        Write(kV1, 0);
        db = std::make_unique<assets::AssetDatabase>(dir / "Content");
        db->Scan();
        (void)GetComponentId<ScriptComponent>();
    }
    ~Project() { stdfs::remove_all(dir); }
    void Write(const char* source, int stamp) {
        const stdfs::path file = dir / "Content/Scripts/Mover.luau";
        std::ofstream(file, std::ios::binary | std::ios::trunc) << source;
        stdfs::last_write_time(file, stdfs::file_time_type::clock::now() + std::chrono::seconds(stamp));
    }
    assets::AssetGuid Script() { return db->FindByPath("Scripts/Mover.luau")->guid; }
    Entity Make(std::vector<ScriptProperty> properties = {}) {
        ScriptComponent component;
        component.script.guid = Script();
        component.properties = std::move(properties);
        Entity e = world.CreateEntity(IdComponent{NewEntityGuid()}, std::move(component));
        guids.Add(world.GetComponent<IdComponent>(e)->guid, e);
        return e;
    }
};

} // namespace

AETHER_TEST(ScriptReload_KeepsStateAndSwapsCode) {
    Project p;
    Lifecycle life(p.world, p.guids);
    ScriptSystem scripts(p.host, p.world, p.guids, ScriptSystem::DatabaseLoader(*p.db));
    scripts.Register(life);
    Entity plain = p.Make();
    Entity custom = p.Make({{"speed", "3"}}); // overrides the default
    life.BeginPlay();
    life.Update(1.0f);
    AETHER_CHECK(Number(scripts.GetField(plain, "moved")) == 1.0 && Number(scripts.GetField(custom, "moved")) == 3.0);

    // A broken edit: the old code keeps running, and the error is kept for the overlay.
    p.Write(kBrokenSyntax, 10);
    ScriptSystem::ReloadResult failed = scripts.Reload(p.Script());
    AETHER_CHECK(!failed.ok && scripts.ReloadErrors().count(p.Script()) == 1);
    const ScriptError& error = scripts.ReloadErrors().at(p.Script());
    AETHER_CHECK(error.chunk == "Mover.luau" && error.line > 0 && !error.message.empty());
    life.Update(1.0f);
    AETHER_CHECK(Number(scripts.GetField(plain, "moved")) == 2.0); // still v1

    // The fix: state kept, new methods, new defaults where not overridden, OnReload called.
    p.Write(kV2, 20);
    ScriptSystem::ReloadResult fixed = scripts.Reload(p.Script());
    AETHER_CHECK(fixed.ok && fixed.instances == 2 && scripts.ReloadErrors().empty());
    AETHER_CHECK(Number(scripts.GetField(plain, "moved")) == 2.0);   // state carried over
    AETHER_CHECK(Number(scripts.GetField(plain, "reloads")) == 1.0); // OnReload ran
    AETHER_CHECK(Number(scripts.CallMethod(plain, "Version").values[0]) == 2.0);
    AETHER_CHECK(Number(scripts.GetField(plain, "speed")) == 10.0);  // the new default
    AETHER_CHECK(Number(scripts.GetField(custom, "speed")) == 3.0);  // its own override stays
    AETHER_CHECK(scripts.ClassInfo(p.Script()).Find("tag")->default_value == "v2");
    life.Update(1.0f);
    AETHER_CHECK(Number(scripts.GetField(plain, "moved")) == 22.0); // 2 + 10 * 1 * 2
    AETHER_CHECK(Number(scripts.GetField(custom, "moved")) == 12.0); // 6 + 3 * 1 * 2
    life.EndPlay();
}

AETHER_TEST(ScriptReload_FromFileChangesAndBrokenAtStart) {
    Project p;
    p.Write(kBrokenSyntax, 5); // broken before play even starts
    p.db->Scan();
    Lifecycle life(p.world, p.guids);
    ScriptSystem scripts(p.host, p.world, p.guids, ScriptSystem::DatabaseLoader(*p.db));
    scripts.Register(life);
    Entity e = p.Make();
    life.BeginPlay();
    AETHER_CHECK(scripts.InstanceCount() == 0 && !scripts.Errors().empty());

    // The hot reloader sees the fix; the entity starts running with it.
    assets::ImporterRegistry importers = assets::ImporterRegistry::WithBuiltins();
    assets::DerivedDataCache cache(p.dir / "Intermediate/DDC");
    assets::HotReloader reloader(*p.db, importers, cache, 200ms);
    auto t = assets::FileWatcher::Clock::now();
    reloader.Update(t);
    p.Write(kV1, 30);
    reloader.Update(t += 10ms);
    std::vector<assets::AssetChange> changes = reloader.Update(t += 300ms);
    AETHER_CHECK(changes.size() == 1 && changes[0].kind == assets::AssetChange::Kind::Changed);
    AETHER_CHECK(scripts.ReloadChanged(changes) == 1);
    AETHER_CHECK(scripts.InstanceCount() == 1 && Number(scripts.GetField(e, "moved")) == 0.0); // OnStart ran
    life.Update(0.5f);
    AETHER_CHECK(Number(scripts.GetField(e, "moved")) == 0.5);

    // Changes to scripts nobody uses, and non-scripts, are skipped.
    assets::AssetChange other;
    other.kind = assets::AssetChange::Kind::Changed;
    other.guid = assets::NewAssetGuid();
    other.path = "Scripts/Unused.luau";
    assets::AssetChange texture = other;
    texture.path = "Textures/a.png";
    AETHER_CHECK(scripts.ReloadChanged({other, texture}) == 0);
    AETHER_CHECK(!scripts.Reload(assets::NewAssetGuid()).ok); // unknown script
    life.EndPlay();

    // Error text parsing, including paths with colons.
    ScriptError parsed = ScriptError::Parse("C:/game/Mover.luau:12: attempt to index nil");
    AETHER_CHECK(parsed.chunk == "C:/game/Mover.luau" && parsed.line == 12 && parsed.message == "attempt to index nil");
    ScriptError plain = ScriptError::Parse("not enough memory");
    AETHER_CHECK(plain.chunk.empty() && plain.line == 0 && plain.message == "not enough memory");
}
