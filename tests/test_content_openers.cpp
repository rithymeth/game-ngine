#include "test_framework.h"

#include "aether/ai/behavior_tree.h"
#include "aether/animation/anim_graph.h"
#include "aether/animation/blend_space.h"
#include "aether/assets/asset_database.h"
#include "aether/assets/asset_ref.h"
#include "aether/audio/cue.h"
#include "aether/blueprint/graph.h"
#include "aether/ecs/world.h"
#include "aether/project/project.h"
#include "aether/reflection/serialize.h"
#include "aether/renderer/material.h"
#include "aether/save/save_system.h"
#include "aether/scene/components.h"
#include "aether/scene/model_assets.h"
#include "aether/scene/serialization.h"
#include "aether/sequencer/sequence.h"
#include "aether/sprite2d/tilemap.h"
#include "aether/vfx/emitter.h"
#include "content/content_browser_panel.h"
#include "core/notifications.h"
#include "sprite2d/tilemap_document.h"
#include "ui/extensions.h"
#include "workspace/editor_workspace.h"
#include "world/scene_panel.h"

#include <imgui.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <string_view>

// Stage 3's openers: a double-click in the Content Browser opens every asset kind in the editor that edits it, and
// says why when nothing does. Also the context menu's Copy Path and Reveal in Explorer, and the reference viewer's
// two directions.

using namespace aether;
using namespace aether::editor;
namespace stdfs = std::filesystem;

#define CHECK(...) AETHER_CHECK((__VA_ARGS__))

namespace opener_test {
struct OpenerDecal {
    assets::AssetRef<assets::TextureAsset> texture;
    f32 size = 1.0f;
};
struct OpenerSave {
    i32 coins = 7;
};
} // namespace opener_test

AETHER_REFLECT(opener_test::OpenerDecal, 1, AETHER_FIELD(texture, Field_EditAnywhere), AETHER_FIELD(size, Field_EditAnywhere))
AETHER_REFLECT(opener_test::OpenerSave, 1, AETHER_FIELD(coins, Field_EditAnywhere))

namespace {

class HeadlessImGui {
public:
    HeadlessImGui() {
        context_ = ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigMacOSXBehaviors = false;
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(1280, 800);
        io.DeltaTime = 1.0f / 60.0f;
        unsigned char* pixels = nullptr;
        int w = 0, h = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
    }
    ~HeadlessImGui() { ImGui::DestroyContext(context_); }
    void Frame(const std::function<void()>& body) {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(1100, 700));
        ImGui::Begin("Test");
        body();
        ImGui::End();
        ImGui::Render();
    }

private:
    ImGuiContext* context_ = nullptr;
};

stdfs::path FreshProject(const std::string& name, ProjectPaths& paths) {
    const stdfs::path dir = stdfs::temp_directory_path() / "aether_content_opener_tests" / name;
    std::error_code ec;
    stdfs::remove_all(dir, ec);
    stdfs::create_directories(dir, ec);
    std::string error;
    CHECK(CreateProject(dir, "Game", &paths, &error));
    return dir;
}

void Write(const stdfs::path& file, const std::string& text) {
    stdfs::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary | std::ios::trunc) << text;
}

// A scene with one entity that `SceneDocument::Load` accepts.
void WriteScene(const stdfs::path& file) {
    stdfs::create_directories(file.parent_path());
    World world;
    world.CreateEntity(Transform{Vec3(0, 0, 0), Quaternion::Identity()});
    CHECK(SaveSceneJson(world, file.string()));
}

// One valid file of every kind the workspace opens, written with the serializer that kind's editor saves with.
void WriteEveryKind(const ProjectPaths& paths) {
    std::string error;
    stdfs::create_directories(paths.content / "Graphs");
    bp::Blueprint blueprint;
    CHECK(bp::SaveBlueprint(blueprint, paths.content / "Graphs/BP_Door.abp", &error));
    mat::Material material;
    material.description = "A lit test material";
    CHECK(mat::SaveMaterial(material, paths.content / "Graphs/M_Lit.amat", &error));
    Write(paths.content / "Animation/ABP.aanim", anim::AnimGraphToJson(anim::AnimGraph{}).dump(2));
    Write(paths.content / "Animation/BS_Locomotion.ablend", anim::BlendSpaceToJson(anim::BlendSpace{}).dump(2));
    audio::SoundCue cue;
    cue.name = "SC_Blip";
    Write(paths.content / "Audio/SC_Blip.acue", audio::SaveCue(cue));
    Write(paths.content / "UI/WBP_MainMenu.aui", R"({"root":{"type":"Canvas","name":"Root"}})" "\n");
    Write(paths.content / "AI/BT_Guard.abt", ai::SaveBehaviorTree(ai::BehaviorTreeAsset{}).dump(2));
    vfx::ParticleSystemAsset particles;
    particles.name = "PS_Sparks";
    Write(paths.content / "VFX/PS_Sparks.avfx", vfx::SaveParticleSystem(particles));
    seq::LevelSequence sequence;
    sequence.name = "Intro";
    CHECK(seq::SaveSequence((paths.content / "Sequences/Intro.asequence").string(), sequence, &error));
    World world;
    world.CreateEntity(Transform{Vec3(0, 1, 0), Quaternion::Identity()});
    CHECK(SaveSceneJson(world, (paths.content / "Maps/Empty.ascene").string()));
    Write(paths.content / "Scripts/door.luau", "print(\"door\")\n");
}

// The tool the workspace shows after an open, and that it popped it out.
bool ShowingTool(EditorWorkspace& ws, std::string_view prefix) {
    const i64 tool = ws.FindTool(prefix);
    return tool >= 0 && ws.Selected() == static_cast<usize>(tool) && ws.IsWindowOpen(static_cast<usize>(tool));
}

bool PostedNoEditor(const EditorWorkspace& ws) {
    for (const Notification& n : ws.Notifications().All()) {
        if (n.title == "No editor") return true;
    }
    return false;
}

} // namespace

AETHER_TEST(Openers_EveryOpenableKindLoadsItsFileAndShowsItsTool) {
    ProjectPaths paths;
    const stdfs::path project = FreshProject("kinds", paths);
    WriteEveryKind(paths);
    EditorWorkspace ws(paths.file);

    struct Case {
        const char* path;
        const char* tool;
    };
    const Case cases[] = {
        {"Graphs/BP_Door.abp", "Blueprint"},
        {"Graphs/M_Lit.amat", "Material"},
        {"Animation/ABP.aanim", "Animation"},
        {"Animation/BS_Locomotion.ablend", "Animation"},
        {"Audio/SC_Blip.acue", "Sound Cue"},
        {"UI/WBP_MainMenu.aui", "UI Designer"},
        {"AI/BT_Guard.abt", "Behavior Tree"},
        {"VFX/PS_Sparks.avfx", "Particle System"},
        {"Sequences/Intro.asequence", "Sequencer"},
        {"Maps/Empty.ascene", "Scene"},
        {"Scripts/door.luau", "Luau Script"},
    };
    for (const Case& c : cases) {
        std::string error;
        const bool opened = ws.OpenAsset(c.path, &error);
        CHECK(opened);
        CHECK(error.empty());
        CHECK(ShowingTool(ws, c.tool));
        CHECK(!PostedNoEditor(ws)); // a refusal is what would be posted
    }
    std::error_code ec;
    stdfs::remove_all(project, ec);
}

AETHER_TEST(Openers_SayWhyAnAssetCannotOpen) {
    ProjectPaths paths;
    const stdfs::path project = FreshProject("refusals", paths);
    Write(paths.content / "notes.txt", "not an asset\n");
    Write(paths.content / "Graphs/broken.amat", "{ not json");
    WriteScene(paths.content / "Maps/level.ascene");
    Write(paths.content / "Plot.dialogue", "{\"lines\": []}\n");
    EditorWorkspace ws(paths.file);

    // Nothing claims the extension.
    std::string error = "untouched";
    CHECK(!ws.OpenAsset("notes.txt", &error));
    CHECK(error == "No editor opens notes.txt.");
    CHECK(ws.Notifications().All().size() == 1 && ws.Notifications().All().back().title == "No editor");

    // The file is not there at all.
    CHECK(!ws.OpenAsset("Graphs/missing.amat", &error));
    CHECK(error.find("doesn't exist") != std::string::npos);

    // A file of a kind that has an editor, but that doesn't load: the loader's reason comes back.
    CHECK(!ws.OpenAsset("Graphs/broken.amat", &error));
    CHECK(error.find("JSON") != std::string::npos);

    // A registered asset type with no opener says so, rather than pretending nothing knows the extension.
    ws.Extensions().AddAssetType({"Dialogue", ".dialogue", "{\"lines\": []}\n", nullptr, "test"});
    CHECK(!ws.OpenAsset("Plot.dialogue", &error));
    CHECK(error == "Dialogue assets have no editor (Plot.dialogue).");

    // A scene with unsaved changes is not replaced silently.
    CHECK(ws.OpenAsset("Maps/level.ascene", &error));
    ws.Scene().Document().CreateEntity("unsaved");
    CHECK(ws.Scene().Document().IsDirty());
    WriteScene(paths.content / "Maps/other.ascene");
    CHECK(!ws.OpenAsset("Maps/other.ascene", &error));
    CHECK(error.find("unsaved changes") != std::string::npos);

    std::error_code ec;
    stdfs::remove_all(project, ec);
}

AETHER_TEST(Openers_ThePanelUsesTheHostOpenerAndFallsBackToTheRegistry) {
    ProjectPaths paths;
    const stdfs::path project = FreshProject("panel", paths);
    Write(paths.content / "Plot.dialogue", "{\"lines\": []}\n");
    Write(paths.content / "notes.txt", "not an asset\n");
    ExtensionRegistry registry;
    registry.MakeActive();
    registry.AddAssetType({"Dialogue", ".dialogue", "{\"lines\": []}\n", nullptr, "test"});
    ContentBrowserPanel panel(paths.file);

    std::string error;
    CHECK(!panel.OpenEntry("Plot.dialogue", &error));
    CHECK(error == "Dialogue assets have no editor (Plot.dialogue).");
    CHECK(panel.LastOpenError() == error);
    // The double-click happened: opening nothing leaves the reason the last one left.
    CHECK(!panel.OpenEntry("notes.txt", &error));
    CHECK(error == "No editor opens notes.txt.");
    CHECK(!panel.OpenEntry("", &error));

    // The host's opener wins when there is one, and its own reason is what the panel keeps.
    std::string asked;
    panel.SetAssetOpener([&](const std::string& path, std::string* why) {
        asked = path;
        if (path == "Plot.dialogue") return true;
        if (why != nullptr) *why = "the host refused this one";
        return false;
    });
    CHECK(panel.OpenEntry("Plot.dialogue", &error) && error.empty() && asked == "Plot.dialogue");
    CHECK(!panel.OpenEntry("notes.txt", &error) && error == "the host refused this one");

    registry.Clear();
    std::error_code ec;
    stdfs::remove_all(project, ec);
}

AETHER_TEST(Openers_CopyPathRevealAndTheReferenceViewer) {
    ProjectPaths paths;
    const stdfs::path project = FreshProject("actions", paths);
    HeadlessImGui imgui;
    Write(paths.content / "Textures/splat.png", "pixels");
    Write(paths.content / "Textures/ghost.png", "goes missing");
    WriteEveryKind(paths);

    // A scene that uses the texture, and one that uses an asset that is not there.
    assets::AssetGuid splat;
    {
        assets::AssetDatabase scan(paths.content);
        scan.Scan();
        const assets::AssetRecord* record = scan.FindByPath("Textures/splat.png");
        CHECK(record != nullptr);
        splat = record->guid;
        World world;
        world.CreateEntity(opener_test::OpenerDecal{{splat}, 2.0f});
        CHECK(SaveSceneJson(world, (paths.content / "Maps/level.ascene").string()));
        World broken;
        broken.CreateEntity(opener_test::OpenerDecal{{assets::NewAssetGuid()}, 1.0f});
        CHECK(SaveSceneJson(broken, (paths.content / "Maps/broken.ascene").string()));
    }
    stdfs::remove(paths.content / "Textures/ghost.png"); // its .ameta remains: a missing source
    ContentBrowserPanel panel(paths.file);
    const assets::AssetRecord* level = panel.Database()->FindByPath("Maps/level.ascene");
    CHECK(level != nullptr);

    // The graph kinds the editor writes are assets now, so the listing can show them.
    const assets::AssetRecord* material = panel.Database()->FindByPath("Graphs/M_Lit.amat");
    CHECK(material != nullptr && material->importer == "Material");
    const assets::AssetRecord* tree = panel.Database()->FindByPath("AI/BT_Guard.abt");
    CHECK(tree != nullptr && tree->importer == "BehaviorTree");

    // The reference viewer's two directions.
    CHECK(panel.ShowReferences("Maps/level.ascene"));
    CHECK(panel.ReferencesOpen() && panel.ReferencesTarget() == level->guid);
    CHECK(panel.ReferencesDirection() == assets::ReferenceDirection::Dependencies);
    const std::vector<assets::ReferenceNode> uses = assets::CollectReferences(*panel.Database(), level->guid, assets::ReferenceDirection::Dependencies);
    CHECK(std::any_of(uses.begin(), uses.end(), [](const assets::ReferenceNode& n) { return n.path == "Textures/splat.png"; }));
    CHECK(panel.ShowReferences("Textures/splat.png", assets::ReferenceDirection::Referencers));
    CHECK(panel.ReferencesTarget() == splat && panel.ReferencesDirection() == assets::ReferenceDirection::Referencers);
    const std::vector<assets::ReferenceNode> users = assets::CollectReferences(*panel.Database(), splat, assets::ReferenceDirection::Referencers);
    CHECK(std::any_of(users.begin(), users.end(), [](const assets::ReferenceNode& n) { return n.path == "Maps/level.ascene"; }));
    CHECK(!panel.ShowReferences("Textures/nope.png")); // not an asset: nothing to show

    // The validation list holds the broken reference, and both windows draw.
    CHECK(!panel.Issues().empty());
    panel.validation_open = true;
    for (int frame = 0; frame < 2; ++frame) imgui.Frame([&] { panel.Draw(); });
    CHECK(ImGui::GetDrawData()->TotalVtxCount > 0);

    // Copy Path is the absolute path, on the clipboard too.
    const std::string copied = panel.CopyPath("Textures/splat.png");
    CHECK(copied == (paths.content / "Textures/splat.png").lexically_normal().string());
    CHECK(panel.LastCopiedPath() == copied);
    CHECK(panel.CopyPath("") .empty());

    // Reveal hands the absolute path to the host, and without a host there is nothing to call.
    std::string revealed;
    panel.SetRevealer([&](const std::string& absolute) {
        revealed = absolute;
        return true;
    });
    CHECK(panel.RevealInFileManager("Textures/splat.png"));
    CHECK(revealed == copied);
    CHECK(!panel.RevealInFileManager(""));
    panel.SetRevealer(std::function<bool(const std::string&)>{});
    CHECK(!panel.RevealInFileManager("Textures/splat.png"));

    std::error_code ec;
    stdfs::remove_all(project, ec);
}

AETHER_TEST(Openers_TilemapsAndTilesetsOpenTheirEditor) {
    ProjectPaths paths;
    const stdfs::path project = FreshProject("tilemaps", paths);
    std::string error;
    // A tileset the scan knows, and maps that name it.
    sprite2d::Tileset tileset;
    tileset.texture_width = 128;
    tileset.texture_height = 64;
    tileset.tile_width = tileset.tile_height = 16;
    CHECK(sprite2d::SaveTileset(paths.content / "Tilesets/Ground.atileset", tileset, &error));
    assets::AssetGuid ground;
    {
        assets::AssetDatabase scan(paths.content);
        scan.Scan();
        const assets::AssetRecord* record = scan.FindByPath("Tilesets/Ground.atileset");
        CHECK(record != nullptr);
        if (record != nullptr) ground = record->guid;
    }
    sprite2d::TilemapData map;
    map.tileset = ground;
    map.Resize(6, 4);
    map.AddLayer("Ground");
    map.Set(0, 1, 2, 7);
    CHECK(sprite2d::SaveTilemap(paths.content / "Maps/Level_02.atilemap", map, &error));
    CHECK(sprite2d::SaveTilemap(paths.content / "Maps/Third.atilemap", map, &error));
    CHECK(sprite2d::SaveTileset(paths.content / "Tilesets/Unused.atileset", tileset, &error));
    sprite2d::TilemapData broken;
    broken.tileset = assets::NewAssetGuid();
    broken.Resize(2, 2);
    broken.AddLayer("L");
    CHECK(sprite2d::SaveTilemap(paths.content / "Maps/Broken.atilemap", broken, &error));
    EditorWorkspace ws(paths.file);

    // The map loads with its tileset, and the tool shows it.
    CHECK(ws.OpenAsset("Maps/Level_02.atilemap", &error));
    CHECK(error.empty());
    CHECK(ShowingTool(ws, "Tilemap"));
    CHECK(ws.Tilemap().Map().width == 6 && ws.Tilemap().Map().height == 4);
    CHECK(ws.Tilemap().Map().layers.size() == 1 && ws.Tilemap().Map().layers[0].name == "Ground");
    CHECK(ws.Tilemap().Map().Get(0, 1, 2) == 7);
    CHECK(ws.Tilemap().GetTileset().tile_width == 16);
    CHECK(!ws.Tilemap().dirty());
    CHECK(ws.Tilemap().TilemapFile().filename() == "Level_02.atilemap");

    // A tileset opens the map that uses it; one no map uses says so.
    CHECK(ws.OpenAsset("Tilesets/Ground.atileset", &error));
    CHECK(error.empty() && ShowingTool(ws, "Tilemap") && ws.Tilemap().Map().width == 6);
    CHECK(!ws.OpenAsset("Tilesets/Unused.atileset", &error));
    CHECK(error.find("No tilemap uses") != std::string::npos);
    CHECK(!ws.OpenAsset("Maps/Broken.atilemap", &error));
    CHECK(error.find("tileset that isn't in the project") != std::string::npos);

    // An edit is not lost silently, and Save writes it back where it came from.
    ws.Tilemap().brush = 3;
    ws.Tilemap().Apply(0, 0);
    CHECK(ws.Tilemap().dirty());
    CHECK(!ws.OpenAsset("Maps/Third.atilemap", &error));
    CHECK(error.find("unsaved changes") != std::string::npos);
    CHECK(ws.Tilemap().Save(&error));
    CHECK(!ws.Tilemap().dirty());
    sprite2d::TilemapData reread;
    CHECK(sprite2d::LoadTilemap(paths.content / "Maps/Level_02.atilemap", reread, &error));
    CHECK(reread.Get(0, 0, 0) == 3);
    CHECK(ws.OpenAsset("Maps/Third.atilemap", &error));

    std::error_code ec;
    stdfs::remove_all(project, ec);
}

AETHER_TEST(Openers_EveryRegisteredTypeWithAnOpenerOpensHeadless) {
    ProjectPaths paths;
    const stdfs::path project = FreshProject("registered", paths);
    // A string table, and a save the workspace's own registrations claim.
    Write(paths.content / "Localization/strings.astrings", "key,en\r\nmenu.play,Play\r\n");
    {
        save::SaveSystem saves(paths.content / "Saves");
        const save::SaveResult saved = saves.Save("slot1", opener_test::OpenerSave{});
        CHECK(saved.ok);
    }
    // The settings type opens the same envelope, so the save does for both.
    stdfs::copy_file(paths.content / "Saves/slot1.asav", paths.content / "Saves/slot1.asettings");
    EditorWorkspace ws(paths.file);

    for (const ExtensionAssetType& type : ws.Extensions().AssetTypes()) {
        if (!type.open) continue; // the ones without an editor are tested above
        const std::string path = type.extension == ".astrings" ? "Localization/strings.astrings" : "Saves/slot1" + type.extension;
        std::string error;
        CHECK(ws.OpenAsset(path, &error));
        CHECK(error.empty());
        CHECK(!PostedNoEditor(ws));
    }
    CHECK(!ws.Extensions().AssetTypes().empty());

    std::error_code ec;
    stdfs::remove_all(project, ec);
}
