#include "aether/cook/cooker.h"
#include "aether/audio/audio_system.h"
#include "aether/gameplay/attribute_set.h"
#include "aether/player/game.h"
#include "aether/scene/components.h"
#include "aether/scene/gameplay.h"
#include "aether/sequencer/sequence_system.h"
#include "test_framework.h"

#include <filesystem>
#include <fstream>
#include <memory>

using namespace aether;
namespace fs = std::filesystem;

#if AETHER_TEST_HAS_SCRIPTING
namespace {
struct ProofRun {
    fs::path root;
    player::GamePackage package;
    std::unique_ptr<player::Game> game;
    Entity hero, scout;

    explicit ProofRun(const char* name) {
        root = fs::temp_directory_path() / "aether01_proof_tests" / (std::string(name) + assets::ToString(assets::NewAssetGuid()));
        fs::create_directories(root);
        const auto source = fs::path(AETHER_REPO_ASSETS_DIR).parent_path() / "games/AETHER-01";
        fs::copy(source, root / "project", fs::copy_options::recursive | fs::copy_options::overwrite_existing);
        cook::CookOptions options;
        options.project_file = root / "project/AETHER-01.aproject";
        options.output_dir = root / "cooked";
        const auto cooked = cook::Cook(options);
        AETHER_CHECK(cooked.ok);
        std::string error;
        AETHER_CHECK(package.Mount(cooked.pak_file.string(), 0, &error));
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
        for (int i = 0; i < count; ++i) game->Tick(1.0f / 60.0f);
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
