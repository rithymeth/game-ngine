#include "aether/assets/asset_database.h"
#include "aether/blueprint/graph.h"
#include "aether/blueprint/system.h"
#include "aether/cook/cooker.h"
#include "aether/input/bindings.h"
#include "aether/player/game.h"
#include "aether/project/project.h"
#include "aether/scene/gameplay.h"
#include "aether/scene/serialization.h"
#include "aether/templates/templates.h"
#include "test_framework.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#ifndef AETHER_TEST_HAS_SCRIPTING
#define AETHER_TEST_HAS_SCRIPTING 0
#endif
#if AETHER_TEST_HAS_SCRIPTING
#include "aether/script/script_system.h"
#endif

// Phase 26 step 2: the project templates - what each creates, that it
// cooks and loads in the player, and (with scripting) that its controller
// really moves the player from the bindings it ships.

using namespace aether;
using namespace aether::templates;
namespace stdfs = std::filesystem;

#define CHECK(...) AETHER_CHECK((__VA_ARGS__))

namespace {

stdfs::path Dir(const std::string& name) {
    const stdfs::path dir = stdfs::temp_directory_path() / "aether_template_tests" / name;
    stdfs::remove_all(dir);
    stdfs::create_directories(dir);
    return dir;
}

ProjectPaths Create(const std::string& template_id, const std::string& name) {
    ProjectPaths paths;
    std::string error;
    const bool ok = CreateProjectFromTemplate(Dir(template_id), name, template_id, &paths, &error);
    if (!ok) std::printf("    creating the %s template: %s\n", template_id.c_str(), error.c_str());
    AETHER_CHECK(ok);
    return paths;
}

bool Near(f32 a, f32 b, f32 tolerance = 0.05f) { return std::fabs(a - b) <= tolerance; }

usize CountTagged(World& world, const std::string& tag) { return FindEntitiesWithTag(world, tag).size(); }

const std::vector<std::string> kPlayable = {"first_person", "third_person", "top_down", "vehicle"};

} // namespace

AETHER_TEST(Templates_ListsEveryTemplateAndWhatIsMissing) {
    const std::vector<ProjectTemplate>& all = ProjectTemplates();
    CHECK(all.size() == 6);
    for (const char* id : {"blank", "first_person", "third_person", "top_down", "vehicle", "platformer_2d"}) {
        const ProjectTemplate* t = FindProjectTemplate(id);
        CHECK(t != nullptr && !t->name.empty() && !t->genre.empty() && !t->description.empty() && !t->features.empty());
    }
    CHECK(FindProjectTemplate("nothing") == nullptr);
    const ProjectTemplate* platformer = FindProjectTemplate("platformer_2d");
    CHECK(!platformer->available && platformer->unavailable_reason.find("2D toolkit") != std::string::npos);
    for (const std::string& id : kPlayable) CHECK(FindProjectTemplate(id)->available);

    std::string error;
    const stdfs::path parent = Dir("Refusals");
    CHECK(!CreateProjectFromTemplate(parent, "A", "nothing", nullptr, &error) && error.find("no project template") != std::string::npos);
    CHECK(!CreateProjectFromTemplate(parent, "A", "platformer_2d", nullptr, &error) && error.find("isn't available") != std::string::npos);
    CHECK(!CreateProjectFromTemplate(parent, "bad/name", "blank", nullptr, &error) && error.find("valid project name") != std::string::npos);
    CHECK(CreateProjectFromTemplate(parent, "Fine", "blank", nullptr, &error));
    CHECK(!CreateProjectFromTemplate(parent, "Fine", "blank", nullptr, &error) && error.find("isn't empty") != std::string::npos);
}

AETHER_TEST(Templates_BlankIsAnEmptyLevelWithACamera) {
    const ProjectPaths paths = Create("blank", "Empty");
    ProjectSettings settings;
    std::string error;
    CHECK(LoadProject(paths.file, settings, &error) && settings.startup_scene == "Scenes/Main.ascene");
    CHECK(settings.always_cook.empty());
    CHECK(stdfs::exists(paths.content / "Scenes/Main.ascene") && !stdfs::exists(paths.content / "Scripts") &&
          !stdfs::exists(paths.content / "Input") && !stdfs::exists(paths.content / "Blueprints"));
    World world;
    CHECK(LoadSceneJson(world, (paths.content / "Scenes/Main.ascene").string()));
    CHECK(world.EntityCount() == 2 && CountTagged(world, "MainCamera") == 1 && CountTagged(world, "PlayerStart") == 1);
    const Entity camera = FindEntitiesWithTag(world, "MainCamera")[0];
    CHECK(world.HasComponent<Camera>(camera));
}

AETHER_TEST(Templates_EachCreatesAProjectThatCooksAndLoads) {
    bp::RegisterBlueprintComponents();
    for (const std::string& id : kPlayable) {
        const ProjectPaths paths = Create(id, "Game");
        ProjectSettings settings;
        std::string error;
        CHECK(LoadProject(paths.file, settings, &error));
        CHECK(settings.startup_scene == "Scenes/Main.ascene" && settings.always_cook == std::vector<std::string>{"Input/"});

        // The assets the scene refers to are in the database, under their GUIDs.
        assets::AssetDatabase db(paths.content);
        db.Scan();
        CHECK(db.FindByPath("Blueprints/BP_Pickup.abp") && db.FindByPath("Blueprints/BP_Pickup.abp")->importer == "Blueprint");
        CHECK(db.FindByPath("Input/Gameplay.amapping") && db.FindByPath("Input/Move.aaction"));
        const bool looks = id == "first_person" || id == "third_person";
        CHECK((db.FindByPath("Input/Look.aaction") != nullptr) == looks && (db.FindByPath("Input/Jump.aaction") != nullptr) == looks);
        input::InputAssetLibrary library;
        CHECK(library.Load(db) == 0 && library.FindContext("Gameplay") != nullptr);

        World world;
        CHECK(LoadSceneJson(world, (paths.content / "Scenes/Main.ascene").string()));
        CHECK(CountTagged(world, "Player") == 1 && CountTagged(world, "Pickup") >= 6);
        CHECK(CountTagged(world, "MainCamera") == 1);
        const Entity camera = FindEntitiesWithTag(world, "MainCamera")[0];
        CHECK(world.HasComponent<Camera>(camera));
        const Entity player = FindEntitiesWithTag(world, "Player")[0];
        CHECK(world.HasComponent<ScriptComponent>(player));
        for (const Entity pickup : FindEntitiesWithTag(world, "Pickup")) {
            CHECK(world.HasComponent<bp::BlueprintInstance>(pickup));
        }

        // Cooked, with everything the game needs, and the player loads it.
        cook::CookOptions options;
        options.project_file = paths.file;
        options.output_dir = paths.root / "Paks";
        options.configuration = cook::BuildConfiguration::Shipping;
        const cook::CookReport report = cook::Cook(options);
        CHECK(report.ok);
        std::vector<std::string> cooked;
        for (const cook::CookedAsset& a : report.assets) cooked.push_back(a.path);
        const auto has = [&](const std::string& p) { return std::find(cooked.begin(), cooked.end(), p) != cooked.end(); };
        CHECK(has("Scenes/Main.ascene") && has("Blueprints/BP_Pickup.abp") && has("Input/Gameplay.amapping"));
        CHECK(std::count_if(cooked.begin(), cooked.end(), [](const std::string& p) { return p.rfind("Scripts/", 0) == 0; }) == 1);
        player::GamePackage package;
        CHECK(package.Mount(report.pak_file.string()) && package.LoadManifest(&error));
        player::Game game(package);
        CHECK(game.LoadStartupScene(&error));
        CHECK(game.GetWorld().EntityCount() == world.EntityCount() && game.Warnings().empty());
    }
}

AETHER_TEST(Templates_PickupBlueprintCompiles) {
    // The Blueprint every playable template places.
    const ProjectPaths paths = Create("first_person", "Pickups");
    bp::Blueprint blueprint;
    std::string error;
    CHECK(bp::LoadBlueprint(paths.content / "Blueprints/BP_Pickup.abp", blueprint, &error));
    const bp::CompileResult compiled = bp::CompileBlueprint(blueprint);
    CHECK(compiled.Ok() && compiled.diagnostics.diagnostics.empty());
    const bp::Variable* speed = blueprint.FindVariable("SpinSpeed");
    CHECK(speed && (speed->flags & bp::Var_InstanceEditable) && std::get<f32>(speed->default_value) == 90.0f);
    CHECK(blueprint.FindVariable("Angle") != nullptr);
}

#if AETHER_TEST_HAS_SCRIPTING

namespace {

// A template's project, run: its scene, bindings and scripts, driven with
// simulated keys and mouse.
struct Run {
    ProjectPaths paths;
    std::unique_ptr<assets::AssetDatabase> db;
    World world;
    GuidIndex guids;
    script::LuauHost host;
    std::unique_ptr<Lifecycle> life;
    std::unique_ptr<script::ScriptSystem> scripts;
    input::InputSystem input;
    input::InputState state;
    input::InputAssetLibrary library;
    Entity player, camera;

    // The script system holds subscriptions on `input`, so it goes first.
    ~Run() {
        scripts.reset();
        life.reset();
    }

    explicit Run(const std::string& id) {
        paths = Create(id, "Run");
        db = std::make_unique<assets::AssetDatabase>(paths.content);
        db->Scan();
        AETHER_CHECK(LoadSceneJson(world, (paths.content / "Scenes/Main.ascene").string()));
        guids.Rebuild(world);
        life = std::make_unique<Lifecycle>(world, guids);
        scripts = std::make_unique<script::ScriptSystem>(host, world, guids, script::ScriptSystem::DatabaseLoader(*db));
        scripts->Register(*life);
        library.Load(*db);
        library.RegisterActions(input);
        AETHER_CHECK(library.Activate(input, "Gameplay", 0));
        scripts->BindInput(&input);
        player = FindEntitiesWithTag(world, "Player")[0];
        camera = FindEntitiesWithTag(world, "MainCamera")[0];
        life->BeginPlay();
        for (const std::string& e : scripts->Errors()) std::printf("    script error: %s\n", e.c_str());
        AETHER_CHECK(scripts->Errors().empty() && scripts->InstanceCount() == 1);
    }
    void Frames(int n, f32 dt = 1.0f / 60.0f) {
        for (int i = 0; i < n; ++i) {
            input.Update(state, dt);
            life->Update(dt);
            scripts->Tick(dt);
            state.EndFrame();
        }
        for (const std::string& e : scripts->Errors()) std::printf("    script error: %s\n", e.c_str());
        AETHER_CHECK(scripts->Errors().empty());
        scripts->ClearErrors();
    }
    Vec3 Position(Entity e) { return world.GetComponent<Transform>(e)->position; }
    Quaternion Rotation(Entity e) { return world.GetComponent<Transform>(e)->rotation; }
    f64 Field(const char* name) {
        const script::ScriptValue v = scripts->GetField(player, name);
        return std::holds_alternative<f64>(v) ? std::get<f64>(v) : -999.0;
    }
};

} // namespace

AETHER_TEST(Templates_FirstPersonWalksLooksAndJumps) {
    Run r("first_person");
    const Vec3 start = r.Position(r.player);
    CHECK(Near(start.y, 1.7f) && Near(start.x, 0.0f));

    r.state.SetButton(input::Key::W, true);
    r.Frames(60);
    Vec3 p = r.Position(r.player);
    CHECK(Near(p.z, -5.0f, 0.1f) && Near(p.x, 0.0f) && Near(p.y, 1.7f)); // a second at 5 m/s, forward is -Z
    r.state.SetButton(input::Key::W, false);

    // Diagonals aren't faster than straight lines.
    r.state.SetButton(input::Key::W, true);
    r.state.SetButton(input::Key::D, true);
    const Vec3 before = r.Position(r.player);
    r.Frames(60);
    p = r.Position(r.player);
    CHECK(Near(std::hypot(p.x - before.x, p.z - before.z), 5.0f, 0.15f) && p.x > before.x);
    r.state.SetButton(input::Key::W, false);
    r.state.SetButton(input::Key::D, false);

    // Looking right (mouse +X) turns the heading clockwise: forward gains +X.
    r.state.AddMouseDelta(500, 0);
    r.Frames(1);
    CHECK(r.Rotation(r.player).y < -0.2f && r.Field("yaw") < 0.0);
    const Vec3 turned = r.Position(r.player);
    r.state.SetButton(input::Key::W, true);
    r.Frames(30);
    CHECK(r.Position(r.player).x > turned.x + 1.0f);
    r.state.SetButton(input::Key::W, false);
    // Looking down tilts the camera: the rotation gains an X part.
    r.state.AddMouseDelta(0, 300);
    r.Frames(1);
    CHECK(r.Rotation(r.player).x < -0.1f);

    // A jump leaves the floor and comes back to it.
    r.state.SetButton(input::Key::Space, true);
    r.Frames(1);
    r.state.SetButton(input::Key::Space, false);
    f32 peak = 0.0f;
    for (int i = 0; i < 90; ++i) {
        r.Frames(1);
        peak = std::max(peak, r.Position(r.player).y);
    }
    CHECK(peak > 1.7f + 0.5f && Near(r.Position(r.player).y, 1.7f, 0.001f));
}

AETHER_TEST(Templates_ThirdPersonOrbitsAndFacesTheWayItRuns) {
    Run r("third_person");
    r.Frames(1);
    // The camera sits behind and above the character, looking at it.
    Vec3 c = r.Position(r.camera);
    Vec3 p = r.Position(r.player);
    CHECK(Near(c.x, p.x) && c.z > p.z + 4.0f && c.y > p.y + 2.0f);

    r.state.SetButton(input::Key::W, true);
    r.Frames(60);
    p = r.Position(r.player);
    CHECK(Near(p.z, -6.0f, 0.15f) && Near(p.x, 0.0f) && Near(r.Rotation(r.player).y, 0.0f));
    CHECK(r.Position(r.camera).z > p.z + 4.0f); // it followed
    r.state.SetButton(input::Key::W, false);

    // Running right turns it to face +X (a quarter turn, clockwise from above).
    r.state.SetButton(input::Key::D, true);
    r.Frames(30);
    CHECK(r.Position(r.player).x > 2.5f && Near(r.Rotation(r.player).y, -std::sqrt(0.5f), 0.02f));
    r.state.SetButton(input::Key::D, false);

    // Orbiting the camera changes what "forward" is: turning it a quarter
    // right (mouse), W now runs towards +X.
    const f32 look = 90.0f / 0.12f;
    r.state.AddMouseDelta(look, 0);
    r.Frames(1);
    const Vec3 before = r.Position(r.player);
    r.state.SetButton(input::Key::W, true);
    r.Frames(30);
    const Vec3 after = r.Position(r.player);
    CHECK(after.x - before.x > 2.5f && Near(after.z, before.z, 0.3f));
    // The camera stays the same distance away.
    const Vec3 cam = r.Position(r.camera);
    const Vec3 target = Vec3(after.x, after.y + 1.6f, after.z);
    CHECK(Near(std::sqrt((cam.x - target.x) * (cam.x - target.x) + (cam.y - target.y) * (cam.y - target.y) +
                         (cam.z - target.z) * (cam.z - target.z)), 6.0f, 0.1f));
}

AETHER_TEST(Templates_TopDownMovesOnTheMapAndTheCameraFollows) {
    Run r("top_down");
    r.state.SetButton(input::Key::W, true);
    r.Frames(60);
    Vec3 p = r.Position(r.player);
    CHECK(Near(p.z, -6.0f, 0.15f) && Near(p.x, 0.0f)); // up the screen is -Z
    r.state.SetButton(input::Key::W, false);
    r.state.SetButton(input::Key::D, true);
    r.Frames(60);
    p = r.Position(r.player);
    CHECK(Near(p.x, 6.0f, 0.15f) && Near(r.Rotation(r.player).y, -std::sqrt(0.5f), 0.02f)); // facing +X
    r.state.SetButton(input::Key::D, false);
    // The camera catches up: above and behind (+Z), the tilt looking down at it.
    r.Frames(240);
    const Vec3 cam = r.Position(r.camera);
    const f32 tilt = 65.0f * 3.14159265f / 180.0f;
    CHECK(Near(cam.x, p.x, 0.05f) && Near(cam.y, p.y + std::sin(tilt) * 18.0f, 0.05f) &&
          Near(cam.z, p.z + std::cos(tilt) * 18.0f, 0.05f));
    CHECK(r.Rotation(r.camera).x < -0.4f && Near(r.Rotation(r.camera).y, 0.0f));
}

AETHER_TEST(Templates_VehicleAcceleratesSteersAndReverses) {
    Run r("vehicle");
    // Standing still, steering does nothing.
    r.state.SetButton(input::Key::D, true);
    r.Frames(30);
    CHECK(Near(r.Field("heading"), 0.0f, 0.001f) && Near(r.Position(r.player).x, 0.0f, 0.001f));
    r.state.SetButton(input::Key::D, false);

    r.state.SetButton(input::Key::W, true);
    r.Frames(60);
    CHECK(Near(static_cast<f32>(r.Field("speed")), 14.0f, 0.3f)); // a second at 14 m/s^2
    CHECK(Near(r.Position(r.player).z, -7.0f, 0.4f));
    // Held on, it tops out.
    r.Frames(240);
    CHECK(Near(static_cast<f32>(r.Field("speed")), 28.0f, 0.001f));

    // Steering right at speed turns it clockwise: heading goes negative, and it drifts to +X.
    r.state.SetButton(input::Key::D, true);
    const f32 x0 = r.Position(r.player).x;
    r.Frames(60);
    CHECK(r.Field("heading") < -1.0 && r.Position(r.player).x > x0 + 5.0f);
    r.state.SetButton(input::Key::D, false);
    r.state.SetButton(input::Key::W, false);

    // Brakes stop it, then it reverses (and no further than the limit).
    r.state.SetButton(input::Key::S, true);
    r.Frames(30);
    CHECK(r.Field("speed") < 14.0); // braking at 30 m/s^2
    r.Frames(300);
    CHECK(Near(static_cast<f32>(r.Field("speed")), -8.0f, 0.001f));
    // The chase camera trails the vehicle by about its set distance.
    const Vec3 v = r.Position(r.player), cam = r.Position(r.camera);
    CHECK(Near(std::sqrt((cam.x - v.x) * (cam.x - v.x) + (cam.z - v.z) * (cam.z - v.z)), 9.0f, 0.1f) && Near(cam.y - v.y, 3.5f, 0.01f));
}

AETHER_TEST(Templates_PlacedPickupsSpinInTheScene) {
    Run r("first_person");
    bp::BlueprintSystem blueprints(r.world);
    blueprints.SetLoader([&](const assets::AssetGuid& guid, bp::Blueprint& out, std::string& name) {
        name = "BP_Pickup";
        return bp::LoadBlueprint(r.db->SourcePath(guid), out);
    });
    blueprints.Register(*r.life);
    r.life->EndPlay();
    r.life->BeginPlay();
    CHECK(blueprints.CompileErrors().empty());
    const Entity pickup = FindEntitiesWithTag(r.world, "Pickup")[0];
    const Quaternion before = r.Rotation(pickup);
    CHECK(before.y == 0.0f);
    for (int i = 0; i < 60; ++i) {
        r.life->Update(1.0f / 60.0f);
        blueprints.Update(1.0f / 60.0f);
    }
    // A second at 90 degrees a second is a quarter turn about +Y.
    CHECK(Near(r.Rotation(pickup).y, std::sin(3.14159265f / 4.0f), 0.02f) && Near(r.Rotation(pickup).w, std::cos(3.14159265f / 4.0f), 0.02f));
}

#endif // AETHER_TEST_HAS_SCRIPTING
