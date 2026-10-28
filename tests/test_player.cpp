#include "aether/assets/asset_guid.h"
#include "aether/audio/sound.h"
#include "aether/audio/audio_system.h"
#include "aether/cook/cooker.h"
#include "aether/pak/pak.h"
#include "aether/player/game.h"
#include "aether/project/project.h"
#include "aether/scene/components.h"
#include "aether/scene/gameplay.h"
#include "aether/platform/window.h"
#include "../player/src/package_textures.h"
#include "../player/src/render_pose.h"
#include "aether/cook/texture_cook.h"
#include "aether/scene/script_component.h"
#include "aether/sequencer/sequence.h"
#include "aether/sequencer/sequence_system.h"
#include "aether/ui/basic.h"
#include "aether/ui/controls.h"
#include "aether/ui/layout_file.h"
#include "aether/ui/panels.h"
#include "aether/ui/viewport.h"
#include "aether/ui/widget_system.h"
#include "aether/reflection/serialize.h"
#include "test_framework.h"

#if AETHER_TEST_PLAYER_PHYSICS
#include "aether/physics/components.h"
#include "aether/physics/net_physics_bridge.h"
#include "aether/net/physics_state.h"
#include "aether/net/interpolation.h"
#include "aether/server/dedicated_server.h"
#endif

#include <nlohmann/json.hpp>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

// Phase 25 step 4: the player's runtime - the manifest, mounting cooked
// archives, the startup scene with its prefabs, and the frame loop.

using namespace aether;
using namespace aether::player;
using nlohmann::json;
namespace stdfs = std::filesystem;

#define CHECK(...) AETHER_CHECK((__VA_ARGS__))

namespace {

stdfs::path TestDir(const std::string& name) {
    const stdfs::path dir = stdfs::temp_directory_path() / "aether_player_tests" / name;
    stdfs::remove_all(dir);
    stdfs::create_directories(dir);
    return dir;
}

json Manifest(const std::string& startup, const std::string& prefab_guid, f32 hz = 60.0f) {
    return {{"$type", "CookManifest"},
            {"$version", 1},
            {"project", "Demo"},
            {"configuration", "Development"},
            {"startup_scene", startup},
            {"fixed_timestep_hz", hz},
            {"gravity", {0.0, -9.81, 0.0}},
            {"layers", {"Default"}},
            {"collision_matrix", json::array()},
            {"assets", json::array({{{"guid", prefab_guid}, {"path", "Prefabs/crate.aprefab"}, {"importer", "Prefab"}},
                                    {{"guid", assets::ToString(assets::NewAssetGuid())},
                                     {"path", "Scenes/start.ascene"},
                                     {"importer", "Scene"}}})},
            {"files", json::array()}};
}

json Transform3(f32 x, f32 y, f32 z) {
    return reflect::ToJson(Transform{Vec3(x, y, z), Quaternion::Identity()});
}

json TagsJson(const std::string& tag) {
    Tags t;
    t.names = {tag};
    return reflect::ToJson(t);
}

// A pak: the startup scene has a tagged player and an instance of a crate
// prefab, whose one entity sits at (1, 2, 3).
stdfs::path MakePak(const stdfs::path& dir, const std::string& name, f32 hz = 60.0f, const std::string& player_tag = "Player") {
    const std::string prefab = assets::ToString(assets::NewAssetGuid());
    const json crate = {{"$type", "Prefab"},
                        {"$version", 1},
                        {"entities", json::array({{{"id", 1}, {"parent", 0}, {"components", {{"Transform", Transform3(1, 2, 3)}}}}})}};
    const json scene = {{"$type", "Scene"},
                        {"$version", 1},
                        {"entities", json::array({{{"components", {{"Transform", Transform3(0, 0, 0)},
                                                                   {"Tags", TagsJson(player_tag)}}}},
                                                  {{"components", {{"PrefabInstance", {{"source", prefab}}}}}}})}};
    pak::PakWriter writer;
    writer.Add("Manifest.json", Manifest("Scenes/start.ascene", prefab, hz).dump(2));
    writer.Add("Content/Scenes/start.ascene", scene.dump(2));
    writer.Add("Content/Prefabs/crate.aprefab", crate.dump(2));
    const stdfs::path file = dir / name;
    std::string error;
    AETHER_CHECK(writer.Write(file.string(), &error));
    return file;
}

Entity FindTagged(World& world, const std::string& tag) {
    const std::vector<Entity> all = FindEntitiesWithTag(world, tag);
    return all.empty() ? Entity{} : all.front();
}

} // namespace

namespace {
stdfs::path MakeAudioPak(const stdfs::path& dir, bool sequence = false, bool bound = false, bool bad_wave = false) {
    const EntityGuid emitter{0xA001, 0xB002};
    AudioSource source;
    source.cue = "Audio/tone.acue";
    source.auto_play = !sequence;
    SequenceComponent component;
    component.sequence = "Sequences/audio.asequence";
    component.auto_play = sequence;
    const json scene = {{"$type", "Scene"}, {"$version", 1},
        {"entities", json::array({{{"guid", ToString(emitter)}, {"components", {{"Transform", Transform3(2, 0, 0)},
            {"Tags", TagsJson("Emitter")}, {"AudioSource", reflect::ToJson(source)}, {"AudioListener", reflect::ToJson(AudioListener{})}}}},
            {{"components", {{"SequenceComponent", reflect::ToJson(component)}}}}})}};
    audio::SoundCue cue;
    cue.name = "Tone";
    cue.root = 1;
    audio::CueNode wave;
    wave.id = 1;
    wave.sound = "Audio/tone.wav";
    cue.nodes.push_back(wave);
    seq::LevelSequence timeline;
    timeline.name = "Audio proof";
    timeline.duration = 2;
    seq::Track track;
    track.id = "sound";
    track.type = seq::TrackType::Audio;
    if (bound) track.binding = emitter;
    track.audio = {{0.1f, "Audio/tone.acue", seq::AudioAction::FadeIn, 0, 0.1f},
                   {0.8f, "Audio/tone.acue", seq::AudioAction::FadeOut, 0, 0.1f}};
    timeline.tracks.push_back(track);
    json manifest = Manifest("Scenes/start.ascene", "unused");
    manifest["assets"] = json::array();
    for (const auto* path : {"Scenes/start.ascene", "Audio/tone.wav", "Audio/tone.acue", "Sequences/audio.asequence"})
        manifest["assets"].push_back({{"guid", assets::ToString(assets::NewAssetGuid())}, {"path", path}, {"importer", "Test"}});
    pak::PakWriter writer;
    CHECK(writer.Add("Manifest.json", manifest.dump()));
    CHECK(writer.Add("Content/Scenes/start.ascene", scene.dump()));
    CHECK(writer.Add("Content/Audio/tone.acue", audio::SaveCue(cue)));
    CHECK(writer.Add("Content/Sequences/audio.asequence", seq::SequenceToJson(timeline).dump()));
    if (bad_wave) CHECK(writer.Add("Content/Audio/tone.wav", std::string("invalid WAV")));
    else CHECK(writer.Add("Content/Audio/tone.wav", audio::EncodeWav(audio::GenerateTone(440, 2))));
    const auto path = dir / "Game.apak";
    CHECK(writer.Write(path.string()));
    return path;
}
}

AETHER_TEST(Player_CookedAudioSourceListenerSettingsAndSceneReload) {
    const auto dir = TestDir("Audio");
    GamePackage package;
    CHECK(package.Mount(MakeAudioPak(dir).string()));
    Game game(package);
    CHECK(game.LoadStartupScene());
    game.BeginPlay();
    for (int i = 0; i < 3; ++i) game.Tick(1.0f / 60);
    auto* system = game.AudioSystem();
    CHECK(system != nullptr);
    CHECK(system->Problems().empty());
    CHECK(system->ListenerEntity() == FindTagged(game.GetWorld(), "Emitter"));
    CHECK(system->GetListener().position.x == 2);
    CHECK(system->GetMixer().FramesRendered() >= 2300);
    CHECK(system->GetMixer().Meter(audio::kMasterBus).peak[0] > 0.1f);
    auto settings = game.Settings().Get();
    settings.master = 0;
    game.Settings().Set(settings);
    for (int i = 0; i < 5; ++i) game.Tick(1.0f / 60); // mixer ramps the gain change
    CHECK(system->GetMixer().BusVolume(audio::kMasterBus) == -80);
    CHECK(system->GetMixer().Meter(audio::kMasterBus).peak[0] < 0.001f);
    game.EndPlay();
    CHECK(system->Player().ActiveCount() == 0);
    CHECK(game.LoadStartupScene());
    game.BeginPlay();
    game.Tick(1.0f / 60);
    CHECK(game.AudioSystem()->GetMixer().BusVolume(audio::kMasterBus) == -80);
    CHECK(game.AudioSystem()->Problems().empty() && game.Warnings().empty());
}

AETHER_TEST(Player_SequenceAudioFadeInAndFadeOutBoundAndUnbound) {
    for (bool bound : {false, true}) {
        const auto dir = TestDir(bound ? "BoundAudio" : "SequenceAudio");
        GamePackage package;
        CHECK(package.Mount(MakeAudioPak(dir, true, bound).string()));
        Game game(package);
        CHECK(game.LoadStartupScene());
        game.BeginPlay();
        for (int i = 0; i < 20; ++i) game.Tick(1.0f / 60);
        auto* system = game.AudioSystem();
        CHECK(system->Player().ActiveCount() == 1);
        CHECK(system->GetMixer().Meter(audio::kMasterBus).peak[0] > 0.1f);
        for (int i = 0; i < 60; ++i) game.Tick(1.0f / 60);
        CHECK(system->Player().ActiveCount() == 0);
        CHECK(system->GetMixer().VoiceCount() == 0);
        CHECK(system->Problems().empty());
        CHECK(game.Warnings().empty());
    }
}

AETHER_TEST(Player_InvalidCookedAudioReportsAWarning) {
    GamePackage package;
    CHECK(package.Mount(MakeAudioPak(TestDir("BadAudio"), false, false, true).string()));
    Game game(package);
    CHECK(game.LoadStartupScene());
    CHECK(game.Warnings().size() == 1 && game.Warnings().front().find("tone.wav") != std::string::npos);
    game.BeginPlay();
    game.Tick(1.0f / 60);
    CHECK(!game.AudioSystem()->Problems().empty());
}

AETHER_TEST(Player_CooksSoundCueAssetsAndPlaysWithoutSourceFiles) {
    const auto dir = TestDir("CookedAudio");
    ProjectPaths paths;
    std::string error;
    CHECK(CreateProject(dir, "Audio proof", &paths, &error));
    stdfs::create_directories(paths.content / "Audio");
    audio::SoundCue cue;
    cue.root = 1;
    audio::CueNode wave;
    wave.id = 1;
    wave.sound = "Audio/tone.wav";
    cue.nodes.push_back(wave);
    std::ofstream(paths.content / "Audio/tone.acue") << audio::SaveCue(cue);
    const auto samples = audio::EncodeWav(audio::GenerateTone(330, 0.5f));
    {
        std::ofstream output(paths.content / "Audio/tone.wav", std::ios::binary);
        output.write(reinterpret_cast<const char*>(samples.data()), static_cast<std::streamsize>(samples.size()));
    }
    AudioSource source;
    source.cue = "Audio/tone.acue";
    const json scene = {{"$type", "Scene"}, {"$version", 1},
        {"entities", json::array({{{"components", {{"Transform", Transform3(0, 0, 0)}, {"AudioSource", reflect::ToJson(source)}}}}})}};
    stdfs::create_directories(paths.content / "Scenes");
    std::ofstream(paths.content / "Scenes/main.ascene") << scene.dump();
    ProjectSettings settings;
    CHECK(LoadProject(paths.file, settings, &error));
    settings.startup_scene = "Scenes/main.ascene";
    settings.always_cook = {"Audio/"}; // cue/wave strings are paths, not serialized GUID dependencies
    CHECK(SaveProject(paths.file, settings, &error));
    cook::CookOptions options;
    options.project_file = paths.file;
    options.output_dir = dir / "cooked";
    const auto cooked = cook::Cook(options);
    CHECK(cooked.ok);
    // This fixture owns Content; playback must use only the mounted pak.
    stdfs::remove_all(paths.content);
    GamePackage package;
    CHECK(package.Mount(cooked.pak_file.string()));
    CHECK(package.LoadManifest());
    const auto* asset = package.FindAsset("Audio/tone.acue");
    CHECK(asset != nullptr && asset->importer == "SoundCue");
    Game game(package);
    CHECK(game.LoadStartupScene());
    game.BeginPlay();
    game.Tick(1.0f / 60);
    CHECK(game.AudioSystem()->GetMixer().Meter(audio::kMasterBus).peak[0] > 0.1f);
    CHECK(game.Warnings().empty() && game.AudioSystem()->Problems().empty());
}

AETHER_TEST(Player_SceneReplacementKeepsCustomStagesAndRefreshesKitSystems) {
    const auto dir = TestDir("ReloadStages");
    GamePackage package;
    CHECK(package.Mount(MakePak(dir, "Game.apak").string()));
    Game game(package);
    CHECK(game.LoadStartupScene());
    int ticks = 0;
    SystemDesc custom;
    custom.name = "Proof.Custom";
    custom.phase = SystemPhase::LateUpdate;
    custom.run = [&](World&, const FrameContext&) { ++ticks; };
    CHECK(game.Systems().Add(std::move(custom)));
    for (int scene = 0; scene < 3; ++scene) {
        if (scene > 0) CHECK(game.LoadStartupScene());
        game.BeginPlay();
        for (int frame = 0; frame < 5; ++frame) game.Tick(1.0f / 60);
    }
    CHECK(ticks == 15);
    CHECK(game.ScriptErrors().empty() && game.BlueprintErrors().empty());
}

AETHER_TEST(Player_ParsesTheManifest) {
    GameManifest m;
    std::string error;
    CHECK(ParseGameManifest(Manifest("Scenes/start.ascene", "g", 30.0f).dump(), m, &error));
    CHECK(m.project == "Demo" && m.configuration == cook::BuildConfiguration::Development);
    CHECK(m.startup_scene == "Scenes/start.ascene" && m.fixed_timestep_hz == 30.0f && m.gravity.y < -9.0f);
    CHECK(m.assets.size() == 2 && m.assets[0].path == "Prefabs/crate.aprefab" && m.layers.size() == 1);

    CHECK(!ParseGameManifest("not json", m, &error) && error.find("isn't JSON") != std::string::npos);
    json bad = Manifest("s", "g");
    bad["$type"] = "Scene";
    CHECK(!ParseGameManifest(bad.dump(), m, &error) && error.find("cook manifest") != std::string::npos);
    bad = Manifest("s", "g");
    bad["configuration"] = "Fast";
    CHECK(!ParseGameManifest(bad.dump(), m, &error) && error.find("configuration") != std::string::npos);
    bad = Manifest("s", "g", 0.0f);
    CHECK(!ParseGameManifest(bad.dump(), m, &error) && error.find("timestep") != std::string::npos);
    bad = Manifest("s", "g");
    bad["assets"].push_back({{"path", "x"}});
    CHECK(!ParseGameManifest(bad.dump(), m, &error) && error.find("no GUID") != std::string::npos);
}

AETHER_TEST(Player_RunsTheStartupSceneFromAPak) {
    const stdfs::path dir = TestDir("Run");
    const stdfs::path pak = MakePak(dir, "Game.apak");
    GamePackage package;
    std::string error;
    CHECK(package.Mount(pak.string(), 0, &error));
    CHECK(package.LoadManifest(&error) && package.Manifest().project == "Demo");
    CHECK(package.FindAsset("Prefabs/crate.aprefab") != nullptr && package.FindAsset("Nope") == nullptr);
    CHECK(GamePackage::FindPaks(dir) == std::vector<std::string>{pak.string()});

    Game game(package);
    CHECK(game.LoadStartupScene(&error));
    CHECK(game.SceneName() == "Scenes/start.ascene" && game.Warnings().empty());
    World& world = game.GetWorld();
    CHECK(!FindTagged(world, "Player").IsNull());
    // The crate prefab's entity came from the pak, at its prefab position.
    bool crate = false;
    world.ForEach<Transform>([&](const Transform& t) {
        crate |= std::fabs(t.position.x - 1) < 1e-4f && std::fabs(t.position.y - 2) < 1e-4f && std::fabs(t.position.z - 3) < 1e-4f;
    });
    CHECK(crate && game.Stats().prefab_instances == 1);

    // Lifecycle callbacks run in play; one second at 60 Hz is 60 fixed steps.
    int updates = 0, fixed = 0, started = 0;
    LifecycleCallbacks callbacks;
    callbacks.on_start = [&](Entity) { ++started; };
    callbacks.on_update = [&](Entity, f32) { ++updates; };
    callbacks.on_fixed_update = [&](Entity, f32) { ++fixed; };
    game.GetLifecycle().Register<Tags>(callbacks);
    game.BeginPlay();
    CHECK(game.IsPlaying() && started == 1);
    for (int i = 0; i < 60; ++i) game.Tick(1.0f / 60.0f);
    const GameStats s = game.Stats();
    CHECK(s.frames == 60 && s.fixed_steps == 60 && std::fabs(s.time - 1.0) < 1e-4);
    CHECK(updates == 60 && fixed == 60);
    game.EndPlay();
    CHECK(!game.IsPlaying());
}

// A game with its own UI: a WidgetComponent shows a layout from the pak, a script changes it by widget name (the Luau UI table), and the
// player's viewport holds it for the renderer to draw.
AETHER_TEST(Player_ShowsADataDrivenUiThatScriptsChange) {
    if (!Game::HasScripting()) return;
    const stdfs::path dir = TestDir("Ui");
    auto root = std::make_unique<ui::Canvas>();
    root->name = "Hud";
    root->Add<ui::Text>("Score 0")->name = "Score";
    root->Add<ui::ProgressBar>()->name = "Health";
    root->Add<ui::Text>("secret")->name = "Secret";
    root->Add<ui::Button>()->name = "Play";
    root->FindAs<ui::Button>("Play")->Add<ui::Text>("Play");
    root->Add<ui::Text>("idle")->name = "Status";
    auto* icon = root->Add<ui::Image>();
    icon->brush.kind = ui::Brush::Kind::Image;
    icon->brush.image = "UI/icon.png";
    icon->desired_size = {32, 32};
    const std::string layout = ui::SaveLayout(*root);
    const std::string script_guid = assets::ToString(assets::NewAssetGuid());
    const char* script =
        "local Hud = {}\n"
        "function Hud:OnUpdate(dt)\n"
        "  self.t = (self.t or 0) + dt\n"
        "  UI.SetText('Score', 'Score ' .. math.floor(self.t * 10))\n"
        "  UI.SetValue('Health', 0.25)\n"
        "  UI.SetVisible('Secret', false)\n"
        "  Hud.found, Hud.missing = UI.Exists('Score'), UI.SetText('Nope', 'x')\n"
        "  if UI.WasClicked('Play') then UI.SetText('Status', 'clicked') end\n"
        "end\n"
        "return Hud\n";
    ScriptComponent component;
    assets::AssetGuid parsed;
    CHECK(assets::ParseAssetGuid(script_guid, parsed));
    component.script.guid = parsed;
    const json scene = {{"$type", "Scene"}, {"$version", 1},
        {"entities", json::array({{{"components", {{"WidgetComponent", {{"layout", "UI/Hud.aui"}, {"z", 0}, {"visible", true}}}}}},
                                  {{"components", {{"ScriptComponent", reflect::ToJson(component)}}}}})}};
    json manifest = Manifest("Scenes/start.ascene", "unused");
    manifest["assets"] = json::array({{{"guid", assets::ToString(assets::NewAssetGuid())}, {"path", "Scenes/start.ascene"}, {"importer", "Scene"}},
                                      {{"guid", script_guid}, {"path", "Scripts/Hud.luau"}, {"importer", "Script"}},
                                      {{"guid", assets::ToString(assets::NewAssetGuid())}, {"path", "UI/Hud.aui"}, {"importer", "Test"}}});
    pak::PakWriter writer;
    CHECK(writer.Add("Manifest.json", manifest.dump()));
    CHECK(writer.Add("Content/Scenes/start.ascene", scene.dump()));
    CHECK(writer.Add("Content/Scripts/Hud.luau", std::string(script)));
    CHECK(writer.Add("Content/UI/Hud.aui", layout));
    const stdfs::path pak = dir / "Game.apak";
    CHECK(writer.Write(pak.string()));

    GamePackage package;
    std::string error;
    CHECK(package.Mount(pak.string(), 0, &error));
    CHECK(package.LoadManifest(&error));
    Game game(package);
    CHECK(game.LoadStartupScene(&error));
    CHECK(game.Ui() != nullptr && game.Ui()->LayerCount() == 0); // shown on the first frame
    game.BeginPlay();
    for (int i = 0; i < 12; ++i) game.Tick(1.0f / 60.0f);
    CHECK(game.ScriptErrors().empty() && game.Warnings().empty());
    ui::Viewport* viewport = game.Ui();
    CHECK(viewport->LayerCount() == 1);
    auto* score = dynamic_cast<ui::Text*>(viewport->Find("Score"));
    CHECK(score != nullptr && score->text != "Score 0" && score->text.rfind("Score ", 0) == 0);
    auto* health = dynamic_cast<ui::ProgressBar*>(viewport->Find("Health"));
    CHECK(health != nullptr && std::fabs(health->percent - 0.25f) < 1e-4f);
    CHECK(viewport->Find("Secret")->visibility == ui::Visibility::Collapsed);
    // The first draw: layout and paint produce quads for the visible widgets.
    ui::BuiltinFont font;
    viewport->SetSize({1280, 720});
    viewport->Layout(font);
    const ui::DrawList painted = viewport->Paint(font);
    CHECK(!painted.quads.empty());
    // The icon's quad names its image by id; the game maps it back to the layout's path.
    bool icon_found = false;
    for (const ui::DrawQuad& quad : painted.quads) icon_found = icon_found || (quad.texture != 0 && game.UiImagePath(quad.texture) == "UI/icon.png");
    CHECK(icon_found && game.UiImagePath(0).empty() && game.UiImagePath(99).empty());
    // The keys drive it: Tab focuses the button, Enter presses it, and the script sees the click on a later frame.
    const auto tap = [&](input::Key key) {
        game.Input().SetButton(key, true);
        game.Tick(1.0f / 60.0f);
        game.Input().SetButton(key, false);
        game.Tick(1.0f / 60.0f);
    };
    tap(input::Key::Tab);
    tap(input::Key::Enter);
    for (int i = 0; i < 4; ++i) game.Tick(1.0f / 60.0f);
    auto* status = dynamic_cast<ui::Text*>(viewport->Find("Status"));
    CHECK(status != nullptr && status->text == "clicked");
    CHECK(game.ScriptErrors().empty());
    game.EndPlay();
}

// On-screen controls: the pointers (the mouse, each finger) press the UI's buttons and drag a virtual stick, a press the UI takes is hidden
// from gameplay, and a button or stick writes the keys it names so the game's actions follow.
AETHER_TEST(Player_PointersDriveTheUiAndVirtualControls) {
    if (!Game::HasScripting()) return;
    const stdfs::path dir = TestDir("UiPointers");
    auto root = std::make_unique<ui::Canvas>();
    root->name = "Hud";
    auto* play = root->Add<ui::Button>();
    play->name = "Play";
    play->hold_key = "GamepadA";
    play->slot.anchors = ui::Anchors::Point(0, 0);
    play->slot.position = {100, 100};
    play->slot.size = {200, 80};
    play->Add<ui::Text>("Play");
    auto* stick = root->Add<ui::VirtualStick>();
    stick->name = "Stick";
    stick->radius = 60.0f;
    stick->slot.anchors = ui::Anchors::Point(0, 0);
    stick->slot.position = {400, 100};
    stick->slot.size = {200, 200};
    root->Add<ui::Text>("idle")->name = "Status";
    const std::string layout = ui::SaveLayout(*root);
    const std::string script_guid = assets::ToString(assets::NewAssetGuid());
    const char* script =
        "local Hud = {}\n"
        "function Hud:OnUpdate(dt)\n"
        "  if UI.WasClicked('Play') then UI.SetText('Status', 'clicked') end\n"
        "end\n"
        "return Hud\n";
    ScriptComponent component;
    assets::AssetGuid parsed;
    CHECK(assets::ParseAssetGuid(script_guid, parsed));
    component.script.guid = parsed;
    const json scene = {{"$type", "Scene"}, {"$version", 1},
        {"entities", json::array({{{"components", {{"WidgetComponent", {{"layout", "UI/Hud.aui"}, {"z", 0}, {"visible", true}}}}}},
                                  {{"components", {{"ScriptComponent", reflect::ToJson(component)}}}}})}};
    json manifest = Manifest("Scenes/start.ascene", "unused");
    manifest["assets"] = json::array({{{"guid", assets::ToString(assets::NewAssetGuid())}, {"path", "Scenes/start.ascene"}, {"importer", "Scene"}},
                                      {{"guid", script_guid}, {"path", "Scripts/Hud.luau"}, {"importer", "Script"}},
                                      {{"guid", assets::ToString(assets::NewAssetGuid())}, {"path", "UI/Hud.aui"}, {"importer", "Test"}}});
    pak::PakWriter writer;
    CHECK(writer.Add("Manifest.json", manifest.dump()));
    CHECK(writer.Add("Content/Scenes/start.ascene", scene.dump()));
    CHECK(writer.Add("Content/Scripts/Hud.luau", std::string(script)));
    CHECK(writer.Add("Content/UI/Hud.aui", layout));
    const stdfs::path pak = dir / "Game.apak";
    CHECK(writer.Write(pak.string()));

    GamePackage package;
    std::string error;
    CHECK(package.Mount(pak.string(), 0, &error));
    CHECK(package.LoadManifest(&error));
    Game game(package);
    CHECK(game.LoadStartupScene(&error));
    game.BeginPlay();
    for (int i = 0; i < 6; ++i) game.Tick(1.0f / 60.0f); // the layout appears on the first frame
    ui::Viewport* viewport = game.Ui();
    CHECK(viewport != nullptr && viewport->LayerCount() == 1);
    viewport->scale_settings.rule = ui::ScaleSettings::Rule::None; // one layout unit per pixel
    viewport->SetSize({1280, 720});
    ui::BuiltinFont font;
    viewport->Layout(font);
    CHECK(dynamic_cast<ui::Button*>(viewport->Find("Play"))->Geometry().w == 200.0f);

    using input::Key;
    input::InputState& in = game.Input();
    // A press on empty space is for gameplay; the UI leaves the mouse button alone.
    in.SetPointer(0, 900, 600, true, true);
    in.SetButton(Key::MouseLeft, true);
    game.Tick(1.0f / 60.0f);
    CHECK(in.IsDown(Key::MouseLeft) && !in.IsDown(Key::GamepadA));
    in.SetPointer(0, 900, 600, false, true);
    in.SetButton(Key::MouseLeft, false);
    game.Tick(1.0f / 60.0f);

    // A press on the button is the UI's: the button holds the pad's A, gameplay does not see the mouse button, and the release clicks.
    in.SetPointer(0, 150, 130, false, true);
    game.Tick(1.0f / 60.0f); // hover
    in.SetPointer(0, 150, 130, true, true);
    in.SetButton(Key::MouseLeft, true);
    game.Tick(1.0f / 60.0f);
    CHECK(dynamic_cast<ui::Button*>(viewport->Find("Play"))->IsPressed());
    CHECK(in.IsDown(Key::GamepadA) && !in.IsDown(Key::MouseLeft));
    in.SetPointer(0, 150, 130, false, true);
    in.SetButton(Key::MouseLeft, false);
    game.Tick(1.0f / 60.0f);
    CHECK(!in.IsDown(Key::GamepadA)); // released with the button
    for (int i = 0; i < 4; ++i) game.Tick(1.0f / 60.0f);
    auto* status = dynamic_cast<ui::Text*>(viewport->Find("Status"));
    CHECK(status != nullptr && status->text == "clicked");

    // A finger on the stick: dragging 30 pixels right of where it landed is half of the stick's 60 pixel reach.
    const auto touch = [&](WindowEventType type, bool down, f32 x, f32 y) {
        WindowEvent e;
        e.type = type, e.pointer = 5, e.down = down, e.x = x, e.y = y;
        const WindowEvent events[] = {e};
        ApplyWindowEvents(events, in);
    };
    touch(WindowEventType::Touch, true, 500, 200);
    game.Tick(1.0f / 60.0f);
    CHECK(!in.IsDown(Key::MouseLeft)); // the first finger is the left button, and the stick took it
    touch(WindowEventType::TouchMove, true, 530, 200);
    game.Tick(1.0f / 60.0f);
    CHECK(std::fabs(in.Value(Key::GamepadLeftStickX) - 0.5f) < 1e-3f && std::fabs(in.Value(Key::GamepadLeftStickY)) < 1e-3f);
    touch(WindowEventType::TouchMove, true, 500, 100); // far above: clamped to full deflection, up is negative
    game.Tick(1.0f / 60.0f);
    CHECK(std::fabs(in.Value(Key::GamepadLeftStickY) + 1.0f) < 1e-3f && std::fabs(in.Value(Key::GamepadLeftStickX)) < 1e-3f);
    touch(WindowEventType::Touch, false, 500, 100);
    game.Tick(1.0f / 60.0f);
    CHECK(in.Value(Key::GamepadLeftStickX) == 0.0f && in.Value(Key::GamepadLeftStickY) == 0.0f);

    // Drawn at three quarters of the window's size (a quality preset's resolution scale), the button that is at 150, 130 in the UI is at 200, 173
    // in window pixels: the host's scale makes that press hit it.
    game.SetUiPointerScale(0.75f, 0.75f);
    in.SetPointer(0, 200, 173, false, true);
    game.Tick(1.0f / 60.0f);
    in.SetPointer(0, 200, 173, true, true);
    in.SetButton(Key::MouseLeft, true);
    game.Tick(1.0f / 60.0f);
    CHECK(dynamic_cast<ui::Button*>(viewport->Find("Play"))->IsPressed() && in.IsDown(Key::GamepadA));
    in.SetPointer(0, 200, 173, false, true);
    in.SetButton(Key::MouseLeft, false);
    game.Tick(1.0f / 60.0f);
    game.EndPlay();
}

AETHER_TEST(Player_ScaledRenderSizeFollowsTheResolutionScale) {
    RenderSize full = ScaledRenderSize(1280, 720, 1.0f);
    CHECK(full.width == 1280 && full.height == 720);
    RenderSize three_quarters = ScaledRenderSize(1280, 720, 0.75f);
    CHECK(three_quarters.width == 960 && three_quarters.height == 540);
    RenderSize clamped_low = ScaledRenderSize(1280, 720, 0.01f); // held to a quarter
    CHECK(clamped_low.width == 320 && clamped_low.height == 180);
    RenderSize clamped_high = ScaledRenderSize(1280, 720, 2.0f);  // never larger than the window
    CHECK(clamped_high.width == 1280 && clamped_high.height == 720);
    RenderSize tiny = ScaledRenderSize(1, 1, 0.25f);
    CHECK(tiny.width == 1 && tiny.height == 1);
    RenderSize odd = ScaledRenderSize(1001, 563, 0.5f);
    CHECK(odd.width == 501 && odd.height == 282);
}

// The platform's lifecycle (§45.2): suspended, the game stands still and queued saves are written; resumed, it carries on without the time that
// passed; memory pressure is counted.
AETHER_TEST(Player_SuspendStopsTheGameWritesSavesAndResumeSkipsTheGap) {
    const stdfs::path dir = TestDir("Lifecycle");
    const stdfs::path pak = MakePak(dir, "Game.apak");
    GamePackage package;
    std::string error;
    CHECK(package.Mount(pak.string(), 0, &error));
    CHECK(package.LoadManifest(&error));
    Game game(package);
    game.SetUserPaths(ResolveUserPaths("Demo", dir / "user"));
    CHECK(game.LoadStartupScene(&error));
    game.BeginPlay();
    for (int i = 0; i < 30; ++i) game.Tick(1.0f / 60.0f);
    const GameStats before = game.Stats();
    CHECK(before.frames == 30);

    // A save is queued, then the app goes to the background: it is on disk before Dispatch returns.
    save::GameSettings settings;
    settings.language = "fr";
    game.Saves()->SaveAsync("pause", settings);
    platform::MockLifecycleBackend backend(game.Platform());
    backend.InjectSuspend();
    CHECK(game.Suspended());
    CHECK(stdfs::exists(game.Saves()->Directory() / "pause.asav"));
    // Suspended, time does not pass however long the host says it did.
    for (int i = 0; i < 10; ++i) game.Tick(1.0f);
    CHECK(game.Stats().frames == before.frames && std::fabs(game.Stats().time - before.time) < 1e-6);

    // Resumed: the first frame ignores the time that was spent away; then time runs again.
    backend.InjectResume();
    CHECK(!game.Suspended());
    game.Tick(5.0f);
    CHECK(std::fabs(game.Stats().time - before.time) < 1e-6);
    game.Tick(1.0f / 60.0f);
    CHECK(game.Stats().time > before.time && game.Stats().time < before.time + 0.1);

    backend.InjectMemoryPressure(platform::MemoryLevel::Critical);
    backend.InjectMemoryPressure(platform::MemoryLevel::Moderate);
    CHECK(game.MemoryPressureEvents() == 2 && game.CriticalMemoryPressureEvents() == 1);
    game.EndPlay();
}

// A package whose source images were stripped: the renderer's texture loader decodes the cooked copy instead.
AETHER_TEST(Player_LoadsATextureFromItsCookedCopyWhenTheSourceIsStripped) {
    const stdfs::path dir = TestDir("StrippedTexture");
    // A 40 x 24 colour gradient, cooked as ASTC 6x6.
    const u32 w = 40, h = 24;
    std::vector<u8> source(static_cast<usize>(w) * h * 4);
    for (u32 y = 0; y < h; ++y) {
        for (u32 x = 0; x < w; ++x) {
            u8* p = &source[(static_cast<usize>(y) * w + x) * 4];
            p[0] = static_cast<u8>(x * 255 / (w - 1)), p[1] = static_cast<u8>(y * 255 / (h - 1)), p[2] = 128, p[3] = 255;
        }
    }
    cook::TextureCookSettings settings;
    settings.target = cook::TextureTarget::Mobile;
    settings.quality = 3;
    const cook::CookedTexture cooked = cook::CookTexture(source, w, h, settings);
    CHECK(cooked.format == cook::TextureFormat::ASTC_6x6);
    json manifest = Manifest("Scenes/start.ascene", "unused");
    manifest["assets"] = json::array({{{"guid", assets::ToString(assets::NewAssetGuid())}, {"path", "Scenes/start.ascene"}, {"importer", "Scene"}},
                                      {{"guid", assets::ToString(assets::NewAssetGuid())}, {"path", "Textures/grad.png"}, {"importer", "Texture"},
                                       {"cooked", "Cooked/grad.atex"}, {"cooked_format", "astc6x6"}}});
    pak::PakWriter writer;
    CHECK(writer.Add("Manifest.json", manifest.dump()));
    CHECK(writer.Add("Content/Scenes/start.ascene", json{{"$type", "Scene"}, {"$version", 1}, {"entities", json::array()}}.dump()));
    CHECK(writer.Add("Cooked/grad.atex", cook::SaveAtex(cooked))); // no Content/Textures/grad.png
    const stdfs::path pak = dir / "Game.apak";
    CHECK(writer.Write(pak.string()));
    GamePackage package;
    std::string error;
    CHECK(package.Mount(pak.string(), 0, &error));
    CHECK(package.LoadManifest(&error));

    assets::ImageData image;
    CHECK(LoadPackageTexture(package, "Textures/grad.png", image, &error));
    CHECK(image.width == w && image.height == h && image.pixels.size() == source.size());
    f64 worst = 0;
    for (usize i = 0; i < source.size(); ++i) worst = std::max(worst, std::fabs(static_cast<f64>(source[i]) - image.pixels[i]));
    CHECK(worst < 24.0); // lossy, but the same picture
    // Neither the source nor a cooked copy: refused with a reason.
    CHECK(!LoadPackageTexture(package, "Textures/none.png", image, &error) && error.find("none.png") != std::string::npos);
}

AETHER_TEST(Player_FixedRateAndPatchPaks) {
    const stdfs::path dir = TestDir("Patch");
    // The base runs at 30 Hz; a later pak replaces the manifest and scene.
    const stdfs::path base = MakePak(dir, "0_Base.apak", 30.0f, "Player");
    GamePackage package;
    CHECK(package.Mount(base.string(), 0));
    Game game(package);
    CHECK(game.LoadStartupScene());
    game.BeginPlay();
    for (int i = 0; i < 60; ++i) game.Tick(1.0f / 60.0f);
    CHECK(game.Stats().fixed_steps == 30);

    const stdfs::path patch = MakePak(dir, "1_Patch.apak", 60.0f, "Hero");
    const std::vector<std::string> paks = GamePackage::FindPaks(dir);
    CHECK(paks.size() == 2 && paks[1] == patch.string());
    GamePackage patched;
    for (usize i = 0; i < paks.size(); ++i) CHECK(patched.Mount(paks[i], static_cast<int>(i)));
    Game game2(patched);
    CHECK(game2.LoadStartupScene());
    CHECK(!FindTagged(game2.GetWorld(), "Hero").IsNull() && FindTagged(game2.GetWorld(), "Player").IsNull());
}

AETHER_TEST(Player_Refusals) {
    std::string error;
    GamePackage empty;
    CHECK(!empty.LoadManifest(&error) && error.find("Nothing is mounted") != std::string::npos);
    Game no_manifest(empty);
    CHECK(!no_manifest.LoadStartupScene(&error));

    const stdfs::path dir = TestDir("Refuse");
    pak::PakWriter writer;
    writer.Add("Content/readme.txt", std::string("hi"));
    CHECK(writer.Write((dir / "NoManifest.apak").string()));
    GamePackage package;
    CHECK(package.Mount((dir / "NoManifest.apak").string()));
    CHECK(!package.LoadManifest(&error) && error.find("No Manifest.json") != std::string::npos);
    CHECK(!package.Mount((dir / "missing.apak").string(), 0, &error));

    GamePackage good;
    CHECK(good.Mount(MakePak(dir, "Game.apak").string()));
    Game game(good);
    CHECK(!game.LoadScene("Scenes/missing.ascene", &error) && error.find("Scenes/missing.ascene") != std::string::npos);
    CHECK(game.LoadStartupScene(&error));
}

AETHER_TEST(Player_PlaysWhatTheCookerCooked) {
    // A project cooked by aether_cook's library, then played from its pak.
    const stdfs::path dir = TestDir("Cooked");
    ProjectPaths paths;
    std::string error;
    CHECK(CreateProject(dir, "Cooked", &paths, &error));
    const json scene = {{"$type", "Scene"},
                        {"$version", 1},
                        {"entities", json::array({{{"components", {{"Transform", Transform3(4, 5, 6)},
                                                                   {"Tags", TagsJson("Spawn")}}}}})}};
    stdfs::create_directories(paths.content / "Scenes");
    std::ofstream(paths.content / "Scenes/main.ascene", std::ios::binary) << scene.dump(2);
    ProjectSettings settings;
    CHECK(LoadProject(paths.file, settings, &error));
    settings.startup_scene = "Scenes/main.ascene";
    settings.fixed_timestep_hz = 50.0f;
    CHECK(SaveProject(paths.file, settings, &error));

    cook::CookOptions options;
    options.project_file = paths.file;
    options.output_dir = dir / "Build";
    options.configuration = cook::BuildConfiguration::Shipping;
    const cook::CookReport report = cook::Cook(options);
    CHECK(report.ok);

    GamePackage package;
    for (const std::string& pak : GamePackage::FindPaks(dir / "Build")) CHECK(package.Mount(pak));
    CHECK(package.LoadManifest(&error));
    CHECK(package.Manifest().configuration == cook::BuildConfiguration::Shipping && package.Manifest().project == "Cooked");
    Game game(package);
    CHECK(game.LoadStartupScene(&error));
    const Entity spawn = FindTagged(game.GetWorld(), "Spawn");
    CHECK(!spawn.IsNull() && game.GetWorld().GetComponent<Transform>(spawn)->position.y == 5.0f);
    game.BeginPlay();
    for (int i = 0; i < 10; ++i) game.Tick(0.1f);
    CHECK(game.Stats().fixed_steps == 50); // 1 s at 50 Hz
}

// ---------------------------------------------------------------------------
// Level sequences in the player (Phase 27 step 4, §27.4)
// ---------------------------------------------------------------------------

namespace {

seq::LevelSequence ShowSequence(const EntityGuid& actor, const std::string& prefab_path) {
    seq::LevelSequence s;
    s.duration = 2.0f;
    seq::Track move;
    move.id = "move";
    move.type = seq::TrackType::Transform;
    move.binding = actor;
    move.channels = {seq::Channel{"x", {seq::Key{0, 0, seq::Interp::Linear, 0, 0}, seq::Key{1, 10, seq::Interp::Linear, 0, 0}}},
                     seq::Channel{"y", {}}, seq::Channel{"z", {}}};
    seq::Track spawn;
    spawn.id = "crate";
    spawn.type = seq::TrackType::Spawn;
    seq::SpawnKey key;
    key.time = 0.5f;
    key.duration = 0.0f;
    key.prefab = prefab_path;
    key.position = Vec3(7, 8, 9);
    spawn.spawns = {key};
    s.tracks = {move, spawn};
    return s;
}

} // namespace

AETHER_TEST(Player_PlaysASequenceFromAPak) {
    const stdfs::path dir = TestDir("Sequence");
    const std::string prefab = assets::ToString(assets::NewAssetGuid());
    const EntityGuid actor{0xA11CE, 0xB0B};
    const json crate = {{"$type", "Prefab"},
                        {"$version", 1},
                        {"entities", json::array({{{"id", 1}, {"parent", 0}, {"components", {{"Transform", Transform3(0, 0, 0)}, {"Tags", TagsJson("Crate")}}}}})}};
    SequenceComponent player_sequence;
    player_sequence.sequence = "Sequences/Show.asequence";
    player_sequence.auto_play = true;
    const json scene = {{"$type", "Scene"},
                        {"$version", 1},
                        {"entities", json::array({{{"guid", ToString(actor)}, {"components", {{"Transform", Transform3(0, 0, 0)}, {"Tags", TagsJson("Actor")}}}},
                                                  {{"components", {{"SequenceComponent", reflect::ToJson(player_sequence)}}}}})}};
    json manifest = Manifest("Scenes/start.ascene", prefab);
    manifest["assets"] = json::array({{{"guid", prefab}, {"path", "Prefabs/crate.aprefab"}, {"importer", "Prefab"}},
                                      {{"guid", assets::ToString(assets::NewAssetGuid())}, {"path", "Scenes/start.ascene"}, {"importer", "Scene"}},
                                      {{"guid", assets::ToString(assets::NewAssetGuid())}, {"path", "Sequences/Show.asequence"}, {"importer", "Sequence"}}});
    pak::PakWriter writer;
    writer.Add("Manifest.json", manifest.dump(2));
    writer.Add("Content/Scenes/start.ascene", scene.dump(2));
    writer.Add("Content/Prefabs/crate.aprefab", crate.dump(2));
    writer.Add("Content/Sequences/Show.asequence", seq::SequenceToJson(ShowSequence(actor, "Prefabs/crate.aprefab")).dump(2));
    const stdfs::path file = dir / "Game.apak";
    std::string error;
    CHECK(writer.Write(file.string(), &error));

    GamePackage package;
    CHECK(package.Mount(file.string(), 0, &error));
    CHECK(package.LoadManifest(&error));
    Game game(package);
    CHECK(game.LoadStartupScene(&error));
    game.BeginPlay();
    const Entity mover = FindTagged(game.GetWorld(), "Actor");
    CHECK(!mover.IsNull());
    CHECK(FindTagged(game.GetWorld(), "Crate").IsNull()); // the prefab appears at t = 0.5
    for (int i = 0; i < 4; ++i) game.Tick(0.25f); // t = 1.0
    CHECK(std::fabs(game.GetWorld().GetComponent<Transform>(mover)->position.x - 10.0f) < 0.01f);
    const Entity crate_entity = FindTagged(game.GetWorld(), "Crate");
    CHECK(!crate_entity.IsNull());
    if (!crate_entity.IsNull()) {
        const Transform* t = game.GetWorld().GetComponent<Transform>(crate_entity);
        CHECK(t->position.x == 7.0f && t->position.y == 8.0f && t->position.z == 9.0f);
    }
    for (int i = 0; i < 4; ++i) game.Tick(0.25f); // past the end: it stays at the last key
    CHECK(std::fabs(game.GetWorld().GetComponent<Transform>(mover)->position.x - 10.0f) < 0.01f);
    CHECK(game.Warnings().empty());
}

AETHER_TEST(Player_WarnsAboutAMissingSequence) {
    const stdfs::path dir = TestDir("MissingSequence");
    const std::string prefab = assets::ToString(assets::NewAssetGuid());
    SequenceComponent c;
    c.sequence = "Sequences/Nope.asequence";
    c.auto_play = true;
    const json scene = {{"$type", "Scene"},
                        {"$version", 1},
                        {"entities", json::array({{{"components", {{"SequenceComponent", reflect::ToJson(c)}}}}})}};
    pak::PakWriter writer;
    writer.Add("Manifest.json", Manifest("Scenes/start.ascene", prefab).dump(2));
    writer.Add("Content/Scenes/start.ascene", scene.dump(2));
    const stdfs::path file = dir / "Game.apak";
    std::string error;
    CHECK(writer.Write(file.string(), &error));
    GamePackage package;
    CHECK(package.Mount(file.string(), 0, &error));
    CHECK(package.LoadManifest(&error));
    Game game(package);
    CHECK(game.LoadStartupScene(&error));
    game.BeginPlay();
    for (int i = 0; i < 3; ++i) game.Tick(0.1f);
    CHECK(game.Warnings().size() == 1 && game.Warnings()[0].find("Nope.asequence") != std::string::npos); // once, not each frame
}

AETHER_TEST(Player_CooksSequencesAsAnAssetType) {
    const stdfs::path dir = TestDir("CookedSequence");
    ProjectPaths paths;
    std::string error;
    CHECK(CreateProject(dir, "Cinematic", &paths, &error));
    const EntityGuid actor{0xCAFE, 0xF00D};
    SequenceComponent c;
    c.sequence = "Sequences/Move.asequence";
    c.auto_play = true;
    const json scene = {{"$type", "Scene"},
                        {"$version", 1},
                        {"entities", json::array({{{"guid", ToString(actor)}, {"components", {{"Transform", Transform3(0, 0, 0)}, {"Tags", TagsJson("Actor")}}}},
                                                  {{"components", {{"SequenceComponent", reflect::ToJson(c)}}}}})}};
    stdfs::create_directories(paths.content / "Scenes");
    stdfs::create_directories(paths.content / "Sequences");
    std::ofstream(paths.content / "Scenes/main.ascene", std::ios::binary) << scene.dump(2);
    seq::LevelSequence s = ShowSequence(actor, "none");
    s.tracks.pop_back(); // just the move
    std::ofstream(paths.content / "Sequences/Move.asequence", std::ios::binary) << seq::SequenceToJson(s).dump(2);
    ProjectSettings settings;
    CHECK(LoadProject(paths.file, settings, &error));
    settings.startup_scene = "Scenes/main.ascene";
    settings.always_cook = {"Sequences/"}; // a scene names its sequences by path, which the cooker doesn't follow yet
    CHECK(SaveProject(paths.file, settings, &error));
    cook::CookOptions options;
    options.project_file = paths.file;
    options.output_dir = dir / "Build";
    options.configuration = cook::BuildConfiguration::Shipping;
    CHECK(cook::Cook(options).ok);

    GamePackage package;
    for (const std::string& pak : GamePackage::FindPaks(dir / "Build")) CHECK(package.Mount(pak));
    CHECK(package.LoadManifest(&error));
    const GameManifest::Asset* asset = package.FindAsset("Sequences/Move.asequence");
    CHECK(asset != nullptr && asset->importer == "Sequence");
    Game game(package);
    CHECK(game.LoadStartupScene(&error));
    game.BeginPlay();
    for (int i = 0; i < 10; ++i) game.Tick(0.1f);
    const Entity mover = FindTagged(game.GetWorld(), "Actor");
    CHECK(!mover.IsNull() && std::fabs(game.GetWorld().GetComponent<Transform>(mover)->position.x - 10.0f) < 0.01f);
    CHECK(game.Warnings().empty());
}

#if AETHER_TEST_PLAYER_PHYSICS
AETHER_TEST(Player_SimulatesPhysics) {
    if (!Game::HasPhysics()) return; // a build without the physics module
    const stdfs::path dir = TestDir("Physics");
    // A ball 10 m up and a floor under it.
    RigidBody dynamic;
    RigidBody fixed;
    fixed.motion = BodyMotion::Static;
    SphereCollider sphere;
    sphere.radius = 0.5f;
    BoxCollider box;
    box.half_extents = Vec3(20, 0.5f, 20);
    const json ball = {{"Transform", Transform3(0, 10, 0)},
                       {"Tags", TagsJson("Ball")},
                       {"RigidBody", reflect::ToJson(dynamic)},
                       {"SphereCollider", reflect::ToJson(sphere)}};
    const json floor = {{"Transform", Transform3(0, -0.5f, 0)},
                        {"RigidBody", reflect::ToJson(fixed)},
                        {"BoxCollider", reflect::ToJson(box)}};
    const json scene = {{"$type", "Scene"},
                        {"$version", 1},
                        {"entities", json::array({{{"components", ball}}, {{"components", floor}}})}};
    pak::PakWriter writer;
    writer.Add("Manifest.json", Manifest("Scenes/start.ascene", assets::ToString(assets::NewAssetGuid())).dump());
    writer.Add("Content/Scenes/start.ascene", scene.dump());
    CHECK(writer.Write((dir / "Game.apak").string()));
    GamePackage package;
    CHECK(package.Mount((dir / "Game.apak").string()));
    Game game(package);
    CHECK(game.LoadStartupScene());
    game.BeginPlay();
    CHECK(game.Stats().physics_bodies == 2);
    const Entity e = FindTagged(game.GetWorld(), "Ball");
    // The opt-in multiplayer client must not advance Jolt between snapshots.
    game.SetAuthoritativePhysicsClient(true);
    std::string reload_error;
    CHECK(!game.LoadStartupScene(&reload_error)); // external UDP World must not dangle
    CHECK(reload_error.find("authoritative multiplayer") != std::string::npos);
    for (int i = 0; i < 30; ++i) game.Tick(1.0f / 60.0f);
    CHECK(std::fabs(game.GetWorld().GetComponent<Transform>(e)->position.y - 10.0f) < 0.001f);
    // Default single-player physics still steps when opt-in is disabled.
    game.SetAuthoritativePhysicsClient(false);
    for (int i = 0; i < 30; ++i) game.Tick(1.0f / 60.0f);
    const f32 half_second = game.GetWorld().GetComponent<Transform>(e)->position.y;
    CHECK(half_second < 9.0f && half_second > 8.0f); // ~10 - g t^2 / 2 = 8.77
    for (int i = 0; i < 240; ++i) game.Tick(1.0f / 60.0f);
    const f32 rest = game.GetWorld().GetComponent<Transform>(e)->position.y;
    CHECK(rest > 0.3f && rest < 0.7f); // on the floor
}

AETHER_TEST(Player_RealUdpSnapshotsShareGameWorld) {
    if (!Game::HasPhysics()) return;
    // Exercise the shipped host's real UDP path against a cooked Game World
    // and its live PhysicsScene, not the headless client's private World.
    const stdfs::path dir = TestDir("UdpGameWorld");
    GamePackage package;
    CHECK(package.Mount(MakePak(dir, "Game.apak").string()));
    Game game(package);
    CHECK(game.LoadStartupScene());
    PhysicsScene* physics = game.PhysicsQueries();
    CHECK(physics != nullptr);
    if (!physics) return;
    game.BeginPlay();

    server::RegisterServerComponents();
    World server_world;
    net::NetPhysicsState motion;
    motion.linear_velocity = Vec3(1, 2, 3);
    motion.angular_velocity = Vec3(0, 1, 0);
    const Entity source = server_world.CreateEntity(
        Transform{Vec3(10, 4, 0), Quaternion::Identity()}, motion);
    server::ServerConfig server_config;
    server_config.port = 0;
    server_config.lan_advertise = false;
    server_config.build_id = 123;
    server::DedicatedServer server;
    std::string error;
    if (!server.Start(server_config, server_world, &error)) {
        std::printf("    (no UDP here: %s)\n", error.c_str());
        game.EndPlay();
        return;
    }

    // A real UDP relay allows deterministic downstream datagram loss without
    // adding fault-injection hooks to the shipped Net/Physics modules.
    net::UdpSocket relay;
    CHECK(relay.Open(0, &error));
    if (!relay.IsOpen()) {
        game.EndPlay();
        server.Stop();
        return;
    }
    const net::Address server_endpoint = net::Address::Loopback(server.Port());
    net::Address client_endpoint;
    bool drop_downstream = false;
    u32 dropped_downstream = 0;
    const auto pump_udp = [&] {
        net::Address from;
        std::vector<u8> packet;
        while (relay.Receive(from, packet)) {
            if (from == server_endpoint) {
                if (drop_downstream) {
                    ++dropped_downstream;
                } else if (client_endpoint.port != 0) {
                    CHECK(relay.Send(client_endpoint, std::span<const u8>(packet.data(), packet.size())));
                }
            } else {
                client_endpoint = from;
                CHECK(relay.Send(server_endpoint, std::span<const u8>(packet.data(), packet.size())));
            }
        }
    };

    using Clock = std::chrono::steady_clock;
    const auto start = Clock::now();
    const auto now = [&] { return std::chrono::duration<f64>(Clock::now() - start).count(); };
    server::NetClient client;
    CHECK(client.UseWorld(game.GetWorld()));
    CHECK(&client.GetWorld() == &game.GetWorld());
    u32 accepted = 0, jolt_verified = 0;
    bool saw_initial = false, saw_updated = false;
    const Quaternion updated_rotation = Quaternion::FromAxisAngle(Vec3(0, 1, 0), Radians(90.0f));
    client.OnSnapshotApplied([&](World& world) {
        CHECK(&world == &game.GetWorld());
        // Exactly the bridge used by aether_player after an accepted snapshot.
        ApplyAuthoritativePhysicsSnapshot<net::NetPhysicsState>(world, *physics);
        ++accepted;
        // Real UDP snapshots must reconcile the shipped Game's live Jolt body,
        // including its tracked pose and activation, before render sampling.
        world.ForEach<Transform, RigidBody, net::NetPhysicsState>(
            [&](Transform& transform, RigidBody& body, net::NetPhysicsState& motion_state) {
                CHECK(!body.body_id.IsInvalid());
                if (body.body_id.IsInvalid()) return;
                const Vec3 position = physics->Physics().GetPosition(body.body_id);
                const Quaternion rotation = physics->Physics().GetRotation(body.body_id);
                const f32 dot = rotation.x * transform.rotation.x + rotation.y * transform.rotation.y +
                                rotation.z * transform.rotation.z + rotation.w * transform.rotation.w;
                CHECK((position - transform.position).Length() < 0.011f);
                CHECK(std::fabs(dot) > 0.999f);
                CHECK(physics->Physics().BodyInterface().IsActive(body.body_id) != motion_state.sleeping);
                ++jolt_verified;
            });
        CHECK(ReconcileAuthoritativePhysicsPose<net::NetPhysicsState>(world, *physics) == 0);
        world.ForEach<Transform, net::NetPhysicsState>(
            [&](Transform& transform, net::NetPhysicsState& state) {
                if (std::fabs(transform.position.x - 10.0f) < 0.01f &&
                    std::fabs(state.linear_velocity.x - 1.0f) < 0.01f)
                    saw_initial = true;
                if (std::fabs(transform.position.x - 25.0f) < 0.01f &&
                    std::fabs(transform.rotation.y - updated_rotation.y) < 0.01f &&
                    std::fabs(transform.rotation.w - updated_rotation.w) < 0.01f &&
                    std::fabs(state.linear_velocity.x - 7.0f) < 0.01f &&
                    std::fabs(state.angular_velocity.z - 3.0f) < 0.01f && state.sleeping)
                    saw_updated = true;
            });
    });
    server::NetClientConfig client_config;
    client_config.server = net::Address::Loopback(relay.LocalAddress().port);
    client_config.build_id = 123;
    client_config.send_rpc = false; // shipped player does not send the smoke-test Ping
    client_config.duration = 1.5;
    client_config.join_timeout = 3.0;
    CHECK(client.Start(client_config, now(), &error));
    CHECK(client.Replication() != nullptr);
    // The shipped host uses a cooked World/PhysicsScene; network spawns get
    // their local collider here, while the server owns all replicated motion.
    client.Replication()->OnSpawn(server::kSceneArchetype, [](World& world, Entity entity, const net::NetIdentity&) {
        RigidBody body;
        body.motion = BodyMotion::Dynamic;
        world.AddComponent(entity, body);
        SphereCollider sphere;
        sphere.radius = 0.5f;
        world.AddComponent(entity, sphere);
    });
    net::SnapshotInterpolation visual(game.GetWorld(), *client.Replication());
    visual.SetRenderOnly(true);
    CHECK(!client.UseWorld(game.GetWorld())); // World cannot change after Start
    game.SetAuthoritativePhysicsClient(true);
    bool moved = false;
    bool sampled_visual = false;
    bool gap_started = false, gap_finished = false, resumed_after_gap = false;
    bool loss_started = false, loss_finished = false, resumed_after_loss = false;
    f64 gap_until = 0.0, loss_until = 0.0;
    u32 accepted_at_gap_end = 0, accepted_at_loss_end = 0;
    u32 poll_iteration = 0;
    while (!client.Done() && now() < 6.0) {
        const f64 tick_now = now();
        // A deterministic 180 ms snapshot-transmission gap exercises recovery
        // from missing updates without pretending to inject UDP packet loss.
        if (saw_updated && !gap_started) {
            gap_started = true;
            gap_until = tick_now + 0.18;
        }
        if (resumed_after_gap && !loss_started) {
            loss_started = true;
            loss_until = tick_now + 0.18;
        }
        drop_downstream = loss_started && tick_now < loss_until;
        if (loss_started && !loss_finished && !drop_downstream) {
            loss_finished = true;
            accepted_at_loss_end = accepted;
        }
        // Forward actual OS UDP datagrams through the relay; during the loss
        // window only server->client datagrams are discarded. The server
        // continues ticking and transmitting, unlike the snapshot-gap phase.
        pump_udp();
        if (!gap_started || tick_now >= gap_until) {
            if (gap_started && !gap_finished) {
                gap_finished = true;
                accepted_at_gap_end = accepted;
            }
            server.Tick(tick_now);
        }
        pump_udp();
        client.Tick(tick_now);
        pump_udp();
        if (gap_finished && accepted > accepted_at_gap_end) resumed_after_gap = true;
        if (loss_finished && accepted > accepted_at_loss_end) resumed_after_loss = true;
        if (saw_initial && !moved) {
            server_world.GetComponent<Transform>(source)->position = Vec3(25, 4, 0);
            server_world.GetComponent<Transform>(source)->rotation = updated_rotation;
            auto* state = server_world.GetComponent<net::NetPhysicsState>(source);
            state->linear_velocity = Vec3(7, 0, 0);
            state->angular_velocity = Vec3(0, 0, 3);
            state->sleeping = true;
            moved = true;
        }
        game.Tick(1.0f / 60.0f); // authoritative mode: never step local Jolt
        visual.Update(now()); // shipped player's render-only interpolation path
        game.GetWorld().ForEach<net::NetIdentity, net::NetPhysicsState>(
            [&](const net::NetIdentity& identity, const net::NetPhysicsState& motion_state) {
                Transform sampled;
                if (!visual.SampleRenderTransform(identity.net_id, sampled)) return;
                sampled_visual = true;
                Transform authoritative;
                if (!client.Replication()->ReadReplicated(identity.net_id, GetComponentId<Transform>(), &authoritative))
                    return;
                const Entity entity = client.Replication()->FindEntity(identity.net_id);
                const Transform* current = game.GetWorld().IsAlive(entity)
                                               ? game.GetWorld().GetComponent<Transform>(entity) : nullptr;
                CHECK(current != nullptr);
                if (current) {
                    // Render sampling must not rewind any accepted authoritative ECS pose.
                    CHECK(std::fabs(current->position.x - authoritative.position.x) < 0.01f);
                    CHECK(std::fabs(current->rotation.y - authoritative.rotation.y) < 0.01f);
                    CHECK(std::fabs(current->rotation.w - authoritative.rotation.w) < 0.01f);
                }
                net::NetPhysicsState authoritative_motion;
                if (client.Replication()->ReadReplicated(identity.net_id, GetComponentId<net::NetPhysicsState>(),
                                                         &authoritative_motion)) {
                    CHECK(std::fabs(motion_state.linear_velocity.x - authoritative_motion.linear_velocity.x) < 0.01f);
                    CHECK(std::fabs(motion_state.angular_velocity.z - authoritative_motion.angular_velocity.z) < 0.01f);
                    CHECK(motion_state.sleeping == authoritative_motion.sleeping);
                }
            });
        // Deliberately vary UDP polling cadence to exercise bursty snapshot arrival.
        // This is jitter coverage, not a packet-loss simulation.
        std::this_thread::sleep_for(std::chrono::milliseconds((++poll_iteration % 11 == 0) ? 18 : 2));
    }
    CHECK(client.Done() && client.Result().joined);
    CHECK(client.Result().snapshots > 0 && accepted > 0);
    CHECK(saw_initial && saw_updated);
    CHECK(jolt_verified > 0);
    CHECK(gap_started && gap_finished && resumed_after_gap);
    CHECK(loss_started && loss_finished && dropped_downstream > 0 && resumed_after_loss);
    CHECK(sampled_visual);
    CHECK(client.Result().rpcs_sent == 0);
    game.SetAuthoritativePhysicsClient(false);
    game.EndPlay();
    server.Stop();
}

#endif

// The order the player runs its systems in, as a snapshot: the kit migration (Phase 37 step 4) must not change it.
// A failure prints the new order; update the golden text only when a change of order is intended.
namespace {
std::string DescribeStages(Game& game) {
    std::string out, error;
    game.Systems().Build(&error);
    if (!error.empty()) return "build failed: " + error;
    for (const SystemPhase phase : {SystemPhase::PreUpdate, SystemPhase::FixedUpdate, SystemPhase::Update, SystemPhase::LateUpdate, SystemPhase::PreRender}) {
        out += std::string(SystemPhaseName(phase)) + ":";
        for (const std::string& name : game.Systems().Order(phase)) out += " " + name;
        out += "\n";
        for (const std::vector<std::string>& level : game.Systems().Levels(phase)) {
            out += "  |";
            for (const std::string& name : level) out += " " + name;
            out += "\n";
        }
    }
    return out;
}
} // namespace


AETHER_TEST(Player_RenderPoseHierarchyIsVisualOnly) {
    World world;
    GuidIndex guids;
    const EntityGuid root_guid{1, 2};
    const Entity root = world.CreateEntity(
        IdComponent{root_guid},
        Transform{Vec3(10, 0, 0), Quaternion::Identity(), Vec3(2, 2, 2)});
    const Entity child = world.CreateEntity(
        Parent{root_guid},
        Transform{Vec3(1, 0, 0), Quaternion::Identity(), Vec3(3, 3, 3)});
    guids.Rebuild(world);

    const Mat4 original = ComputeWorldTransform(world, guids, child);
    CHECK(std::fabs(original.cols[3].x - 12.0f) < 1e-4f);
    const Mat4 sampled = ComputeRenderWorldTransform(
        world, guids, child, [&](Entity entity, Transform& pose) {
            if (entity == root) pose.position = Vec3(20, 0, 0);
            else if (entity == child) pose.position = Vec3(2, 0, 0);
            else return false;
            pose.scale = Vec3(99, 99, 99); // a sampler cannot replace authored scale
            return true;
        });
    CHECK(std::fabs(sampled.cols[3].x - 24.0f) < 1e-4f);
    CHECK(std::fabs(sampled.cols[0].x - 6.0f) < 1e-4f);
    CHECK(std::fabs(world.GetComponent<Transform>(root)->position.x - 10.0f) < 1e-4f);
    CHECK(std::fabs(world.GetComponent<Transform>(child)->position.x - 1.0f) < 1e-4f);
    const Mat4 offline = ComputeRenderWorldTransform(world, guids, child, {});
    CHECK(std::fabs(offline.cols[3].x - original.cols[3].x) < 1e-4f);
}

AETHER_TEST(Player_StageOrderSnapshot) {
    const stdfs::path dir = TestDir("StageOrder");
    const stdfs::path pak = MakePak(dir, "Game.apak");
    GamePackage package;
    std::string error;
    CHECK(package.Mount(pak.string(), 0, &error));
    CHECK(package.LoadManifest(&error));
    Game game(package);
    CHECK(game.LoadStartupScene(&error));
    game.Tick(1.0f / 60.0f); // builds the frame's systems
    const std::string order = DescribeStages(game);
#if defined(AETHER_KIT_INVENTORY) && defined(AETHER_KIT_INTERACTION) && defined(AETHER_KIT_QUESTS) && AETHER_KIT_INVENTORY && AETHER_KIT_INTERACTION && AETHER_KIT_QUESTS
    const std::string golden =
        "PreUpdate:\n"
        "FixedUpdate: Player.Physics Player.Physics2D Player.FixedUpdate\n"
        "  | Player.Physics\n"
        "  | Player.Physics2D\n"
        "  | Player.FixedUpdate\n"
        "Update: Player.Update Player.Sequencer Player.Animation Player.Audio Player.Save Player.Effects Player.Particles Player.Abilities Player.Inventory Player.Interaction Player.Attributes Player.Quests Player.Scripting\n"
        "  | Player.Update\n"
        "  | Player.Sequencer\n"
        "  | Player.Animation Player.Audio Player.Save Player.Effects\n"
        "  | Player.Particles Player.Abilities Player.Inventory Player.Interaction Player.Attributes\n"
        "  | Player.Quests\n"
        "  | Player.Scripting\n"
        "LateUpdate: Player.LateUpdate\n"
        "  | Player.LateUpdate\n"
        "PreRender:\n";
    if (order != golden) std::printf("  stage order is now:\n%s", order.c_str());
    CHECK(order == golden);
#else
    CHECK(order.find("Player.Scripting") != std::string::npos);
#endif
}
