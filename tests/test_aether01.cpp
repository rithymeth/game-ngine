#include "aether/cook/cooker.h"
#include "aether/audio/audio_system.h"
#include "aether/gameplay/attribute_set.h"
#include "aether/player/game.h"
#include "aether/scene/components.h"
#include "aether/scene/gameplay.h"
#include "aether/scene/serialization.h"
#include "aether/reflection/serialize.h"
#include "aether/sequencer/sequence_system.h"
#include "test_framework.h"
#include "aether/ai/perception.h"
#include "aether/nav/components.h"
#include "aether/nav/dynamic.h"
#include "aether/scene/script_component.h"
#if AETHER_TEST_HAS_PHYSICS
#include "aether/physics/components.h"
#endif

#include <filesystem>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <memory>

using namespace aether;
namespace fs = std::filesystem;

#if AETHER_TEST_HAS_SCRIPTING
#include "aether/script/ai_api.h"
namespace {
struct ProofProject {
    fs::path root;
    fs::path pak_file;

    ProofProject() {
        root = fs::temp_directory_path() / "aether01_proof_tests" /
               (std::string("shared") + assets::ToString(assets::NewAssetGuid()));
        fs::create_directories(root);
        const auto source = fs::path(AETHER_REPO_ASSETS_DIR).parent_path() / "games/AETHER-01";
        fs::copy(source, root / "project", fs::copy_options::recursive | fs::copy_options::overwrite_existing);
        cook::CookOptions options;
        options.project_file = root / "project/AETHER-01.aproject";
        options.output_dir = root / "cooked";
        const auto cooked = cook::Cook(options);
        AETHER_CHECK(cooked.ok);
        pak_file = cooked.pak_file;
    }

};

const ProofProject& GetProofProject() {
    static const ProofProject project;
    return project;
}

struct ProofRun {
    fs::path root;
    player::GamePackage package;
    std::unique_ptr<player::Game> game;
    Entity hero, scout;
    bool record = false;
    i64 recorded_frames = 0;
    input::InputState recorded_input;
    reflect::Json recorded_events = reflect::Json::array();

    explicit ProofRun(const char* name) {
        record = std::string(name) == "ending";
        root = fs::temp_directory_path() / "aether01_proof_tests" / (std::string(name) + assets::ToString(assets::NewAssetGuid()));
        fs::create_directories(root);
        const auto& project = GetProofProject();
        std::string error;
        AETHER_CHECK(package.Mount(project.pak_file.string(), 0, &error));
        AETHER_CHECK(package.LoadManifest(&error));
        AETHER_CHECK(package.FindAsset("fonts/Roboto-Medium.ttf") != nullptr);
        Start();
    }

    void Start() {
        game.reset();
        game = std::make_unique<player::Game>(package);
        std::string error;
        game->SetUserPaths(player::ResolveUserPaths("AETHER-01", root / "user"));
        AETHER_CHECK(game->LoadStartupScene(&error));
        const auto heroes = FindEntitiesWithTag(game->GetWorld(), "Player");
        const auto scouts = FindEntitiesWithTag(game->GetWorld(), "Scout");
        AETHER_CHECK(heroes.size() == 1 && scouts.size() == 1);
        hero = heroes.front();
        scout = scouts.front();
        game->BeginPlay();
        Frames(1);
    }
    void Frames(int count) {
        for (int i = 0; i < count; ++i) {
            if (record) {
                for (usize index = 1; index < input::kKeyCount; ++index) {
                    const auto key = static_cast<input::Key>(index);
                    const f32 value = game->Input().Value(key);
                    if (value != recorded_input.Value(key)) recorded_events.push_back(
                        {{"frame", recorded_frames}, {"key", input::KeyName(key)}, {"value", value}});
                }
                recorded_input = game->Input();
                recorded_input.EndFrame();
                ++recorded_frames;
            }
            game->Tick(1.0f / 60.0f);
        }
        AETHER_CHECK(game->ScriptErrors().empty());
        AETHER_CHECK(game->AudioSystem() != nullptr && game->AudioSystem()->Problems().empty());
    }
    f32 Stat(Entity entity, const char* name) {
        const auto* attributes = game->GetWorld().GetComponent<gas::AttributeSet>(entity);
        AETHER_CHECK(attributes != nullptr);
        return attributes ? attributes->Get(name, -1) : -1;
    }
    void Tap(input::Key key) {
        game->Input().SetButton(key, true);
        Frames(1);
        game->Input().SetButton(key, false);
        Frames(1);
    }
    void ClearResearch() {
        const Entity sentinel = FindEntitiesWithTag(game->GetWorld(), "Sentinel").front();
        const Entity hunter = FindEntitiesWithTag(game->GetWorld(), "Hunter").front();
        game->Input().AddMouseDelta(0, -143.24f); // level the aim down the service route
        Frames(1);
        Tap(input::Key::Num2);
        Frames(12);
        for (int shot = 0; shot < 3; ++shot) { Tap(input::Key::MouseLeft); Frames(15); }
        AETHER_CHECK(Stat(sentinel, "Shield") == 0 && Stat(sentinel, "Health") == 6);
        Tap(input::Key::Num1);
        Frames(12);
        for (int shot = 0; shot < 6; ++shot) { Tap(input::Key::MouseLeft); Frames(15); }
        // Enter the route: the bay wall occludes the Hunter from the spawn.
        game->Input().SetButton(input::Key::W, true);
        Frames(120);
        game->Input().SetButton(input::Key::W, false);
        game->Input().SetButton(input::Key::D, true);
        Frames(30);
        game->Input().SetButton(input::Key::D, false);
        Frames(20);
        for (int shot = 0; shot < 4; ++shot) { Tap(input::Key::MouseLeft); Frames(15); }
        AETHER_CHECK(Stat(sentinel, "Health") == 0 && Stat(hunter, "Health") == 0);
        AETHER_CHECK(Stat(hero, "ResearchClear") == 1);
        // Return to the bay so the route/arena checks retain their start point.
        game->Input().SetButton(input::Key::A, true);
        Frames(30);
        game->Input().SetButton(input::Key::A, false);
        game->Input().SetButton(input::Key::S, true);
        Frames(120);
        game->Input().SetButton(input::Key::S, false);
        Frames(20);
    }
};
}

AETHER_TEST(Aether01_AuthoredSceneSaveCloseReopenPreservesComponents) {
    ProofRun run("authoring");
    const auto first = (run.root / "authored.ascene").string();
    const auto second = (run.root / "reopened.ascene").string();
    {
        player::Game author(run.package);
        std::string error;
        AETHER_CHECK(author.LoadStartupScene(&error));
        const auto heroes = FindEntitiesWithTag(author.GetWorld(), "Player");
        AETHER_CHECK(heroes.size() == 1);
        author.GetWorld().GetComponent<Transform>(heroes.front())->position.x = 1.25f;
        AETHER_CHECK(SaveSceneJson(author.GetWorld(), first));
    }
    World reopened;
    AETHER_CHECK(LoadSceneJson(reopened, first));
    AETHER_CHECK(reopened.EntityCount() == 41);
    const auto heroes = FindEntitiesWithTag(reopened, "Player");
    AETHER_CHECK(heroes.size() == 1);
    AETHER_CHECK(reopened.GetComponent<Transform>(heroes.front())->position.x == 1.25f);
    AETHER_CHECK(SaveSceneJson(reopened, second));
    std::ifstream a(first), b(second);
    AETHER_CHECK(reflect::Json::parse(a) == reflect::Json::parse(b));
}

AETHER_TEST(Aether01_CookedCombatAmmoReloadAndScoutDefeat) {
    ProofRun run("combat");
    AETHER_CHECK(run.Stat(run.hero, "Health") == 3);
    AETHER_CHECK(run.Stat(run.hero, "Ammo") == 30);
    run.Tap(input::Key::E);
    AETHER_CHECK(run.Stat(run.hero, "MissionStage") == 1);
    for (int shot = 0; shot < 3; ++shot) {
        run.Tap(input::Key::MouseLeft);
        run.Frames(15);
        AETHER_CHECK(run.Stat(run.scout, "Health") == 2 - shot);
    }
    AETHER_CHECK(run.Stat(run.hero, "Ammo") == 27);
    AETHER_CHECK(run.game->GetWorld().GetComponent<Transform>(run.scout)->position.y < -50);
    AETHER_CHECK(run.Stat(run.hero, "MissionStage") == 2);
    run.Tap(input::Key::R);
    AETHER_CHECK(run.Stat(run.hero, "Reloading") == 1);
    run.Tap(input::Key::MouseLeft);
    AETHER_CHECK(run.Stat(run.hero, "Ammo") == 27);
    run.Frames(70);
    AETHER_CHECK(run.Stat(run.hero, "Ammo") == 30);
    AETHER_CHECK(run.Stat(run.hero, "Reloading") == 0);
}

AETHER_TEST(Aether01_CookedContactDeathAndRestart) {
    ProofRun run("restart");
    run.Tap(input::Key::E);
    run.Frames(750);
    AETHER_CHECK(run.Stat(run.hero, "Health") == 0);
    run.Tap(input::Key::R);
    AETHER_CHECK(run.Stat(run.hero, "Health") == 3);
    AETHER_CHECK(run.Stat(run.hero, "Ammo") == 30);
    AETHER_CHECK(run.Stat(run.scout, "Health") == 3);
    AETHER_CHECK(run.Stat(run.hero, "DamageFlash") == 0);
}

AETHER_TEST(Aether01_PauseMenusFreezeCombatAndResume) {
    ProofRun run("pause");
    run.Tap(input::Key::E);
    const Vec3 hero = run.game->GetWorld().GetComponent<Transform>(run.hero)->position;
    const Vec3 scout = run.game->GetWorld().GetComponent<Transform>(run.scout)->position;
    run.Tap(input::Key::Escape);
    AETHER_CHECK(run.game->IsPaused() && run.Stat(run.hero, "MenuState") == 1);
    const auto stats = run.game->Stats();
    run.game->Input().SetButton(input::Key::W, true);
    run.game->Input().SetButton(input::Key::MouseLeft, true);
    run.Frames(300);
    run.game->Input().SetButton(input::Key::W, false);
    run.game->Input().SetButton(input::Key::MouseLeft, false);
    const auto still = run.game->GetWorld().GetComponent<Transform>(run.hero)->position;
    const auto enemy = run.game->GetWorld().GetComponent<Transform>(run.scout)->position;
    AETHER_CHECK(still.z == hero.z && enemy.z == scout.z);
    AETHER_CHECK(run.Stat(run.hero, "Ammo") == 30 && run.Stat(run.hero, "Health") == 3);
    AETHER_CHECK(run.game->Stats().time == stats.time && run.game->Stats().fixed_steps == stats.fixed_steps);
    run.Tap(input::Key::Down);
    run.Tap(input::Key::Down);
    run.Tap(input::Key::Enter);
    AETHER_CHECK(run.Stat(run.hero, "MenuState") == 3);
    run.Tap(input::Key::Escape);
    run.Tap(input::Key::Escape);
    AETHER_CHECK(!run.game->IsPaused());
    run.Frames(60);
    AETHER_CHECK(run.game->Stats().time > stats.time && run.game->GetWorld().GetComponent<Transform>(run.scout)->position.z > scout.z);
}

AETHER_TEST(Aether01_AudioMenuPersistsVolumesAndReportsWriteFailure) {
    ProofRun run("settings-menu");
    run.Tap(input::Key::GamepadStart);
    run.Tap(input::Key::GamepadDPadDown);
    run.Tap(input::Key::GamepadA);
    AETHER_CHECK(run.Stat(run.hero, "MenuState") == 2);
    run.Tap(input::Key::GamepadDPadLeft);
    AETHER_CHECK(std::abs(run.game->Settings().Get().master - 0.9f) < 0.001f);
    AETHER_CHECK(run.Stat(run.hero, "SettingsStatus") == 1);
    run.Start();
    run.game->LoadSettings({});
    AETHER_CHECK(std::abs(run.game->Settings().Get().master - 0.9f) < 0.001f);
    AETHER_CHECK(run.game->AudioSystem()->GetMixer().BusVolume(audio::kMasterBus) < 0);
    const auto blocked = run.root / "blocked-settings";
    std::ofstream(blocked) << "file blocks the settings directory";
    run.game->SetUserPaths(player::ResolveUserPaths("AETHER-01", blocked));
    run.Tap(input::Key::Escape);
    run.Tap(input::Key::Down);
    run.Tap(input::Key::Enter);
    run.Tap(input::Key::Left);
    AETHER_CHECK(run.Stat(run.hero, "SettingsStatus") == -1);
}

AETHER_TEST(Aether01_MenuCheckpointNewGameAndQuit) {
    ProofRun run("menu-actions");
    run.Tap(input::Key::GamepadStart);
    run.Tap(input::Key::GamepadA);
    run.Frames(10);
    AETHER_CHECK(!run.game->IsPaused() && run.Stat(run.hero, "MissionStage") == 0);
    AETHER_CHECK(std::abs(run.game->GetWorld().GetComponent<Transform>(run.hero)->position.y) < 0.01f);
    run.Tap(input::Key::GamepadStart);
    run.Tap(input::Key::GamepadB);
    AETHER_CHECK(!run.game->IsPaused() && run.Stat(run.hero, "MissionStage") == 0);
    run.Tap(input::Key::GamepadB);
    AETHER_CHECK(!run.game->IsPaused() && run.Stat(run.hero, "MissionStage") == 1);
    run.Tap(input::Key::N);
    run.Tap(input::Key::Escape);
    for (int i = 0; i < 3; ++i) run.Tap(input::Key::Down);
    run.Tap(input::Key::Enter);
    AETHER_CHECK(run.game->IsPaused() && run.Stat(run.hero, "SettingsStatus") == -2);
    run.Tap(input::Key::Escape);
    run.Tap(input::Key::E);
    run.Tap(input::Key::MouseLeft);
    AETHER_CHECK(run.Stat(run.hero, "Ammo") == 29);
    run.Tap(input::Key::Escape);
    for (int i = 0; i < 3; ++i) run.Tap(input::Key::Down);
    run.Tap(input::Key::Enter);
    AETHER_CHECK(!run.game->IsPaused() && run.Stat(run.hero, "Ammo") == 30 && run.Stat(run.scout, "Health") == 3);
    run.Tap(input::Key::Escape);
    for (int i = 0; i < 4; ++i) run.Tap(input::Key::Down);
    run.Tap(input::Key::Enter);
    AETHER_CHECK(run.Stat(run.hero, "MissionStage") == 0 && !run.game->Saves()->Exists("first-contact"));
    run.Tap(input::Key::Escape);
    for (int i = 0; i < 5; ++i) run.Tap(input::Key::Down);
    run.Tap(input::Key::Enter);
    AETHER_CHECK(run.game->ExitRequested());
}

AETHER_TEST(Aether01_CookedWeaponReloadAndDamageSoundFeedback) {
    ProofRun run("audio");
    auto& world = run.game->GetWorld();
    const Entity speaker = FindEntitiesWithTag(world, "WeaponAudio").front();
    const Entity damage = FindEntitiesWithTag(world, "DamageAudio").front();
    run.Tap(input::Key::E);
    for (int weapon = 1; weapon <= 3; ++weapon) {
        if (weapon == 2) run.Tap(input::Key::Num2);
        if (weapon == 3) run.Tap(input::Key::Num3);
        run.Frames(15);
        run.Tap(input::Key::MouseLeft);
        const auto* source = world.GetComponent<AudioSource>(speaker);
        const char* paths[] = {"Audio/rifle.acue", "Audio/pistol.acue", "Audio/shotgun.acue"};
        AETHER_CHECK(source != nullptr && source->playing && source->cue == paths[weapon - 1]);
        AETHER_CHECK(run.game->AudioSystem()->HandleOf(speaker) != 0);
    }
    run.Tap(input::Key::R);
    AETHER_CHECK(world.GetComponent<AudioSource>(speaker)->cue == "Audio/reload.acue");
    world.GetComponent<gas::AttributeSet>(run.hero)->SetBase("Health", 2);
    run.Frames(2);
    AETHER_CHECK(world.GetComponent<AudioSource>(damage)->playing);
    AETHER_CHECK(run.game->AudioSystem()->GetMixer().Meter(audio::kMasterBus).peak[0] > 0.01f);
    AETHER_CHECK(run.game->Warnings().empty());
}

AETHER_TEST(Aether01_WeaponMagazinesSwitchReloadAndHeldFire) {
    ProofRun run("weapons");
    run.Tap(input::Key::Num3);
    AETHER_CHECK(run.Stat(run.hero, "WeaponIndex") == 1); // locked before pickup
    run.Tap(input::Key::E);
    run.Tap(input::Key::GamepadDPadUp);
    run.Frames(12);
    AETHER_CHECK(run.Stat(run.hero, "WeaponIndex") == 2);
    AETHER_CHECK(run.Stat(run.hero, "MaxAmmo") == 12);
    run.Tap(input::Key::MouseLeft);
    AETHER_CHECK(run.Stat(run.hero, "Ammo") == 11);
    run.Tap(input::Key::Num3);
    run.Frames(12);
    run.Tap(input::Key::MouseLeft);
    AETHER_CHECK(run.Stat(run.hero, "Ammo") == 5);
    run.Tap(input::Key::Num1);
    AETHER_CHECK(run.Stat(run.hero, "Ammo") == 30);
    run.Tap(input::Key::Num2);
    AETHER_CHECK(run.Stat(run.hero, "Ammo") == 11);
    run.Tap(input::Key::R);
    run.Tap(input::Key::Num3);
    AETHER_CHECK(run.Stat(run.hero, "WeaponIndex") == 2);
    run.Frames(60);
    AETHER_CHECK(run.Stat(run.hero, "Ammo") == 12);
    run.game->Input().SetButton(input::Key::MouseLeft, true);
    run.Frames(60);
    run.game->Input().SetButton(input::Key::MouseLeft, false);
    AETHER_CHECK(run.Stat(run.hero, "Ammo") <= 7 && run.Stat(run.hero, "Ammo") >= 5);
    run.Tap(input::Key::N);
    AETHER_CHECK(run.Stat(run.hero, "WeaponIndex") == 1 && run.Stat(run.hero, "Ammo") == 30);
}

AETHER_TEST(Aether01_ShotgunRangeDamageKnockbackAndStagger) {
    ProofRun run("shotgun");
    run.Tap(input::Key::E);
    auto& world = run.game->GetWorld();
    world.GetComponent<Transform>(run.scout)->position = Vec3(0, 1, -12);
    run.Tap(input::Key::Num3);
    run.Frames(12);
    run.Tap(input::Key::MouseLeft);
    AETHER_CHECK(run.Stat(run.scout, "Health") == 3); // out of shotgun range
    world.GetComponent<Transform>(run.scout)->position = Vec3(0, 1, -6);
    run.Frames(50);
    run.Tap(input::Key::MouseLeft);
    AETHER_CHECK(run.Stat(run.scout, "Health") == 0); // one close three-damage blast
    run.Frames(50);
    const Entity hunter = FindEntitiesWithTag(world, "Hunter").front();
    world.GetComponent<Transform>(hunter)->position = Vec3(0, 0.9f, -6);
    run.Tap(input::Key::MouseLeft);
    AETHER_CHECK(run.Stat(hunter, "Health") == 1);
    AETHER_CHECK(world.GetComponent<Transform>(hunter)->position.z < -7);
    AETHER_CHECK(run.Stat(hunter, "Stagger") > 0);
    run.Frames(35);
    AETHER_CHECK(run.Stat(hunter, "Stagger") == 0);
}

AETHER_TEST(Aether01_SentinelTellDamageAndDodge) {
    ProofRun run("sentinel");
    run.Tap(input::Key::E);
    for (int shot = 0; shot < 3; ++shot) { run.Tap(input::Key::MouseLeft); run.Frames(15); }
    run.Start(); // research checkpoint gives a repeatable attack timer
    const Entity sentinel = FindEntitiesWithTag(run.game->GetWorld(), "Sentinel").front();
    run.Frames(120);
    AETHER_CHECK(run.Stat(sentinel, "AttackState") == 1);
    const Entity warning = FindEntitiesWithTag(run.game->GetWorld(), "WarningAudio").front();
    AETHER_CHECK(run.game->GetWorld().GetComponent<AudioSource>(warning)->playing);
    AETHER_CHECK(run.Stat(run.hero, "Health") == 3);
    run.Frames(67);
    AETHER_CHECK(run.Stat(run.hero, "Health") == 2);
    run.Start();
    run.Frames(120);
    run.game->Input().SetButton(input::Key::D, true);
    run.Frames(30);
    run.game->Input().SetButton(input::Key::D, false);
    run.Frames(38);
    AETHER_CHECK(run.Stat(run.hero, "Health") == 3);
    AETHER_CHECK(run.Stat(run.hero, "ResearchClear") == 0);
}

AETHER_TEST(Aether01_HunterTelegraphsChargeAndCanBeDodged) {
    ProofRun run("hunter");
    run.Tap(input::Key::E);
    for (int shot = 0; shot < 3; ++shot) { run.Tap(input::Key::MouseLeft); run.Frames(15); }
    auto prepare = [&]() {
        auto& world = run.game->GetWorld();
        const Entity sentinel = FindEntitiesWithTag(world, "Sentinel").front();
        // Isolate the Hunter behavior; the complete mission test kills both via inputs.
        world.GetComponent<gas::AttributeSet>(sentinel)->SetBase("Health", 0);
        world.GetComponent<Transform>(sentinel)->position.y = -100;
        run.game->Input().SetButton(input::Key::W, true);
        run.Frames(155);
        run.game->Input().SetButton(input::Key::W, false);
        run.Frames(20);
    };
    prepare();
    Entity hunter = FindEntitiesWithTag(run.game->GetWorld(), "Hunter").front();
    for (int i = 0; i < 150 && run.Stat(hunter, "AttackState") == 0; ++i) run.Frames(1);
    AETHER_CHECK(run.Stat(hunter, "AttackState") == 1);
    AETHER_CHECK(run.Stat(run.hero, "Health") == 3);
    run.Frames(90);
    AETHER_CHECK(run.Stat(run.hero, "Health") == 2);
    run.Start();
    prepare();
    hunter = FindEntitiesWithTag(run.game->GetWorld(), "Hunter").front();
    for (int i = 0; i < 150 && run.Stat(hunter, "AttackState") == 0; ++i) run.Frames(1);
    AETHER_CHECK(run.Stat(hunter, "AttackState") == 1);
    run.game->Input().SetButton(input::Key::A, true);
    run.Frames(40);
    run.game->Input().SetButton(input::Key::A, false);
    run.Frames(50);
    AETHER_CHECK(run.Stat(run.hero, "Health") == 3);
}

AETHER_TEST(Aether01_PickupCheckpointReloadAndServiceRoute) {
    ProofRun run("checkpoint");
    run.Tap(input::Key::MouseLeft);
    AETHER_CHECK(run.Stat(run.hero, "Ammo") == 30); // rifle is locked until retrieved
    AETHER_CHECK(run.Stat(run.hero, "MissionStage") == 0);
    run.Tap(input::Key::E);
    AETHER_CHECK(run.Stat(run.hero, "MissionStage") == 1);
    AETHER_CHECK(run.Stat(run.hero, "SaveStatus") == 1);
    run.Start(); // a new runtime from the cooked package, using the same save folder
    AETHER_CHECK(run.Stat(run.hero, "MissionStage") == 1);
    for (int shot = 0; shot < 3; ++shot) {
        run.Tap(input::Key::MouseLeft);
        run.Frames(15);
    }
    AETHER_CHECK(run.Stat(run.hero, "MissionStage") == 2);
    run.Start();
    AETHER_CHECK(run.Stat(run.hero, "MissionStage") == 2);
    AETHER_CHECK(run.Stat(run.scout, "Health") == 0);
    AETHER_CHECK(run.Stat(run.hero, "Ammo") == 30);
    const auto gates = FindEntitiesWithTag(run.game->GetWorld(), "ServiceGate");
    AETHER_CHECK(gates.size() == 1);
    AETHER_CHECK(run.game->GetWorld().GetComponent<Transform>(gates.front())->position.y < -5);
    run.ClearResearch();
    run.game->Input().SetButton(input::Key::W, true);
    run.Frames(280);
    AETHER_CHECK(run.Stat(run.hero, "MissionStage") == 3);
    const auto p = run.game->GetWorld().GetComponent<Transform>(run.hero)->position;
    AETHER_CHECK(p.z < -22 && p.y > -0.2f);
    run.game->Input().SetButton(input::Key::W, false);
    run.Start();
    AETHER_CHECK(run.Stat(run.hero, "MissionStage") == 3);
    AETHER_CHECK(std::abs(run.game->GetWorld().GetComponent<Transform>(run.hero)->position.z + 23) < 0.01f);
}

AETHER_TEST(Aether01_WardenPhasesEndingAndNewGame) {
    ProofRun run("ending");
    run.Tap(input::Key::E);
    for (int shot = 0; shot < 3; ++shot) { run.Tap(input::Key::MouseLeft); run.Frames(15); }
    run.ClearResearch();
    run.game->Input().SetButton(input::Key::W, true);
    run.Frames(280);
    run.game->Input().SetButton(input::Key::W, false);
    run.Frames(20);
    AETHER_CHECK(run.Stat(run.hero, "MissionStage") == 3);
    const auto bosses = FindEntitiesWithTag(run.game->GetWorld(), "Warden");
    AETHER_CHECK(bosses.size() == 1);
    const Entity boss = bosses.front();
    // Level the aim at the Warden, then use the real input/fire/cooldown path.
    run.Frames(1);
    for (int shot = 0; shot < 9; ++shot) {
        run.Tap(input::Key::MouseLeft);
        run.Frames(15);
        if (shot == 2) AETHER_CHECK(run.Stat(boss, "Phase") == 2);
        if (shot == 5) AETHER_CHECK(run.Stat(boss, "Phase") == 3);
        if (shot == 7) AETHER_CHECK(run.Stat(boss, "Phase") == 4);
    }
    AETHER_CHECK(run.Stat(boss, "Health") == 0);
    AETHER_CHECK(run.Stat(run.hero, "MissionStage") == 4);
    AETHER_CHECK(run.Stat(run.hero, "Health") > 0);
    run.game->Input().SetButton(input::Key::W, true);
    for (int frame = 0; frame < 240 && run.game->GetWorld().GetComponent<Transform>(run.hero)->position.z > -39; ++frame)
        run.Frames(1);
    run.game->Input().SetButton(input::Key::W, false);
    run.Frames(20);
    run.Tap(input::Key::E);
    AETHER_CHECK(run.Stat(run.hero, "MissionStage") == 5);
    run.Frames(20);
    const Entity archive = FindEntitiesWithTag(run.game->GetWorld(), "ArchiveTerminal").front();
    const Entity camera = FindEntitiesWithTag(run.game->GetWorld(), "MainCamera").front();
    AETHER_CHECK(run.game->GetWorld().GetComponent<SequenceComponent>(archive)->playing);
    AETHER_CHECK(run.game->GetWorld().GetComponent<AudioSource>(archive)->playing);
    AETHER_CHECK(run.game->AudioSystem()->HandleOf(archive) != 0);
    const auto camera_position = run.game->GetWorld().GetComponent<Transform>(camera)->position;
    AETHER_CHECK(camera_position.z < -32 && camera_position.z > -34);
    run.Frames(480);
    AETHER_CHECK(!run.game->GetWorld().GetComponent<SequenceComponent>(archive)->playing);
    AETHER_CHECK(run.Stat(run.hero, "ArchiveTime") == 8);
    if (const char* path = std::getenv("AETHER01_INPUT_TRACE")) {
        std::ofstream trace(path);
        trace << reflect::Json({{"frames", run.recorded_frames}, {"events", run.recorded_events}}).dump(2);
        AETHER_CHECK(trace.good());
    }
    run.Start();
    AETHER_CHECK(run.Stat(run.hero, "MissionStage") == 5);
    const auto restored_bosses = FindEntitiesWithTag(run.game->GetWorld(), "Warden");
    AETHER_CHECK(run.Stat(restored_bosses.front(), "Health") == 0);
    run.Tap(input::Key::N);
    AETHER_CHECK(run.Stat(run.hero, "MissionStage") == 0);
    AETHER_CHECK(run.Stat(run.scout, "Health") == 3);
    AETHER_CHECK(run.Stat(restored_bosses.front(), "Health") == 12);
}

AETHER_TEST(Aether01_WardenStrikeTellDamageAndDodge) {
    ProofRun run("dodge");
    run.Tap(input::Key::E);
    for (int shot = 0; shot < 3; ++shot) { run.Tap(input::Key::MouseLeft); run.Frames(15); }
    run.ClearResearch();
    run.game->Input().SetButton(input::Key::W, true);
    run.Frames(280);
    run.game->Input().SetButton(input::Key::W, false);
    run.Start(); // arena checkpoint, fresh attack timing and health
    const Entity boss = FindEntitiesWithTag(run.game->GetWorld(), "Warden").front();
    run.Frames(90);
    AETHER_CHECK(run.Stat(boss, "AttackState") == 1);
    AETHER_CHECK(run.Stat(run.hero, "Health") == 3); // tell precedes damage
    run.Frames(91);
    AETHER_CHECK(run.Stat(run.hero, "Health") == 2);
    run.Start();
    const Entity restarted_boss = FindEntitiesWithTag(run.game->GetWorld(), "Warden").front();
    run.Frames(90);
    AETHER_CHECK(run.Stat(restarted_boss, "AttackState") == 1);
    run.game->Input().SetButton(input::Key::D, true);
    run.Frames(85);
    run.game->Input().SetButton(input::Key::D, false);
    run.Frames(10);
    AETHER_CHECK(run.Stat(run.hero, "Health") == 3); // moving outside the marked area dodges it
}

AETHER_TEST(Aether01_CheckpointWriteFailureIsVisible) {
    ProofRun run("save-failure");
    const fs::path blocked = run.root / "blocked";
    std::ofstream(blocked) << "a file cannot contain a save directory";
    run.game->SetUserPaths(player::ResolveUserPaths("AETHER-01", blocked));
    run.Tap(input::Key::E);
    AETHER_CHECK(run.Stat(run.hero, "MissionStage") == 1);
    AETHER_CHECK(run.Stat(run.hero, "SaveStatus") == -1);
    AETHER_CHECK(!run.game->Saves()->LastError().empty());
}

// M8.8: the archetypes use the engine's navigation and line of sight. A physics
// player bakes both from the scene's box colliders; without physics there are no
// colliders, so these tests stand in the same boxes (Main.ascene's static ones).
struct Aether01Ai {
    struct Box {
        Vec3 center, half;
    };
    std::vector<Box> walls = {
        {{0, -0.5f, 0}, {5.5f, 0.5f, 8.5f}},      {{-5.5f, 2, 0}, {0.5f, 2, 8.5f}},      {{5.5f, 2, 0}, {0.5f, 2, 8.5f}},
        {{-3.5f, 2, -8.5f}, {2.5f, 2, 0.5f}},     {{3.5f, 2, -8.5f}, {2.5f, 2, 0.5f}},   {{0, 3.5f, -8.5f}, {1, 0.5f, 0.5f}},
        {{-3.5f, 2, 8.5f}, {2.5f, 2, 0.5f}},      {{3.5f, 2, 8.5f}, {2.5f, 2, 0.5f}},    {{0, 3.5f, 8.5f}, {1, 0.5f, 0.5f}},
        {{-3, 1.5f, -4}, {0.5f, 1.5f, 0.5f}},     {{3, 1.5f, -4}, {0.5f, 1.5f, 0.5f}},   {{0, -0.5f, -17}, {5.5f, 0.5f, 8.5f}},
        {{-5.5f, 2, -17}, {0.5f, 2, 8.5f}},       {{5.5f, 2, -17}, {0.5f, 2, 8.5f}},
    };
    std::unique_ptr<nav::NavWorld> navigation;
    std::unique_ptr<ai::PerceptionWorld> perception;

    // Makes sure the game has them: the player's own (physics builds) or these.
    explicit Aether01Ai(World& world) {
#if AETHER_TEST_HAS_PHYSICS
        (void)world;
        AETHER_CHECK(nav::NavWorld::Active() != nullptr && nav::NavWorld::Active()->Ready());
        AETHER_CHECK(ai::PerceptionWorld::Active() != nullptr && ai::PerceptionWorld::Active()->HasLineOfSightTest());
#else
        AETHER_CHECK(nav::NavWorld::Active() == nullptr); // nothing to bake from without colliders
        nav::NavGeometry geometry;
        for (const Box& box : walls) geometry.AddBox(box.center, box.half);
        navigation = std::make_unique<nav::NavWorld>(world, [](const ModelRenderer&, std::vector<Vec3>&, std::vector<u32>&) { return false; });
        std::string error;
        AETHER_CHECK(navigation->Dynamic().Build(std::move(geometry), nav::NavMeshSettings{}, &error));
        perception = std::make_unique<ai::PerceptionWorld>(world);
        perception->SetLineOfSight([this](const Vec3& from, const Vec3& to, Entity, Entity) {
            for (const Box& box : walls) {
                f32 first = 0, last = 1;
                const f32 o[3] = {from.x, from.y, from.z}, d[3] = {to.x - from.x, to.y - from.y, to.z - from.z};
                const f32 c[3] = {box.center.x, box.center.y, box.center.z}, h[3] = {box.half.x, box.half.y, box.half.z};
                bool hit = true;
                for (int axis = 0; axis < 3 && hit; ++axis) {
                    if (std::abs(d[axis]) < 1e-5f) { hit = o[axis] >= c[axis] - h[axis] && o[axis] <= c[axis] + h[axis]; continue; }
                    f32 a = (c[axis] - h[axis] - o[axis]) / d[axis], b = (c[axis] + h[axis] - o[axis]) / d[axis];
                    if (a > b) std::swap(a, b);
                    first = std::max(first, a);
                    last = std::min(last, b);
                    hit = first <= last;
                }
                if (hit) return false;
            }
            return true;
        });
#endif
    }
    // Closes the service doorway with a solid box; OpenDoorway takes it away again.
    Entity BlockDoorway(World& world) { return Block(world, Vec3(0, 2, -8.5f), Vec3(1, 2, 0.4f)); }
    void OpenDoorway(World& world, Entity gate) { Unblock(world, gate); }
    // A solid box that blocks sight (the last one made is the one Unblock takes away).
    Entity Block(World& world, const Vec3& center, const Vec3& half) {
#if AETHER_TEST_HAS_PHYSICS
        Transform at;
        at.position = center;
        BoxCollider gate;
        gate.half_extents = half;
        return world.CreateEntity(at, gate);
#else
        (void)world;
        walls.push_back({center, half});
        return kNullEntity;
#endif
    }
    void Unblock(World& world, Entity gate) {
#if AETHER_TEST_HAS_PHYSICS
        world.GetComponent<Transform>(gate)->position.y = -100;
#else
        (void)world;
        (void)gate;
        walls.pop_back();
#endif
    }
};

AETHER_TEST(Aether01_ScoutPathsAroundACryoPodOnTheNavMesh) {
    ProofRun run("scout_nav");
    auto& world = run.game->GetWorld();
    Aether01Ai ai(world);
    run.Tap(input::Key::E); // the rifle wakes the Scout
    AETHER_CHECK(run.Stat(run.hero, "MissionStage") == 1);
    const Vec3 hero = world.GetComponent<Transform>(run.hero)->position;
    // Directly behind the pod at (3, -4) as seen from Kael: a straight chase walks into it.
    const Vec3 pod(3.0f, 0.0f, -4.0f);
    const f32 along = std::hypot(pod.x - hero.x, pod.z - hero.z);
    const Vec3 away((pod.x - hero.x) / along, 0, (pod.z - hero.z) / along);
    const Vec3 start(pod.x + away.x * 1.5f, 0.9f, pod.z + away.z * 1.5f);
    world.GetComponent<Transform>(run.scout)->position = start;
    const f32 start_distance = std::hypot(start.x - hero.x, start.z - hero.z);
    bool entered_pod = false;
    f32 widest = 0.0f; // how far it strayed from the straight line
    for (int frame = 0; frame < 300; ++frame) {
        run.Frames(1);
        const Vec3 p = world.GetComponent<Transform>(run.scout)->position;
        // The pod's box grown by the Scout's half width.
        if (std::abs(p.x - pod.x) < 0.9f && std::abs(p.z - pod.z) < 0.9f) entered_pod = true;
        AETHER_CHECK(std::abs(p.y - 0.9f) < 1e-4f);
        widest = std::max(widest, std::abs((p.x - hero.x) * away.z - (p.z - hero.z) * away.x));
    }
    const Vec3 end = world.GetComponent<Transform>(run.scout)->position;
    AETHER_CHECK(!entered_pod);
    AETHER_CHECK(widest > 0.9f);
    AETHER_CHECK(std::hypot(end.x - hero.x, end.z - hero.z) < start_distance - 2.0f);
}

AETHER_TEST(Aether01_SentinelLosesKaelWhenSightIsBlocked) {
    ProofRun run("sentinel_sight");
    run.Tap(input::Key::E);
    for (int shot = 0; shot < 3; ++shot) { run.Tap(input::Key::MouseLeft); run.Frames(15); }
    run.Start(); // research checkpoint
    auto& world = run.game->GetWorld();
    Aether01Ai ai(world);
    const Entity sentinel = FindEntitiesWithTag(world, "Sentinel").front();
    run.Frames(120);
    AETHER_CHECK(run.Stat(sentinel, "AttackState") == 1); // it sees Kael through the open door and telegraphs
    ai.BlockDoorway(world); // mid-tell: the Sentinel loses its target
    run.Frames(1);
    AETHER_CHECK(run.Stat(sentinel, "AttackState") == 0);
    run.Frames(240);
    AETHER_CHECK(run.Stat(sentinel, "AttackState") == 0);
    AETHER_CHECK(run.Stat(run.hero, "Health") == 3);
}

// M8.8 encounter scenarios: one deterministic run per archetype covering
// detection, loss of target, a navmesh rebuild mid-path (movers only),
// checkpoint reload and (Warden) the boss phases. Obstacles are carved into
// the active navmesh as dynamic volumes, so both configs rebuild the same way.
namespace {
// The AI debugger's row for an entity (Game::AiDebug); null if it has none.
const player::Game::AiDebugInfo* DebugOf(const std::vector<player::Game::AiDebugInfo>& rows, Entity e) {
    for (const auto& row : rows) if (row.entity == e) return &row;
    return nullptr;
}
Vec3 PositionOf(ProofRun& run, Entity e) { return run.game->GetWorld().GetComponent<Transform>(e)->position; }
f32 Flat(const Vec3& a, const Vec3& b) { return std::hypot(a.x - b.x, a.z - b.z); }
bool InBox(const Vec3& p, const Vec3& center, const Vec3& half, f32 grow) {
    return std::abs(p.x - center.x) < half.x + grow && std::abs(p.z - center.z) < half.z + grow;
}
// Carves a box into the active navmesh and rebakes it now.
nav::DynamicNavMesh::Id CarveObstacle(const Vec3& center, const Vec3& half) {
    nav::NavWorld* nw = nav::NavWorld::Active();
    AETHER_CHECK(nw != nullptr && nw->Ready());
    const u64 before = nw->Dynamic().Revision();
    const auto id = nw->Dynamic().AddVolume(nav::NavVolume::Box(center, half));
    nw->Dynamic().Update(0);
    AETHER_CHECK(nw->Dynamic().Revision() != before && nw->Dynamic().PendingTiles() == 0);
    return id;
}
} // namespace

AETHER_TEST(Aether01_Scenario_Scout) {
    ProofRun run("scenario_scout");
    const Vec3 authored = PositionOf(run, run.scout);
    run.Frames(60);
    AETHER_CHECK(Flat(PositionOf(run, run.scout), authored) < 1e-4f); // dormant before the rifle
    run.Tap(input::Key::E);                                             // checkpoint: MissionStage 1 is saved
    AETHER_CHECK(run.Stat(run.hero, "MissionStage") == 1 && run.Stat(run.hero, "SaveStatus") == 1);
    run.Frames(30);
    AETHER_CHECK(Flat(PositionOf(run, run.scout), authored) > 0.3f);
    // Checkpoint reload: a fresh runtime puts the Scout back and it re-engages.
    run.Start();
    AETHER_CHECK(run.Stat(run.hero, "MissionStage") == 1);
    AETHER_CHECK(Flat(PositionOf(run, run.scout), authored) < 0.05f); // (Start ran one frame of pursuit)
    auto& world = run.game->GetWorld();
    Aether01Ai ai(world);
    const Vec3 hero = PositionOf(run, run.hero);
    // Detection: it closes on Kael.
    const f32 d0 = Flat(PositionOf(run, run.scout), hero);
    run.Frames(30);
    AETHER_CHECK(Flat(PositionOf(run, run.scout), hero) < d0 - 0.4f);
    // Loss of target: beyond its leash it holds position, then re-engages in range.
    const Vec3 far(0, 0.9f, hero.z - 40.0f);
    world.GetComponent<Transform>(run.scout)->position = far;
    run.Frames(60);
    AETHER_CHECK(Flat(PositionOf(run, run.scout), far) < 1e-4f);
    // Path rebuild: start it walking down the middle, then drop a crate across the line.
    const Vec3 start(hero.x, 0.9f, hero.z - 9.0f);
    world.GetComponent<Transform>(run.scout)->position = start;
    run.Frames(10);
    const Vec3 walking = PositionOf(run, run.scout);
    AETHER_CHECK(Flat(walking, start) > 0.1f);
    const Vec3 crate(hero.x + (walking.x - hero.x) * 0.5f, 1.0f, hero.z + (walking.z - hero.z) * 0.5f), crate_half(1.2f, 1.0f, 0.4f);
    CarveObstacle(crate, crate_half);
    bool entered = false;
    f32 widest = 0.0f;
    for (int frame = 0; frame < 360; ++frame) {
        run.Frames(1);
        const Vec3 p = PositionOf(run, run.scout);
        if (InBox(p, crate, crate_half, 0.3f)) entered = true;
        widest = std::max(widest, std::abs(p.x - hero.x));
    }
    AETHER_CHECK(!entered);
    AETHER_CHECK(widest > 1.2f); // it went round the crate's end
    AETHER_CHECK(Flat(PositionOf(run, run.scout), hero) < Flat(walking, hero) - 2.0f);
}

AETHER_TEST(Aether01_Scenario_Sentinel) {
    ProofRun run("scenario_sentinel");
    run.Tap(input::Key::E);
    for (int shot = 0; shot < 3; ++shot) { run.Tap(input::Key::MouseLeft); run.Frames(15); }
    AETHER_CHECK(run.Stat(run.hero, "MissionStage") == 2); // research checkpoint saved
    Entity sentinel = FindEntitiesWithTag(run.game->GetWorld(), "Sentinel").front();
    run.game->GetWorld().GetComponent<gas::AttributeSet>(sentinel)->SetBase("Shield", 0);
    run.game->GetWorld().GetComponent<gas::AttributeSet>(sentinel)->SetBase("Health", 2);
    run.Start(); // checkpoint reload restores the Sentinel
    auto& world = run.game->GetWorld();
    sentinel = FindEntitiesWithTag(world, "Sentinel").front();
    AETHER_CHECK(run.Stat(run.hero, "MissionStage") == 2);
    AETHER_CHECK(run.Stat(sentinel, "Shield") == 6 && run.Stat(sentinel, "Health") == 6 && run.Stat(sentinel, "AttackState") == 0);
    Aether01Ai ai(world);
    run.Frames(120);
    AETHER_CHECK(run.Stat(sentinel, "AttackState") == 1); // detection through the open doorway
    {
        // The debugger sees the decision and the perception (ticked by the game) behind it.
        const auto rows = run.game->AiDebug();
        const auto* row = DebugOf(rows, sentinel);
        AETHER_CHECK(row != nullptr);
        if (row != nullptr) {
            AETHER_CHECK(row->state == "tell" && row->has_perception && row->target == run.hero);
            AETHER_CHECK(row->known.size() == 1 && row->known.front() == run.hero && row->path.empty());
        }
    }
    const Entity gate = ai.BlockDoorway(world);
    run.Frames(1);
    AETHER_CHECK(run.Stat(sentinel, "AttackState") == 0); // loss of target
    {
        const auto rows = run.game->AiDebug();
        const auto* row = DebugOf(rows, sentinel);
        AETHER_CHECK(row != nullptr && row->state == "idle");
        if (row != nullptr) AETHER_CHECK(!row->known.empty()); // remembered, not seen (forgotten after 10 s)
    }
    run.Frames(150);
    AETHER_CHECK(run.Stat(sentinel, "AttackState") == 0 && run.Stat(run.hero, "Health") == 3);
    // The Sentinel is a stationary turret: no path to rebuild. Reopen sight; it reacquires.
    ai.OpenDoorway(world, gate);
    bool reacquired = false;
    for (int i = 0; i < 180 && !reacquired; ++i) { run.Frames(1); reacquired = run.Stat(sentinel, "AttackState") == 1; }
    AETHER_CHECK(reacquired);
}

AETHER_TEST(Aether01_Scenario_Hunter) {
    ProofRun run("scenario_hunter");
    run.Tap(input::Key::E);
    for (int shot = 0; shot < 3; ++shot) { run.Tap(input::Key::MouseLeft); run.Frames(15); }
    Entity hunter = FindEntitiesWithTag(run.game->GetWorld(), "Hunter").front();
    const Vec3 authored = PositionOf(run, hunter);
    run.game->GetWorld().GetComponent<gas::AttributeSet>(hunter)->SetBase("Health", 1);
    run.game->GetWorld().GetComponent<Transform>(hunter)->position.x += 2;
    run.Start(); // checkpoint reload restores the Hunter
    auto& world = run.game->GetWorld();
    hunter = FindEntitiesWithTag(world, "Hunter").front();
    AETHER_CHECK(run.Stat(hunter, "Health") == 4 && run.Stat(hunter, "AttackState") == 0);
    AETHER_CHECK(Flat(PositionOf(run, hunter), authored) < 1e-4f);
    Aether01Ai ai(world);
    const Entity sentinel = FindEntitiesWithTag(world, "Sentinel").front();
    world.GetComponent<gas::AttributeSet>(sentinel)->SetBase("Health", 0); // isolate the Hunter
    world.GetComponent<Transform>(sentinel)->position.y = -100;
    run.Frames(60);
    AETHER_CHECK(Flat(PositionOf(run, hunter), authored) < 1e-4f); // dormant outside its trigger
    // Detection: Kael enters the ambush room.
    run.game->Input().SetButton(input::Key::W, true);
    run.Frames(155);
    run.game->Input().SetButton(input::Key::W, false);
    bool told = false;
    for (int i = 0; i < 200 && !told; ++i) { run.Frames(1); told = run.Stat(hunter, "AttackState") == 1; }
    AETHER_CHECK(told);
    // Loss of target: Kael leaves the trigger mid-tell; the tell is dropped and it holds.
    world.GetComponent<Transform>(run.hero)->position = Vec3(0, 0, 0);
    run.Frames(1);
    AETHER_CHECK(run.Stat(hunter, "AttackState") == 0);
    const Vec3 held = PositionOf(run, hunter);
    run.Frames(60);
    AETHER_CHECK(Flat(PositionOf(run, hunter), held) < 1e-4f && run.Stat(run.hero, "Health") == 3);
    // Path rebuild: Kael back at the room's mouth, the Hunter at the far end; it
    // starts its approach, then a barrier is carved across its line.
    const Vec3 kael(0, 0, -13.2f);
    world.GetComponent<Transform>(run.hero)->position = kael;
    const Vec3 start(0, authored.y, -21.3f);
    world.GetComponent<Transform>(hunter)->position = start;
    run.Frames(5);
    AETHER_CHECK(PositionOf(run, hunter).z > start.z + 0.05f);
    {
        // The debugger shows the path it is walking (Nav.MoveTowards' cache) toward Kael.
        const auto rows = run.game->AiDebug();
        const auto* row = DebugOf(rows, hunter);
        AETHER_CHECK(row != nullptr);
        if (row != nullptr) {
            AETHER_CHECK(row->state == "idle" && row->has_perception);
            AETHER_CHECK(row->path.size() >= 2 && row->next_corner < row->path.size() && Flat(row->goal, kael) < 0.3f);
        }
    }
    const Vec3 barrier(0, 1, -19.0f), barrier_half(2.0f, 1.0f, 0.3f);
    CarveObstacle(barrier, barrier_half);
    bool entered = false;
    f32 widest = 0.0f;
    for (int frame = 0; frame < 120 && run.Stat(hunter, "AttackState") == 0; ++frame) {
        world.GetComponent<Transform>(run.hero)->position = kael;
        run.Frames(1);
        const Vec3 p = PositionOf(run, hunter);
        if (InBox(p, barrier, barrier_half, 0.3f)) entered = true;
        widest = std::max(widest, std::abs(p.x));
    }
    AETHER_CHECK(!entered);
    AETHER_CHECK(widest > 2.0f);
    AETHER_CHECK(Flat(PositionOf(run, hunter), kael) < 5.3f); // it reached striking range past the barrier
}

AETHER_TEST(Aether01_Scenario_WardenPhases) {
    ProofRun run("scenario_warden");
    run.Tap(input::Key::E);
    for (int shot = 0; shot < 3; ++shot) { run.Tap(input::Key::MouseLeft); run.Frames(15); }
    run.Start();
    run.ClearResearch();
    run.game->Input().SetButton(input::Key::W, true);
    run.Frames(280);
    run.game->Input().SetButton(input::Key::W, false);
    AETHER_CHECK(run.Stat(run.hero, "MissionStage") == 3); // arena checkpoint saved
    auto& world = run.game->GetWorld();
    const Entity warden = FindEntitiesWithTag(world, "Warden").front();
    const Entity armor = FindEntitiesWithTag(world, "WardenArmor").front();
    const Entity core = FindEntitiesWithTag(world, "WardenCore").front();
    // Detection: the Warden telegraphs a strike at Kael.
    bool told = false;
    for (int i = 0; i < 150 && !told; ++i) { run.Frames(1); told = run.Stat(warden, "AttackState") == 1; }
    AETHER_CHECK(told);
    AETHER_CHECK(run.Stat(warden, "Phase") == 1);
    AETHER_CHECK(PositionOf(run, armor).y > -50 && PositionOf(run, core).y < -50);
    // Phase transitions follow its health: 2 drops the armor, 3 exposes the core.
    auto* stats = world.GetComponent<gas::AttributeSet>(warden);
    const struct { f32 health; f32 phase; bool armor, core; } phases[] = {{9, 2, false, false}, {6, 3, false, true}, {3, 4, false, true}};
    for (const auto& step : phases) {
        stats->SetBase("Health", step.health);
        run.Frames(1);
        AETHER_CHECK(run.Stat(warden, "Phase") == step.phase);
        AETHER_CHECK((PositionOf(run, armor).y > -50) == step.armor);
        AETHER_CHECK((PositionOf(run, core).y > -50) == step.core);
    }
    {
        // Loss of sight: a wall between them cancels the pending strike and it holds
        // ("lost") until it sees Kael again. (Scoped: its perception world must go
        // before this game does.)
        Aether01Ai ai(world);
        told = false;
        for (int i = 0; i < 150 && !told; ++i) { run.Frames(1); told = run.Stat(warden, "AttackState") == 1; }
        AETHER_CHECK(told);
        const Vec3 w = PositionOf(run, warden), k = PositionOf(run, run.hero);
        const f32 health = run.Stat(run.hero, "Health");
        const Entity wall = ai.Block(world, Vec3((w.x + k.x) * 0.5f, 2.0f, (w.z + k.z) * 0.5f), Vec3(6, 4, 0.3f));
        run.Frames(1);
        AETHER_CHECK(run.Stat(warden, "AttackState") == 0);
        for (Entity marker : FindEntitiesWithTag(world, "WardenMarker")) AETHER_CHECK(PositionOf(run, marker).y < -50);
        run.Frames(120);
        AETHER_CHECK(run.Stat(warden, "AttackState") == 0 && run.Stat(run.hero, "Health") == health);
        const auto rows = run.game->AiDebug();
        const auto* row = DebugOf(rows, warden);
        AETHER_CHECK(row != nullptr && row->state == "lost");
        ai.Unblock(world, wall);
        bool reacquired = false;
        for (int i = 0; i < 150 && !reacquired; ++i) { run.Frames(1); reacquired = run.Stat(warden, "AttackState") == 1; }
        AETHER_CHECK(reacquired);
    }
    // Checkpoint reload: back to a full-health phase 1 Warden in the arena.
    run.Start();
    auto& reloaded = run.game->GetWorld();
    const Entity warden2 = FindEntitiesWithTag(reloaded, "Warden").front();
    AETHER_CHECK(run.Stat(run.hero, "MissionStage") == 3);
    AETHER_CHECK(run.Stat(warden2, "Health") == 12 && run.Stat(warden2, "Phase") == 1);
    AETHER_CHECK(PositionOf(run, FindEntitiesWithTag(reloaded, "WardenArmor").front()).y > -50);
    // Loss of target: Kael is down mid-telegraph; the strike is cancelled.
    told = false;
    for (int i = 0; i < 150 && !told; ++i) { run.Frames(1); told = run.Stat(warden2, "AttackState") == 1; }
    AETHER_CHECK(told);
    reloaded.GetComponent<gas::AttributeSet>(run.hero)->SetBase("Health", 0);
    run.Frames(1);
    AETHER_CHECK(run.Stat(warden2, "AttackState") == 0);
    for (Entity marker : FindEntitiesWithTag(reloaded, "WardenMarker")) AETHER_CHECK(PositionOf(run, marker).y < -50);
}

// M8.8 profile: the planned encounter load (24 Scout path followers; the plan
// names no count) on the AETHER-01 navmesh. Prints ms/frame for the whole
// headless tick with and without them; the difference is the AI+nav cost.
// M8.8 profile: baseline vs N scripted nav followers, with and without the
// Nav.MoveTowards path cache. Prints only; timing is never asserted (CI noise).
namespace {
struct ProfileResult {
    double base = 0, loaded = 0;
    int moved = 0;
};
ProfileResult ProfileNavFollowers(int count, bool cache) {
    script::AiDebugStore::SetPathCacheEnabled(cache);
    ProofRun run("profile");
    run.Tap(input::Key::E);
    auto& world = run.game->GetWorld();
    Aether01Ai ai(world);
    const auto time_frames = [&](int frames) {
        const auto t0 = std::chrono::steady_clock::now();
        run.Frames(frames);
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / frames;
    };
    run.Frames(10);
    time_frames(480); // warm up: the opening frames carry one-off work (hints, audio starts)
    ProfileResult r;
    r.base = time_frames(240);
    const ScriptComponent script = *world.GetComponent<ScriptComponent>(run.scout);
    const gas::AttributeSet stats = *world.GetComponent<gas::AttributeSet>(run.scout);
    std::vector<Entity> followers;
    for (int i = 0; i < count; ++i) {
        Transform at;
        // Half in the route room (they path through the doorway), half in the bay;
        // past 24 the extra rows are offset half a cell so nobody spawns stacked.
        const int k = i % 24;
        const f32 off = (i / 24) * 1.3f;
        at.position = k < 12 ? Vec3(-4.0f + (k % 4) * 2.6f + off, 0.9f, -12.0f - (k / 4) * 3.0f)
                             : Vec3(-4.0f + (k % 4) * 2.6f + off, 0.9f, -6.0f + ((k - 12) / 4) * 1.5f);
        followers.push_back(world.CreateEntity(at, ScriptComponent(script), gas::AttributeSet(stats)));
    }
    std::vector<Vec3> spawned;
    for (Entity e : followers) spawned.push_back(world.GetComponent<Transform>(e)->position);
    run.Frames(10);
    r.loaded = time_frames(240);
    for (usize i = 0; i < followers.size(); ++i) r.moved += Flat(world.GetComponent<Transform>(followers[i])->position, spawned[i]) > 0.5f;
    script::AiDebugStore::SetPathCacheEnabled(true);
    std::printf("[profile] AETHER-01 %d nav followers, path cache %s: %.3f ms/frame (baseline %.3f, AI+nav %.3f ms/frame)\n", count,
                cache ? "on " : "off", r.loaded, r.base, r.loaded - r.base);
    return r;
}
} // namespace

AETHER_TEST(Aether01_Profile_24NavFollowers) {
    AETHER_CHECK(ProfileNavFollowers(24, true).moved == 24);
}

AETHER_TEST(Aether01_Profile_NavFollowersCacheAndScaling) {
    AETHER_CHECK(ProfileNavFollowers(24, false).moved == 24);
    AETHER_CHECK(ProfileNavFollowers(48, true).moved == 48);
    AETHER_CHECK(ProfileNavFollowers(48, false).moved == 48);
    AETHER_CHECK(script::AiDebugStore::PathCacheEnabled());
}

#if AETHER_TEST_HAS_PHYSICS
AETHER_TEST(Aether01_CameraShortensBeforeTheRoomWall) {
    ProofRun run("camera");
    run.game->GetWorld().GetComponent<Transform>(run.hero)->position = Vec3(4.6f, 0, 0);
    run.game->Input().AddMouseDelta(-750, 0); // look left; the camera would otherwise enter the right wall
    run.Frames(1);
    const Entity camera = FindEntitiesWithTag(run.game->GetWorld(), "MainCamera").front();
    const Vec3 eye = run.game->GetWorld().GetComponent<Transform>(camera)->position;
    AETHER_CHECK(eye.x < 5.0f && eye.x > 4.6f);
}
#endif
#endif

