#include "aether/assets/asset_database.h"
#include "aether/sprite2d/atlas.h"
#include "aether/reflection/serialize.h"
#include "aether/gfx/rhi/command_list.h"
#include "aether/gfx/rhi/device.h"
#include "aether/player/scene_renderer.h"
#include "aether/scene/hierarchy.h"
#include "aether/scene/components.h"
#include "aether/assets/gltf_loader.h"
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

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <memory>
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

AETHER_TEST(Templates_3DProjectsStartWithAProceduralSky) {
    for (const std::string& id : kPlayable) {
        const ProjectPaths paths = Create(id, "Sky_" + id);
        std::ifstream stream(paths.content / "Scenes/Main.ascene");
        nlohmann::json scene;
        stream >> scene;
        bool found_sky = false;
        for (const auto& entity : scene.at("entities")) {
            const auto& components = entity.at("components");
            const auto sky = components.find("SkyLight");
            if (sky == components.end()) continue;
            found_sky = true;
            CHECK(sky->at("intensity").get<f32>() > 0.0f);
            CHECK(!sky->contains("environment") || sky->at("environment").get<std::string>().empty());
        }
        CHECK(found_sky);
    }
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

        // Something to see. A template used to be entities with no models, and a new project opened on a black window: the
        // floor, every pickup and (unless the player is the camera) the player's body carry a model that the database knows
        // under that GUID and that loads as a closed, outward-facing cube.
        const auto models = [&](const char* tag) {
            usize count = 0;
            for (const Entity e : FindEntitiesWithTag(world, tag)) {
                const ModelRenderer* renderer = world.GetComponent<ModelRenderer>(e);
                AETHER_CHECK(renderer != nullptr);
                if (renderer == nullptr) continue;
                const assets::AssetRecord* record = db.Find(renderer->model.guid);
                AETHER_CHECK(record != nullptr && record->path == renderer->asset_path && record->importer == "Model");
                ++count;
            }
            return count;
        };
        CHECK(models("Ground") == 1 && models("Pickup") >= 6);
        CHECK(models("PlayerBody") == (id == "first_person" ? 0u : 1u));
        for (const Entity body : FindEntitiesWithTag(world, "PlayerBody")) {
            const Parent* parent = world.GetComponent<Parent>(body);
            const IdComponent* player_id = world.GetComponent<IdComponent>(player);
            CHECK(parent != nullptr && player_id != nullptr && parent->parent == player_id->guid);
        }
        for (const char* file : {"Models/Floor.gltf", "Models/Player.gltf", "Models/Pickup.gltf"}) {
            assets::GltfScene gltf;
            CHECK(assets::LoadGltf((paths.content / file).string(), gltf));
            CHECK(gltf.meshes.size() == 1 && gltf.meshes[0].primitives.size() == 1 && gltf.materials.size() == 1);
            if (gltf.meshes.size() != 1 || gltf.meshes[0].primitives.size() != 1) continue;
            const assets::GltfPrimitive& cube = gltf.meshes[0].primitives[0];
            CHECK(cube.vertices.size() == 24 && cube.indices.size() == 36);
            bool outward = cube.indices.size() == 36;
            for (usize i = 0; i + 2 < cube.indices.size() && outward; i += 3) {
                const auto at = [&](usize k) {
                    const assets::GltfVertex& v = cube.vertices[cube.indices[i + k]];
                    return Vec3(v.position[0], v.position[1], v.position[2]);
                };
                const assets::GltfVertex& first = cube.vertices[cube.indices[i]];
                const Vec3 normal(first.normal[0], first.normal[1], first.normal[2]);
                const Vec3 face = (at(1) - at(0)).Cross(at(2) - at(0));
                // Counter-clockwise seen from outside, the stored normal agreeing, and the face on the cube's surface.
                outward = face.Dot(normal) > 0.0f && at(0).Dot(normal) > 0.49f;
            }
            CHECK(outward);
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
        // The models come along because the scene uses them, and the buffer they share comes with them.
        CHECK(has("Models/Floor.gltf") && has("Models/Pickup.gltf") && has("Models/Player.gltf") == (id != "first_person"));
        CHECK(std::find(report.extra_files.begin(), report.extra_files.end(), "Models/Cube.bin") != report.extra_files.end());
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
    CHECK(db.FindByPath("Sprites/Player.aatlas") && db.FindByPath("Sprites/Player.aatlas")->importer == "SpriteAtlas");
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
    CHECK(world.HasComponent<sprite2d::Sprite>(hero) && world.HasComponent<sprite2d::Rigidbody2D>(hero) && world.HasComponent<sprite2d::Collider2D>(hero) &&
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

    // Into the pit: running right without jumping, it falls through the gap. With the respawn switched off (the engine's
    // default; the template turns it on) it keeps falling.
    r.game->GetWorld().GetComponent<sprite2d::PlatformerController2D>(r.hero)->respawn_on_fall = false;
    r.Hold(input::Key::D, true);
    r.Frames(240);
    CHECK(r.Position().y < -5.0f);
    CHECK(r.Velocity().y < -5.0f);
    CHECK(r.game->GetWorld().GetComponent<sprite2d::PlatformerController2D>(r.hero)->respawns == 0);
}

// The template's level has a pit, and a first-time player holding right walks straight into it. The hero used to fall for ever
// and never be seen again; the template now turns on the controller's respawn, which puts it back where it started.
AETHER_TEST(Templates_PlatformerPutsTheHeroBackAfterAFall) {
    PlatformerRun r;
    r.Frames(90);
    const Vec3 start = r.Position();
    const auto controller = [&] { return r.game->GetWorld().GetComponent<sprite2d::PlatformerController2D>(r.hero); };
    CHECK(controller()->respawn_on_fall && controller()->respawns == 0);
    const f32 limit = controller()->fall_limit_y;

    r.Hold(input::Key::D, true);
    f32 lowest = start.y;
    int frames = 0;
    for (; frames < 600 && controller()->respawns == 0; ++frames) {
        r.Frames(1);
        lowest = std::min(lowest, r.Position().y);
    }
    r.Hold(input::Key::D, false);
    CHECK(controller()->respawns == 1 && frames < 600);
    // It fell past the limit by no more than one step's fall, then was put back at its start, at rest.
    CHECK(lowest > limit - 1.0f);
    CHECK(Near(r.Position().x, 3.0f, 0.3f) && r.Position().y > 2.0f && r.Position().y < 3.2f);
    r.Frames(90);
    CHECK(r.Grounded() && Near(r.Position().y, start.y, 0.05f) && Near(r.Position().x, 3.0f, 0.3f));
    CHECK(std::fabs(r.Velocity().x) < 0.1f && controller()->respawns == 1);

    // A limit above where the character started would put it back every step for ever: that is refused, and it just plays.
    PlatformerRun high;
    high.game->GetWorld().GetComponent<sprite2d::PlatformerController2D>(high.hero)->fall_limit_y = 100.0f;
    high.Frames(90);
    CHECK(high.game->GetWorld().GetComponent<sprite2d::PlatformerController2D>(high.hero)->respawns == 0);
    CHECK(high.Grounded() && Near(high.Position().y, start.y, 0.05f));
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

// The packaged player draws the 2D Platformer: rendered from the cooked template, the frame has the level's light tiles, the
// hero's colour and the dark background in it. Before sprites and tilemaps were drawn it was one dark colour.
AETHER_TEST(Templates_ThePlayerDrawsThePlatformer2D) {
    namespace rhi = aether::gfx::rhi;
    if (!rhi::IsBackendAvailable(rhi::Backend::Vulkan)) return;
    std::unique_ptr<rhi::IDevice> device;
    try {
        device = rhi::CreateDevice(rhi::Backend::Vulkan, false);
    } catch (const std::exception&) {
    }
    if (!device) return;
    PlatformerRun run;
    const u32 width = 320, height = 180;
    std::unique_ptr<rhi::ISwapChain> target = device->CreateSwapChain(nullptr, width, height, 2);
    std::vector<u8> pixels;
    {
        player::SceneRenderer renderer(*device, *target, run.package);
        std::unique_ptr<rhi::ICommandList> cmd = device->CreateCommandList();
        for (u32 frame = 0; frame < 12; ++frame) {
            run.Frames(1);
            target->AcquireNextImage();
            cmd->Reset();
            renderer.Draw(*run.game, *cmd, frame % 2);
            cmd->Close();
            device->WaitForFence(device->Submit(*cmd, target.get()));
            if (frame == 11) CHECK(target->ReadBack(pixels) && pixels.size() == static_cast<usize>(width) * height * 4);
            target->Present(false);
        }
    }
    if (pixels.size() != static_cast<usize>(width) * height * 4) return;
    usize tiles = 0, hero = 0, dark = 0;
    for (usize i = 0; i < pixels.size(); i += 4) {
        const int a = pixels[i], g = pixels[i + 1], c = pixels[i + 2];
        const int low = std::min({a, g, c}), high = std::max({a, g, c});
        if (low > 110 && high - low < 40) ++tiles;
        else if (high > 140 && high - low > 60) ++hero; // strongly coloured, whichever of red and blue the read-back puts first
        else if (high < 50) ++dark;
    }
    const usize total = static_cast<usize>(width) * height;
    std::printf("    2D platformer frame: %zu tile, %zu hero and %zu dark pixels of %zu\n", tiles, hero, dark, total);
    CHECK(tiles > total / 20 && hero > 20 && dark > total / 3);
}

// Sprite clips play in the packaged game. Nothing advanced a SpriteAnimator there, so an animated sprite stayed on one frame.
// The 2D template's hero gets a two-frame looping clip and a one-shot one, is cooked, and is watched frame by frame.
AETHER_TEST(Templates_SpriteClipsPlayInThePackagedGame) {
    std::string error;
    ProjectPaths paths;
    AETHER_CHECK(CreateProjectFromTemplate(Dir("sprite_clips"), "Clips2D", "platformer_2d", &paths, &error));
    sprite2d::SpriteAtlas atlas;
    AETHER_CHECK(sprite2d::LoadAtlas(paths.content / "Sprites/Player.aatlas", atlas, &error));
    atlas.frames.push_back({"step_a", 0, 0, 13, 16, 0.5f, 0.5f});
    atlas.frames.push_back({"step_b", 0, 0, 13, 16, 0.5f, 0.5f});
    atlas.clips.push_back({"run", {"step_a", "step_b"}, 10.0f, true});
    atlas.clips.push_back({"land", {"step_b", "idle"}, 10.0f, false});
    AETHER_CHECK(sprite2d::SaveAtlas(paths.content / "Sprites/Player.aatlas", atlas, &error));
    // The hero plays "run" from the start.
    const std::filesystem::path scene_file = paths.content / "Scenes/Main.ascene";
    nlohmann::json scene;
    {
        std::ifstream in(scene_file);
        in >> scene;
    }
    bool added = false;
    for (auto& entity : scene["entities"]) {
        if (!entity["components"].contains("Sprite")) continue;
        sprite2d::SpriteAnimator animator;
        sprite2d::PlayClip(animator, "run");
        entity["components"]["SpriteAnimator"] = reflect::ToJson(animator);
        added = true;
    }
    AETHER_CHECK(added);
    {
        std::ofstream out(scene_file, std::ios::binary);
        out << scene.dump(2);
    }
    cook::CookOptions options;
    options.project_file = paths.file;
    options.output_dir = paths.root / "Paks";
    options.configuration = cook::BuildConfiguration::Shipping;
    const cook::CookReport report = cook::Cook(options);
    AETHER_CHECK(report.ok);
    player::GamePackage package;
    AETHER_CHECK(package.Mount(report.pak_file.string()) && package.LoadManifest(&error));
    player::Game game(package);
    AETHER_CHECK(game.LoadStartupScene(&error));
    const Entity hero = FindEntitiesWithTag(game.GetWorld(), "Player")[0];
    game.BeginPlay();
    const auto frame = [&] { return std::string(game.GetWorld().GetComponent<sprite2d::Sprite>(hero)->frame); };
    const auto animator = [&] { return game.GetWorld().GetComponent<sprite2d::SpriteAnimator>(hero); };
    AETHER_CHECK(animator() != nullptr);
    if (animator() == nullptr) return;

    // 10 frames a second at 60 Hz: six ticks a frame. The clip loops a, b, a, b.
    std::string seen;
    for (int tick = 0; tick < 24; ++tick) {
        game.Tick(1.0f / 60.0f);
        if (tick % 6 == 2) seen += frame() == "step_a" ? 'a' : frame() == "step_b" ? 'b' : '?';
    }
    CHECK(seen == "abab");
    CHECK(animator()->playing && !animator()->finished);

    // A clip that does not loop ends on its last frame and says so; a game script starts it the same way.
    sprite2d::PlayClip(*animator(), "land");
    for (int tick = 0; tick < 30; ++tick) game.Tick(1.0f / 60.0f);
    CHECK(frame() == "idle" && animator()->finished && !animator()->playing);

    // A clip the atlas does not have leaves the sprite as it is, and the game does not complain every frame.
    sprite2d::PlayClip(*animator(), "no_such_clip");
    const usize warnings = game.Warnings().size();
    for (int tick = 0; tick < 5; ++tick) game.Tick(1.0f / 60.0f);
    CHECK(frame() == "idle" && game.Warnings().size() == warnings);
}
