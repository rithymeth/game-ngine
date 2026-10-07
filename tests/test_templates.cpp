#include "aether/assets/asset_database.h"
#include "aether/blueprint/graph.h"
#include "aether/blueprint/system.h"
#include "aether/cook/cooker.h"
#include "aether/input/bindings.h"
#include "aether/platform/window.h"
#include "aether/player/game.h"
#include "aether/project/project.h"
#include "aether/scene/gameplay.h"
#include "aether/scene/serialization.h"
#include "aether/sprite2d/components.h"
#include "aether/sprite2d/physics2d.h"
#include "aether/sprite2d/platformer.h"
#include "aether/sprite2d/tilemap.h"
#include "aether/templates/templates.h"
#if AETHER_TEST_HAS_PHYSICS
#include "aether/job/job_system.h"
#include "aether/physics/character.h"
#include "aether/physics/components.h"
#include "aether/physics/physics_scene.h"
#endif
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
#ifndef AETHER_TEST_HAS_PHYSICS
#define AETHER_TEST_HAS_PHYSICS 0
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
    for (const ProjectTemplate& t : all) CHECK(t.available);
    for (const char* id : {"blank", "first_person", "third_person", "top_down", "vehicle", "platformer_2d"}) {
        const ProjectTemplate* t = FindProjectTemplate(id);
        CHECK(t != nullptr && !t->name.empty() && !t->genre.empty() && !t->description.empty() && !t->features.empty());
    }
    CHECK(FindProjectTemplate("nothing") == nullptr);
    for (const std::string& id : kPlayable) CHECK(FindProjectTemplate(id)->available);

    std::string error;
    const stdfs::path parent = Dir("Refusals");
    CHECK(!CreateProjectFromTemplate(parent, "A", "nothing", nullptr, &error) && error.find("no project template") != std::string::npos);
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

AETHER_TEST(Templates_InputAssetsLoadFromText) {
    // What the player does with cooked bindings: no database, no files.
    const ProjectPaths paths = Create("third_person", "Bindings");
    input::InputAssetLibrary library;
    const auto read = [&](const char* rel) {
        std::ifstream in(paths.content / rel, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    };
    std::string error;
    CHECK(library.AddFromText("InputAction", "Input/Move.aaction", read("Input/Move.aaction"), &error));
    CHECK(library.AddFromText("InputMapping", "Input/Gameplay.amapping", read("Input/Gameplay.amapping"), &error));
    CHECK(library.Actions().size() == 1 && library.FindContext("Gameplay") != nullptr);
    CHECK(!library.AddFromText("InputAction", "Input/Bad.aaction", "{ not json", &error) && error.find("isn't InputAction") != std::string::npos);
    CHECK(!library.AddFromText("InputAction", "Input/Wrong.aaction", read("Input/Gameplay.amapping"), &error));
    CHECK(!library.AddFromText("Texture", "a.png", "x", &error) && error.find("isn't an input asset") != std::string::npos);
    CHECK(library.Errors().size() == 2);
    input::InputSystem system;
    library.RegisterActions(system);
    CHECK(library.Activate(system, "Gameplay", 0));
    input::InputState state;
    state.SetButton(input::Key::W, true);
    system.Update(state, 1.0f / 60.0f);
    CHECK(Near(system.GetAxis2D("Move").y, 1.0f, 0.001f));
    // The gamepad: a stick pushed forward is negative Y.
    input::InputState pad;
    pad.SetAxis(input::Key::GamepadLeftStickY, -1.0f);
    system.Update(pad, 1.0f / 60.0f);
    CHECK(Near(system.GetAxis2D("Move").y, 1.0f, 0.001f));
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

namespace {

// The 2D Platformer, cooked and played in the player.
struct PlatformerRun {
    ProjectPaths paths;
    player::GamePackage package;
    std::unique_ptr<player::Game> game;
    Entity hero, camera;

    PlatformerRun() {
        // Each run its own folder: a mounted pak is read as the game plays, so another run must not rewrite it.
        static int counter = 0;
        std::string error;
        AETHER_CHECK(CreateProjectFromTemplate(Dir("platformer_2d_" + std::to_string(counter++)), "Run2D", "platformer_2d", &paths, &error));
        cook::CookOptions options;
        options.project_file = paths.file;
        options.output_dir = paths.root / "Paks";
        options.configuration = cook::BuildConfiguration::Shipping;
        const cook::CookReport report = cook::Cook(options);
        AETHER_CHECK(report.ok);
        AETHER_CHECK(package.Mount(report.pak_file.string()) && package.LoadManifest(&error));
        game = std::make_unique<player::Game>(package);
        AETHER_CHECK(game->LoadStartupScene(&error));
        hero = FindEntitiesWithTag(game->GetWorld(), "Player")[0];
        camera = FindEntitiesWithTag(game->GetWorld(), "MainCamera")[0];
        game->BeginPlay();
    }
    ~PlatformerRun() = default;
    void Frames(int n) {
        for (int i = 0; i < n; ++i) game->Tick(1.0f / 60.0f);
    }
    void Hold(input::Key key, bool down) { game->Input().SetButton(key, down); }
    Vec3 Position() { return game->GetWorld().GetComponent<Transform>(hero)->position; }
    Vec3 Velocity() {
        const sprite2d::Vec2 v = game->GetWorld().GetComponent<sprite2d::Rigidbody2D>(hero)->velocity;
        return Vec3(v.x, v.y, 0);
    }
    bool Grounded() { return game->Physics2D()->IsGrounded(hero); }
    // Holds Space for `frames` frames from the ground and returns the highest the hero got.
    f32 JumpPeak(int frames) {
        Hold(input::Key::Space, true);
        f32 peak = Position().y;
        for (int i = 0; i < 90; ++i) {
            if (i == frames) Hold(input::Key::Space, false);
            Frames(1);
            peak = std::max(peak, Position().y);
        }
        Hold(input::Key::Space, false);
        return peak;
    }
};

} // namespace

AETHER_TEST(Templates_PlatformerIsACompleteTilemapGame) {
    const ProjectPaths paths = Create("platformer_2d", "Tiles");
    ProjectSettings settings;
    std::string error;
    CHECK(LoadProject(paths.file, settings, &error));
    CHECK(settings.startup_scene == "Scenes/Main.ascene" && settings.always_cook == std::vector<std::string>{"Input/"});
    assets::AssetDatabase db(paths.content);
    db.Scan();
    CHECK(db.FindByPath("Tiles/Level.atileset") && db.FindByPath("Tiles/Level.atileset")->importer == "Tileset");
    CHECK(db.FindByPath("Tiles/Level1.atilemap") && db.FindByPath("Tiles/Level1.atilemap")->importer == "Tilemap");
    CHECK(db.FindByPath("Input/Move.aaction") && db.FindByPath("Input/Jump.aaction") && db.FindByPath("Input/Gameplay.amapping"));
    CHECK(!db.FindByPath("Input/Look.aaction") && !stdfs::exists(paths.content / "Scripts") && !stdfs::exists(paths.content / "Blueprints"));
    // The tilemap names its tileset, which the database sees as a dependency.
    sprite2d::TilemapData map;
    CHECK(sprite2d::LoadTilemap(paths.content / "Tiles/Level1.atilemap", map, &error));
    CHECK(map.tileset == db.FindByPath("Tiles/Level.atileset")->guid && map.width == 48 && map.layers.size() == 1);
    CHECK(!db.Referencers(db.FindByPath("Tiles/Level.atileset")->guid).empty());
    CHECK(!db.Referencers(db.FindByPath("Tiles/Level1.atilemap")->guid).empty()); // the scene

    sprite2d::RegisterSprite2DComponents();
    sprite2d::RegisterPhysics2DComponents();
    sprite2d::RegisterPlatformerComponents();
    World world;
    CHECK(LoadSceneJson(world, (paths.content / "Scenes/Main.ascene").string()));
    CHECK(CountTagged(world, "Player") == 1 && CountTagged(world, "Level") == 1 && CountTagged(world, "MainCamera") == 1);
    const Entity hero = FindEntitiesWithTag(world, "Player")[0];
    CHECK(world.HasComponent<sprite2d::Rigidbody2D>(hero) && world.HasComponent<sprite2d::Collider2D>(hero) &&
          world.HasComponent<sprite2d::PlatformerController2D>(hero));
    const Entity camera = FindEntitiesWithTag(world, "MainCamera")[0];
    CHECK(world.GetComponent<Camera>(camera)->projection == Projection::Orthographic);
    CHECK(Near(world.GetComponent<Camera>(camera)->ortho_height, 11.25f) && world.HasComponent<sprite2d::CameraFollow2D>(camera));
    CHECK(FindProjectTemplate("platformer_2d")->available);
}

AETHER_TEST(Templates_PlatformerRunsJumpsAndFalls) {
    PlatformerRun r;
    // It lands on the ground (its top is y = 2, the hero's half height 0.5 above).
    r.Frames(90);
    CHECK(Near(r.Position().y, 2.5f, 0.05f) && r.Grounded());
    CHECK(Near(r.Position().x, 3.0f, 0.05f)); // no input, no drift
    const Vec3 camera_start = r.game->GetWorld().GetComponent<Transform>(r.camera)->position;

    // Running: acceleration to full speed, and the camera follows.
    r.Hold(input::Key::D, true);
    r.Frames(30);
    CHECK(r.Velocity().x > 5.5f && r.Position().x > 5.5f && r.Grounded());
    r.Hold(input::Key::D, false);
    r.Frames(40);
    CHECK(std::fabs(r.Velocity().x) < 0.1f); // it stops
    const Vec3 camera_now = r.game->GetWorld().GetComponent<Transform>(r.camera)->position;
    CHECK(camera_now.x > camera_start.x - 0.001f && Near(camera_now.z, camera_start.z, 0.001f));

    // Running left against the map's left edge isn't a wall here: it runs to the edge of the ground only.
    // A held jump rises higher than a tapped one.
    const f32 floor_y = r.Position().y;
    PlatformerRun full, tap;
    full.Frames(90);
    tap.Frames(90);
    const f32 high = full.JumpPeak(60);
    const f32 low = tap.JumpPeak(4);
    CHECK(high > floor_y + 5.0f);
    CHECK(low > floor_y + 0.5f && low < high - 2.0f);
    full.Frames(60);
    CHECK(full.Grounded() && Near(full.Position().y, 2.5f, 0.05f)); // it comes back down

    // Into the pit: running right without jumping, it falls through the gap and keeps falling.
    r.Hold(input::Key::D, true);
    r.Frames(240);
    CHECK(r.Position().y < -5.0f);
    CHECK(r.Velocity().y < -5.0f);
}

AETHER_TEST(Templates_PlatformerStopsAtTheStepAndFacesWhereItRuns) {
    PlatformerRun r;
    r.Frames(90);
    // A short hop onto the first platform's level isn't needed: run right then jump over the pit to the far side.
    r.Hold(input::Key::D, true);
    r.Frames(15);
    CHECK(r.game->GetWorld().GetComponent<sprite2d::PlatformerController2D>(r.hero)->facing == 1);
    r.Hold(input::Key::D, false);
    r.Hold(input::Key::A, true);
    r.Frames(10);
    CHECK(r.game->GetWorld().GetComponent<sprite2d::PlatformerController2D>(r.hero)->facing == -1);
    CHECK(r.Velocity().x < -2.0f);
    r.Hold(input::Key::A, false);
    // Running at the tall block at the end of the map needs the pit crossed: jump it at full speed.
    r.Frames(30);
    r.Hold(input::Key::D, true);
    bool crossed = false;
    for (int i = 0; i < 300 && !crossed; ++i) {
        r.Frames(1);
        const Vec3 p = r.Position();
        if (r.Grounded() && p.x > 17.0f && p.x < 19.0f && !r.game->Input().IsDown(input::Key::Space)) r.Hold(input::Key::Space, true);
        if (p.x > 17.0f) r.Hold(input::Key::Space, p.x < 19.5f);
        crossed = r.Grounded() && p.x > 24.0f;
    }
    CHECK(crossed);
    CHECK(Near(r.Position().y, 2.5f, 0.1f));
    // The block's wall at x = 40 stops it.
    for (int i = 0; i < 200; ++i) r.Frames(1);
    CHECK(r.Position().x < 40.0f - 0.4f + 0.1f && r.Position().x > 36.0f);
}

#if AETHER_TEST_HAS_SCRIPTING

namespace {

// A template's project, run: its scene, bindings and scripts, driven with
// simulated keys and mouse.
struct Run {
    ProjectPaths paths;
    std::unique_ptr<assets::AssetDatabase> db;
    World world;
#if AETHER_TEST_HAS_PHYSICS
    std::unique_ptr<JobSystem> jobs;
    std::unique_ptr<PhysicsWorld> physics;
    std::unique_ptr<PhysicsScene> physics_scene;
    std::unique_ptr<CharacterSystem> characters;
#endif
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
#if AETHER_TEST_HAS_PHYSICS
        RegisterPhysicsComponentSerializers();
        (void)GetComponentId<BoxCollider>();
        (void)GetComponentId<CharacterMovement>();
#endif
        AETHER_CHECK(LoadSceneJson(world, (paths.content / "Scenes/Main.ascene").string()));
#if AETHER_TEST_HAS_PHYSICS
        jobs = std::make_unique<JobSystem>(2);
        physics = std::make_unique<PhysicsWorld>(*jobs);
        physics_scene = std::make_unique<PhysicsScene>(world, *physics);
        physics_scene->Sync();
        characters = std::make_unique<CharacterSystem>(world, *physics, physics_scene.get());
#endif
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
#if AETHER_TEST_HAS_PHYSICS
            characters->Step(dt);
            physics_scene->Step(dt);
#endif
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
#if AETHER_TEST_HAS_PHYSICS
    CHECK(Near(p.z, -5.05f, 0.15f) && Near(p.x, 0.0f) && Near(r.Rotation(r.player).y, 0.0f));
    CHECK(r.world.HasComponent<CharacterMovement>(r.player));
    if (const CharacterMovement* movement = r.world.GetComponent<CharacterMovement>(r.player)) {
        CHECK(movement->grounded);
        CHECK(Near(p.y, 0.0f, 0.05f));
    }
#else
    CHECK(Near(p.z, -6.0f, 0.15f) && Near(p.x, 0.0f) && Near(r.Rotation(r.player).y, 0.0f));
#endif
    CHECK(r.Position(r.camera).z > p.z + 4.0f); // it followed
    r.state.SetButton(input::Key::W, false);

    // Running right turns it to face +X (a quarter turn, clockwise from above).
    r.state.SetButton(input::Key::D, true);
    r.Frames(30);
#if AETHER_TEST_HAS_PHYSICS
    CHECK(r.Position(r.player).x > 1.5f && Near(r.Rotation(r.player).y, -std::sqrt(0.5f), 0.02f));
#else
    CHECK(r.Position(r.player).x > 2.5f && Near(r.Rotation(r.player).y, -std::sqrt(0.5f), 0.02f));
#endif
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

// The packaged game (§26.4): the same projects, cooked and run by the
// player's Game, which hosts the scripts, the Blueprints and the input.
AETHER_TEST(Templates_ThePlayerRunsThemCooked) {
    CHECK(player::Game::HasScripting());
#if AETHER_TEST_HAS_PHYSICS
    const f32 expected_z[] = {-5.0f, -5.05f, -6.0f, -7.0f}; // acceleration affects the physics-backed template
#else
    const f32 expected_z[] = {-5.0f, -6.0f, -6.0f, -7.0f}; // after a second of W, by template
#endif
    for (usize i = 0; i < kPlayable.size(); ++i) {
        const std::string& id = kPlayable[i];
        const ProjectPaths paths = Create(id, "Cooked");
        cook::CookOptions options;
        options.project_file = paths.file;
        options.output_dir = paths.root / "Paks";
        options.configuration = cook::BuildConfiguration::Shipping;
        const cook::CookReport report = cook::Cook(options);
        CHECK(report.ok);
        player::GamePackage package;
        std::string error;
        CHECK(package.Mount(report.pak_file.string()) && package.LoadManifest(&error));
        player::Game game(package);
        CHECK(game.LoadStartupScene(&error) && game.Warnings().empty());
        // The bindings the template shipped are the game's actions.
        CHECK(game.InputAssets().Errors().empty() && game.InputActions().FindAction("Move") != nullptr);
        CHECK(game.InputAssets().FindContext("Gameplay") != nullptr && game.InputActions().HasContext("Gameplay"));
        game.BeginPlay();
        const usize pickups = CountTagged(game.GetWorld(), "Pickup");
        player::GameStats stats = game.Stats();
        CHECK(stats.script_instances == 1 && stats.blueprint_instances == pickups && pickups >= 6);

        const Entity hero = FindEntitiesWithTag(game.GetWorld(), "Player")[0];
        game.Input().SetButton(input::Key::W, true);
        for (int f = 0; f < 60; ++f) game.Tick(1.0f / 60.0f);
        const Vec3 p = game.GetWorld().GetComponent<Transform>(hero)->position;
        CHECK(Near(p.z, expected_z[i], 0.4f) && Near(p.x, 0.0f, 0.01f));

        // The Blueprint spun the pickups: a quarter turn in that second.
        const Entity pickup = FindEntitiesWithTag(game.GetWorld(), "Pickup")[0];
        CHECK(Near(game.GetWorld().GetComponent<Transform>(pickup)->rotation.y, std::sin(3.14159265f / 4.0f), 0.03f));
        stats = game.Stats();
        CHECK(stats.script_errors == 0 && stats.blueprint_errors == 0 && stats.fixed_steps == 60);
        game.Input().SetButton(input::Key::W, false);
        game.EndPlay();
    }
}

// The player's mouse path: events from a window become the game's input.
AETHER_TEST(Templates_FirstPersonLooksFromWindowEvents) {
    const ProjectPaths paths = Create("first_person", "Looking");
    cook::CookOptions options;
    options.project_file = paths.file;
    options.output_dir = paths.root / "Paks";
    const cook::CookReport report = cook::Cook(options);
    CHECK(report.ok);
    player::GamePackage package;
    CHECK(package.Mount(report.pak_file.string()) && package.LoadManifest());
    player::Game game(package);
    CHECK(game.LoadStartupScene());
    game.BeginPlay();
    const Entity hero = FindEntitiesWithTag(game.GetWorld(), "Player")[0];
    std::vector<WindowEvent> events;
    WindowEvent move;
    move.type = WindowEventType::MouseMove;
    move.dx = 500.0f;
    events.push_back(move);
    WindowEvent key;
    key.type = WindowEventType::Key;
    key.key = input::Key::W;
    key.down = true;
    events.push_back(key);
    ApplyWindowEvents(events, game.Input());
    game.Tick(1.0f / 60.0f);
    for (int f = 0; f < 30; ++f) game.Tick(1.0f / 60.0f); // the mouse moved once; W stays down
    const Transform* t = game.GetWorld().GetComponent<Transform>(hero);
    CHECK(t->rotation.y < -0.2f && t->position.x > 1.0f && t->position.z < -1.0f); // turned right, and went that way
    CHECK(game.Stats().script_errors == 0);
}

#endif // AETHER_TEST_HAS_SCRIPTING
