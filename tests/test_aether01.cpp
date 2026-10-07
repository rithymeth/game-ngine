#include "aether/cook/cooker.h"
#include "aether/gameplay/attribute_set.h"
#include "aether/player/game.h"
#include "aether/scene/components.h"
#include "aether/scene/gameplay.h"
#include "test_framework.h"

#include <filesystem>
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
        root = fs::temp_directory_path() / "aether01_proof_tests" / name;
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
        game = std::make_unique<player::Game>(package);
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
    }
    f32 Stat(Entity entity, const char* name) {
        const auto* attributes = game->GetWorld().GetComponent<gas::AttributeSet>(entity);
        AETHER_CHECK(attributes != nullptr);
        return attributes->Get(name, -1);
    }
    void Tap(input::Key key) {
        game->Input().SetButton(key, true);
        Frames(1);
        game->Input().SetButton(key, false);
        Frames(1);
    }
};
}

AETHER_TEST(Aether01_CookedCombatAmmoReloadAndScoutDefeat) {
    ProofRun run("combat");
    AETHER_CHECK(run.Stat(run.hero, "Health") == 3);
    AETHER_CHECK(run.Stat(run.hero, "Ammo") == 30);
    for (int shot = 0; shot < 3; ++shot) {
        run.Tap(input::Key::MouseLeft);
        run.Frames(15);
        AETHER_CHECK(run.Stat(run.scout, "Health") == 2 - shot);
    }
    AETHER_CHECK(run.Stat(run.hero, "Ammo") == 27);
    AETHER_CHECK(run.game->GetWorld().GetComponent<Transform>(run.scout)->position.y < -50);
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
    run.Frames(750);
    AETHER_CHECK(run.Stat(run.hero, "Health") == 0);
    run.Tap(input::Key::R);
    AETHER_CHECK(run.Stat(run.hero, "Health") == 3);
    AETHER_CHECK(run.Stat(run.hero, "Ammo") == 30);
    AETHER_CHECK(run.Stat(run.scout, "Health") == 3);
    AETHER_CHECK(run.Stat(run.hero, "DamageFlash") == 0);
}
#endif
