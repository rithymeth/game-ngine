#include "aether/assets/asset_guid.h"
#include "aether/audio/sound.h"
#include "aether/audio/audio_system.h"
#include "aether/cook/cooker.h"
#include "aether/pak/pak.h"
#include "aether/player/game.h"
#include "aether/project/project.h"
#include "aether/scene/components.h"
#include "aether/scene/gameplay.h"
#include "aether/sequencer/sequence.h"
#include "aether/sequencer/sequence_system.h"
#include "aether/reflection/serialize.h"
#include "test_framework.h"

#if AETHER_TEST_PLAYER_PHYSICS
#include "aether/physics/components.h"
#endif

#include <nlohmann/json.hpp>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>

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
    for (int i = 0; i < 30; ++i) game.Tick(1.0f / 60.0f);
    const f32 half_second = game.GetWorld().GetComponent<Transform>(e)->position.y;
    CHECK(half_second < 9.0f && half_second > 8.0f); // ~10 - g t^2 / 2 = 8.77
    for (int i = 0; i < 240; ++i) game.Tick(1.0f / 60.0f);
    const f32 rest = game.GetWorld().GetComponent<Transform>(e)->position.y;
    CHECK(rest > 0.3f && rest < 0.7f); // on the floor
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
        "Update: Player.Update Player.Sequencer Player.Audio Player.Save Player.Effects Player.Abilities Player.Inventory Player.Interaction Player.Attributes Player.Quests Player.Scripting\n"
        "  | Player.Update\n"
        "  | Player.Sequencer\n"
        "  | Player.Audio Player.Save Player.Effects\n"
        "  | Player.Abilities Player.Inventory Player.Interaction Player.Attributes\n"
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
