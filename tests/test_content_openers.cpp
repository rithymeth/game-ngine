#include "test_framework.h"

#include "aether/ai/behavior_tree.h"
#include "aether/animation/anim_graph.h"
#include "aether/animation/blend_space.h"
#include "aether/assets/asset_database.h"
#include "aether/assets/asset_ref.h"
#include "aether/assets/png_writer.h"
#include "aether/audio/cue.h"
#include "aether/blueprint/graph.h"
#include "aether/ecs/world.h"
#include "aether/cook/package.h"
#include "aether/platform/process.h"
#include "aether/input/bindings.h"
#include "aether/scene/gameplay.h"
#include "aether/sprite2d/platformer.h"
#include "aether/player/game.h"
#include "aether/project/project.h"
#include "aether/reflection/serialize.h"
#include "aether/renderer/material.h"
#include "aether/save/save_system.h"
#include "aether/scene/components.h"
#include "aether/scene/model_assets.h"
#include "aether/scene/serialization.h"
#include "aether/sequencer/sequence.h"
#include "aether/sprite2d/atlas.h"
#include "aether/sprite2d/tilemap.h"
#include "aether/terrain/asset_io.h"
#include "aether/terrain/terrain.h"
#include "aether/vfx/emitter.h"
#include "content/content_browser_panel.h"
#include "core/notifications.h"
#include "sprite2d/tilemap_document.h"
#include "sprite2d/sprite_atlas_panel.h"
#include "ui/extensions.h"
#include "workspace/editor_workspace.h"
#include "world/scene_panel.h"
#include "world/content_drop.h"
#include "aether/sprite2d/components.h"
#include "aether/sprite2d/physics2d.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cstdio>
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
    terrain::TerrainData terrain;
    terrain::Heightmap heightmap;
    heightmap.width = heightmap.height = 3;
    heightmap.heights.assign(9, 0.0f);
    const terrain::TerrainSettings terrain_settings;
    terrain::InitTerrainData(terrain, heightmap, terrain_settings);
    Write(paths.content / "World/Hills.aterrain", terrain::TerrainToJson(terrain, terrain_settings, {}).dump(2));
    terrain::FoliageLayer foliage;
    foliage.types.resize(1);
    Write(paths.content / "World/Forest.afoliage", terrain::FoliageToJson(foliage, {}).dump(2));
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
        {"World/Hills.aterrain", "Terrain"},
        {"World/Forest.afoliage", "Foliage"},
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

AETHER_TEST(Openers_SequenceReplacementClearsThePreviousKeySelection) {
    HeadlessImGui ui;
    ProjectPaths paths;
    FreshProject("sequence_selection", paths);
    EditorWorkspace ws(paths.file);
    seq::LevelSequence sequence;
    sequence.name = "First";
    seq::Track track;
    track.type = seq::TrackType::Event;
    track.id = "markers";
    track.events = {seq::EventKey{1.0f, "Original", ""}};
    sequence.tracks.push_back(track);
    CHECK(seq::SaveSequence((paths.content / "Sequences/First.asequence").string(), sequence));
    sequence.name = "Second";
    sequence.tracks[0].events[0].name = "Replacement";
    CHECK(seq::SaveSequence((paths.content / "Sequences/Second.asequence").string(), sequence));
    Write(paths.content / "Sequences/Broken.asequence", "not a sequence");

    CHECK(ws.OpenAsset("Sequences/First.asequence"));
    const i64 tool = ws.FindTool("Sequencer");
    CHECK(tool >= 0);
    if (tool < 0) return;
    const auto draw = [&] { ws.DrawTool(static_cast<usize>(tool)); };
    ui.Frame(draw);
    ui.Frame(draw);
    const auto diamond = [](ImU32 color, ImVec2* center = nullptr) {
        int count = 0;
        ImVec2 sum{};
        for (const ImDrawList* list : ImGui::GetDrawData()->CmdLists) {
            for (const ImDrawVert& v : list->VtxBuffer) {
                if (v.col != color) continue;
                sum.x += v.pos.x;
                sum.y += v.pos.y;
                ++count;
            }
        }
        if (center && count > 0) *center = ImVec2(sum.x / count, sum.y / count);
        return count > 0;
    };
    constexpr ImU32 unselected = IM_COL32(120, 190, 255, 255);
    constexpr ImU32 selected = IM_COL32(255, 200, 60, 255);
    ImVec2 center{};
    const bool found = diamond(unselected, &center);
    CHECK(found);
    if (!found) return;
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(center.x, center.y);
    ui.Frame(draw);
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    ui.Frame(draw);
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    ui.Frame(draw);
    CHECK(diamond(selected));

    CHECK(!ws.OpenAsset("Sequences/Broken.asequence"));
    ui.Frame(draw);
    CHECK(diamond(selected));
    CHECK(ws.OpenAsset("Sequences/Second.asequence"));
    ui.Frame(draw);
    CHECK(diamond(unselected) && !diamond(selected));
}

AETHER_TEST(Openers_SoundCueReplacementDoesNotSelectTheSameNodeId) {
    HeadlessImGui ui;
    ProjectPaths paths;
    FreshProject("cue_selection", paths);
    EditorWorkspace ws(paths.file);
    audio::SoundCue cue;
    cue.name = "First cue";
    cue.root = 1;
    audio::CueNode node;
    node.id = 1;
    node.type = audio::CueNodeType::Wave;
    node.x = node.y = 50.0f;
    node.sound = "blip";
    cue.nodes.push_back(node);
    Write(paths.content / "Audio/First.acue", audio::SaveCue(cue));
    cue.name = "Second cue";
    cue.nodes.front().sound = "hum";
    Write(paths.content / "Audio/Second.acue", audio::SaveCue(cue));
    Write(paths.content / "Audio/Broken.acue", "not a sound cue");

    CHECK(ws.OpenAsset("Audio/First.acue"));
    const i64 tool = ws.FindTool("Sound Cue");
    CHECK(tool >= 0);
    if (tool < 0) return;
    const auto draw = [&] { ws.DrawTool(static_cast<usize>(tool)); };
    ui.Frame(draw);
    ui.Frame(draw);
    // The only wave header gives a click target independent of window padding.
    ImVec2 center{};
    int vertices = 0;
    for (const ImDrawList* list : ImGui::GetDrawData()->CmdLists) {
        for (const ImDrawVert& v : list->VtxBuffer) {
            if (v.col != IM_COL32(60, 120, 200, 255)) continue;
            center.x += v.pos.x;
            center.y += v.pos.y;
            ++vertices;
        }
    }
    CHECK(vertices > 0);
    if (vertices == 0) return;
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(center.x / vertices, center.y / vertices);
    ui.Frame(draw);
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    ui.Frame(draw);
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    ui.Frame(draw);

    std::string rendered;
    auto& platform = ImGui::GetPlatformIO();
    platform.Platform_ClipboardUserData = &rendered;
    platform.Platform_SetClipboardTextFn = [](ImGuiContext*, const char* text) {
        *static_cast<std::string*>(ImGui::GetPlatformIO().Platform_ClipboardUserData) = text;
    };
    const auto inspect = [&] {
        ImGui::LogToClipboard();
        draw();
        ImGui::LogFinish();
    };
    ui.Frame(inspect);
    CHECK(rendered.find("Wave node 1") != std::string::npos);

    CHECK(!ws.OpenAsset("Audio/Broken.acue"));
    ui.Frame(inspect);
    CHECK(rendered.find("Wave node 1") != std::string::npos);
    CHECK(ws.OpenAsset("Audio/Second.acue"));
    ui.Frame(inspect);
    CHECK(rendered.find("Second cue") != std::string::npos && rendered.find("Wave node 1") == std::string::npos);
}

AETHER_TEST(Openers_ContentBrowserCreatesAndSelectsAtlasWithoutReplacingOne) {
    ProjectPaths paths;
    FreshProject("create_sprite_atlas", paths);
    const auto sheet = paths.content / "Characters" / "Hero.png";
    stdfs::create_directories(sheet.parent_path());
    const auto image = assets::EncodePngRgba8(16, 8, std::vector<u8>(16*8*4, 255));
    std::ofstream(sheet, std::ios::binary).write(reinterpret_cast<const char*>(image.data()), image.size());
    ContentBrowserPanel browser(paths.file);
    std::string opened;
    browser.SetAssetOpener([&](const std::string& path, std::string*) { opened = path; return true; });
    std::string error;
    CHECK(!browser.CreateSpriteAtlas("Characters/Hero.png", {5, 4}, &error) && error.find("divide") != std::string::npos);
    CHECK(!stdfs::exists(sheet.parent_path() / "Hero.aatlas") && opened.empty());
    CHECK(browser.CreateSpriteAtlas("Characters/Hero.png", {4, 4}, &error));
    CHECK(browser.Selection() == std::vector<std::string>{"Characters/Hero.aatlas"});
    CHECK(opened == "Characters/Hero.aatlas");
    const auto file = sheet.parent_path() / "Hero.aatlas";
    sprite2d::SpriteAtlas result;
    CHECK(sprite2d::LoadAtlas(file, result, &error));
    const auto* tex = browser.Database()->FindByPath("Characters/Hero.png");
    CHECK(tex && tex->guid == result.texture && result.frames.size() == 8 && result.FindClip("grid"));
    CHECK(!browser.CreateSpriteAtlas("Characters/Hero.png", {8, 8}, &error) && error.find("already exists") != std::string::npos);
    sprite2d::SpriteAtlas unchanged;
    CHECK(sprite2d::LoadAtlas(file, unchanged, &error) && unchanged.frames.size() == 8);

    const auto padded = assets::EncodePngRgba8(13, 18, std::vector<u8>(13*18*4, 255));
    std::ofstream(sheet.parent_path() / "Padded.png", std::ios::binary)
        .write(reinterpret_cast<const char*>(padded.data()), padded.size());
    CHECK(!browser.CreateSpriteAtlas("Characters/Padded.png", {4, 5, -1, 2, 3, 4}, &error));
    CHECK(error.find("non-negative") != std::string::npos);
    CHECK(!browser.CreateSpriteAtlas("Characters/Padded.png", {4, 5, 1, 2, 0, 4}, &error));
    const auto padded_atlas = sheet.parent_path() / "Padded.aatlas";
    CHECK(!stdfs::exists(padded_atlas));
    CHECK(browser.CreateSpriteAtlas("Characters/Padded.png", {4, 5, 1, 2, 3, 4}, &error));
    CHECK(opened == "Characters/Padded.aatlas");
    CHECK(sprite2d::LoadAtlas(padded_atlas, result, &error));
    CHECK(result.frames.size() == 4);
    for (usize index = 0; index < result.frames.size() && index < 4; ++index) {
        const auto& frame = result.frames[index];
        const auto i = static_cast<i32>(index);
        CHECK(frame.x == 1 + (i % 2) * 7 && frame.y == 2 + (i / 2) * 9 && frame.w == 4 && frame.h == 5);
    }
}

AETHER_TEST(Openers_ContentBrowserAuthorsValidatedNamedAtlasClips) {
    ProjectPaths paths;
    FreshProject("clips_sprite_atlas", paths);
    const auto sheet = paths.content / "Motion.png";
    const auto image = assets::EncodePngRgba8(12, 8, std::vector<u8>(12*8*4, 255));
    std::ofstream(sheet, std::ios::binary).write(reinterpret_cast<const char*>(image.data()), image.size());
    ContentBrowserPanel browser(paths.file);
    const auto file = paths.content / "Motion.aatlas";
    ContentBrowserPanel::SpriteAtlasGrid grid{4, 4};
    std::string error;
    grid.clips = {{"walk", 0, 3, 12, true}, {"walk", 3, 2, 18, false}};
    CHECK(!browser.CreateSpriteAtlas("Motion.png", grid, &error) && error.find("duplicate") != std::string::npos);
    grid.clips[1].name = "land";
    grid.clips[1].fps = 241;
    CHECK(!browser.CreateSpriteAtlas("Motion.png", grid, &error) && error.find("FPS") != std::string::npos);
    grid.clips[1].fps = 18;
    grid.clips[1].first = -1;
    CHECK(!browser.CreateSpriteAtlas("Motion.png", grid, &error) && error.find("FIRST") != std::string::npos);
    grid.clips[1].first = 5;
    CHECK(!browser.CreateSpriteAtlas("Motion.png", grid, &error) && error.find("outside") != std::string::npos);
    CHECK(!stdfs::exists(file));
    grid.clips[1].first = 3;
    CHECK(browser.CreateSpriteAtlas("Motion.png", grid, &error));
    CHECK(browser.Selection() == std::vector<std::string>{"Motion.aatlas"});
    sprite2d::SpriteAtlas atlas;
    CHECK(sprite2d::LoadAtlas(file, atlas, &error) && atlas.frames.size() == 6 && atlas.clips.size() == 2);
    const auto* walk = atlas.FindClip("walk");
    const auto* land = atlas.FindClip("land");
    CHECK(walk && walk->loop && walk->fps == 12.0f && walk->frames == std::vector<std::string>({"frame_0", "frame_1", "frame_2"}));
    CHECK(land && !land->loop && land->fps == 18.0f && land->frames == std::vector<std::string>({"frame_3", "frame_4"}));
}

AETHER_TEST(Openers_SpriteAtlasDrawsTheSourceRectangleAndKeepsThePreviousAssetOnError) {
    ProjectPaths paths;
    FreshProject("sprite_atlas", paths);
    const auto sheet = paths.content / "Sheet.png";
    const auto image = assets::EncodePngRgba8(18, 8, std::vector<u8>(18 * 8 * 4, 255));
    std::ofstream(sheet, std::ios::binary).write(reinterpret_cast<const char*>(image.data()), image.size());
    assets::AssetDatabase db(paths.content);
    db.Scan();
    const auto* texture = db.FindByPath("Sheet.png");
    CHECK(texture != nullptr);
    if (!texture) return;

    auto atlas = sprite2d::AtlasFromGrid(texture->guid, 16, 8, 8, 8);
    atlas.width = 18;
    atlas.frames[1].x = 10; // two-pixel gutter
    atlas.clips.push_back({"move", {"frame_1", "frame_0"}, 12.0f, true});
    std::string error;
    CHECK(sprite2d::SaveAtlas(paths.content / "Sheet.aatlas", atlas, &error));
    Write(paths.content / "Broken.aatlas", "not JSON");
    HeadlessImGui ui;
    EditorWorkspace ws(paths.file);
    if (!ws.OpenAsset("Sheet.aatlas", &error)) { CHECK(false); return; }
    CHECK(ShowingTool(ws, "Sprite Atlas"));
    auto& scene = ws.Scene().Document();
    bool placed = false;
    PlaceContentAsset(scene, paths.content, "Sheet.aatlas", Vec3{}, &placed);
    if (!placed || scene.Selection().size() != 1) { CHECK(false); return; }
    const Entity entity = scene.Selection().front();
    ImVec2 panel_start;
    const auto draw = [&] { ui.Frame([&] { panel_start = ImGui::GetCursorScreenPos(); ws.DrawTool(ws.Selected()); }); };
    struct Bounds { float x = 0, y = 0, w = 0, h = 0; };
    const auto region = [](float low, float high) -> Bounds {
        for (const auto* list : ImGui::GetDrawData()->CmdLists) {
            for (const auto& command : list->CmdBuffer) {
                const auto* tex = command.TexRef._TexData;
                if (!tex || tex->Width != 18 || tex->Height != 8 || command.ElemCount != 6) continue;
                float minimum_u = 1.0f, maximum_u = 0.0f;
                float x0 = 2000, y0 = 2000, x1 = 0, y1 = 0;
                for (unsigned int i = 0; i < command.ElemCount; ++i) {
                    const auto& v = list->VtxBuffer[command.VtxOffset + list->IdxBuffer[command.IdxOffset + i]];
                    minimum_u = std::min(minimum_u, v.uv.x); maximum_u = std::max(maximum_u, v.uv.x);
                    x0 = std::min(x0, v.pos.x); x1 = std::max(x1, v.pos.x);
                    y0 = std::min(y0, v.pos.y); y1 = std::max(y1, v.pos.y);
                }
                if (minimum_u == low && maximum_u == high) return {x0, y0, x1-x0, y1-y0};
            }
        }
        return {};
    };
    draw();
    CHECK(region(0.0f, 8.0f/18).w > 0);
    const auto full = region(0.0f, 1.0f);
    CHECK(full.w > 0);
    if (full.w > 0) {
        const auto click_image = [&](float fraction) {
            auto& io = ImGui::GetIO();
            io.AddMousePosEvent(full.x + fraction * full.w, full.y + 0.5f * full.h);
            draw();
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
            draw();
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
            draw();
            draw();
        };
        click_image(0.8f);
        const auto selected_crop = region(10.0f/18, 1.0f);
        CHECK(selected_crop.w > 0);
        if (selected_crop.w > 0) {
            // The assignment button is just above the cropped frame.
            auto& io = ImGui::GetIO();
            io.AddMousePosEvent(selected_crop.x + 12,
                                selected_crop.y - ImGui::GetStyle().ItemSpacing.y - 0.5f * ImGui::GetFrameHeight());
            draw();
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
            draw();
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
            draw();
            const auto* sprite = scene.GetWorld().GetComponent<sprite2d::Sprite>(entity);
            CHECK(sprite && std::string_view(sprite->frame) == "frame_1");
        }
        click_image(0.5f);
        CHECK(region(10.0f/18, 1.0f).w > 0); // the gutter did not change selection
        click_image(0.2f);
        CHECK(region(0.0f, 8.0f/18).w > 0);
    }
    CHECK(!ws.OpenAsset("Broken.aatlas", &error) && error.find("JSON") != std::string::npos);
    draw();
    CHECK(region(0.0f, 8.0f/18).w > 0);
    CHECK(ws.RunCommand("asset.open_selected_sprite_atlas")); // static Sprite reopens on its stored second frame
    draw();
    CHECK(region(10.0f/18, 1.0f).w > 0);
    CHECK(ws.OpenAsset("Sheet.aatlas", &error));
    draw();
    const auto animator_id = GetComponentId<sprite2d::SpriteAnimator>();
    if (scene.AddComponent(entity, animator_id)) {
        const auto click = [&](float x, float y) {
            auto& io = ImGui::GetIO();
            io.AddMousePosEvent(x, y);
            draw();
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
            draw();
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
            draw();
            draw();
        };
        const float line = ImGui::GetTextLineHeightWithSpacing(), row = ImGui::GetFrameHeight();
        const float x = panel_start.x + 15;
        const float combo_top = panel_start.y + 3 * line;
        click(x, combo_top + 0.5f * row);
        const auto* popup = ImGui::FindWindowByName("##Combo_00");
        CHECK(popup && popup->Active);
        if (popup && popup->Active) {
            const ImVec2 start = popup->DC.CursorStartPos;
            click(start.x + 15, start.y + line + 0.5f * ImGui::GetTextLineHeight()); // "move", below Manual frame
            CHECK(region(10.0f/18, 1.0f).w > 0); // first frame of "move", different from manual
            click(x, combo_top + row + ImGui::GetStyle().ItemSpacing.y + line + 0.5f * row);
            const auto* animator = scene.GetWorld().GetComponent<sprite2d::SpriteAnimator>(entity);
            CHECK(animator && std::string_view(animator->clip) == "move");
        }
    } else CHECK(false);
    // The selection shortcut reopens this specific atlas on the authored clip,
    // even after a normal asset open has reset the preview to manual frame zero.
    CHECK(ws.OpenAsset("Sheet.aatlas", &error));
    draw();
    CHECK(region(0.0f, 8.0f/18).w > 0);
    const auto history = scene.Commands().UndoCount();
    scene.ClearSelection();
    CHECK(!ws.RunCommand("asset.open_selected_sprite_atlas"));
    scene.SetSelection({entity});
    CHECK(ws.RunCommand("asset.open_selected_sprite_atlas"));
    CHECK(ShowingTool(ws, "Sprite Atlas"));
    draw();
    CHECK(region(10.0f/18, 1.0f).w > 0);
    CHECK(scene.Commands().UndoCount() == history && scene.Selection() == std::vector<Entity>{entity});
    scene.GetWorld().DestroyEntity(entity); // a plain selection may still hold a stale handle
    CHECK(!ws.RunCommand("asset.open_selected_sprite_atlas"));
    ProjectPaths other;
    FreshProject("sprite_atlas_other", other);
    CHECK(ws.OpenProject(other.file));
    const i64 atlas_tool = ws.FindTool("Sprite Atlas");
    CHECK(atlas_tool >= 0);
    if (atlas_tool >= 0) ws.Select(static_cast<usize>(atlas_tool));
    draw();
    CHECK(region(0.0f, 1.0f).w == 0 && region(0.0f, 8.0f/18).w == 0); // never show the previous project's atlas
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
    Write(paths.content / "World/broken.aterrain", "{ not json");
    CHECK(!ws.OpenAsset("World/broken.aterrain", &error));
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
    const auto invalid_tileset_file = paths.content / "Tilesets/Invalid.atileset";
    CHECK(sprite2d::SaveTileset(invalid_tileset_file, tileset, &error));
    assets::AssetGuid ground, invalid;
    {
        assets::AssetDatabase scan(paths.content);
        scan.Scan();
        const assets::AssetRecord* record = scan.FindByPath("Tilesets/Ground.atileset");
        CHECK(record != nullptr);
        if (record != nullptr) ground = record->guid;
        const auto* invalid_record = scan.FindByPath("Tilesets/Invalid.atileset");
        if (!invalid_record) { CHECK(false); return; }
        invalid = invalid_record->guid;
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
    broken.tileset = invalid;
    CHECK(sprite2d::SaveTilemap(paths.content / "Maps/InvalidTileset.atilemap", broken, &error));
    EditorWorkspace ws(paths.file);
    Write(invalid_tileset_file, "{invalid json");

    // The practice map has no backing asset. An invalid open leaves its edits
    // intact, but experimenting here must not block opening a real project map.
    auto& practice = ws.Tilemap();
    CHECK(practice.TilemapFile().empty());
    practice.brush = 3;
    practice.Apply(0, 2);
    CHECK(practice.dirty() && practice.Map().Get(0, 0, 2) == 3);
    CHECK(!ws.OpenAsset("Maps/Broken.atilemap", &error));
    CHECK(error.find("tileset that isn't in the project") != std::string::npos);
    CHECK(practice.dirty() && practice.TilemapFile().empty() && practice.Map().Get(0, 0, 2) == 3);

    // The map loads with its tileset, and the tool shows it.
    if (!ws.OpenAsset("Maps/Level_02.atilemap", &error)) {
        CHECK(false);
        return;
    }
    CHECK(error.empty());
    CHECK(ShowingTool(ws, "Tilemap"));
    CHECK(ws.Tilemap().Map().width == 6 && ws.Tilemap().Map().height == 4);
    CHECK(ws.Tilemap().Map().layers.size() == 1 && ws.Tilemap().Map().layers[0].name == "Ground");
    CHECK(ws.Tilemap().Map().Get(0, 1, 2) == 7);
    CHECK(ws.Tilemap().GetTileset().tile_width == 16);
    CHECK(!ws.Tilemap().dirty());
    CHECK(ws.Tilemap().TilemapFile().filename() == "Level_02.atilemap");

    // A registered tileset that cannot be parsed must not replace the current map.
    const auto revision = ws.Tilemap().Revision();
    CHECK(!ws.OpenAsset("Maps/InvalidTileset.atilemap", &error));
    CHECK(error.find("Invalid.atileset") != std::string::npos && error.find("valid JSON") != std::string::npos);
    CHECK(ws.Tilemap().Revision() == revision && !ws.Tilemap().dirty());
    CHECK(ws.Tilemap().TilemapFile().filename() == "Level_02.atilemap" &&
          ws.Tilemap().TilesetFile().filename() == "Ground.atileset");
    CHECK(ws.Tilemap().Map().Get(0, 1, 2) == 7 && ws.Tilemap().GetTileset().tile_width == 16);

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

AETHER_TEST(Openers_DifferentTilemapResetsCanvasAndRuleSelection) {
    using namespace sprite2d;
    ProjectPaths paths;
    const auto project = FreshProject("tilemap_switch_view", paths);
    std::string error;
    Tileset tiles;
    tiles.texture_width = tiles.texture_height = 16;
    std::array<i32, 16> variants{};
    tiles.AddAutotile("First rule", variants);
    tiles.AddAutotile("Second rule", variants);
    if (!SaveTileset(paths.content / "First.atileset", tiles, &error)) { CHECK(false); return; }
    tiles.autotiles[0].name = "New first";
    tiles.autotiles[1].name = "New second";
    if (!SaveTileset(paths.content / "Second.atileset", tiles, &error)) { CHECK(false); return; }
    {
        assets::AssetDatabase scan(paths.content);
        scan.Scan();
        const auto* first = scan.FindByPath("First.atileset");
        const auto* second = scan.FindByPath("Second.atileset");
        if (!first || !second) { CHECK(false); return; }
        TilemapData map;
        map.Resize(12, 8);
        map.AddLayer("Ground");
        map.tileset = first->guid;
        if (!SaveTilemap(paths.content / "First.atilemap", map, &error)) { CHECK(false); return; }
        map.Resize(3, 2);
        map.tileset = second->guid;
        if (!SaveTilemap(paths.content / "Second.atilemap", map, &error)) { CHECK(false); return; }
    }
    HeadlessImGui ui;
    EditorWorkspace ws(paths.file);
    if (!ws.OpenAsset("First.atilemap", &error)) { CHECK(false); return; }
    auto& doc = ws.Tilemap();
    ImVec2 lower_left;
    ImGuiWindow* palette = nullptr;
    const auto draw = [&] {
        const auto* parent = ImGui::GetCurrentWindowRead();
        ImGui::PushID(static_cast<int>(ws.Selected()));
        const ImGuiID palette_id = ImGui::GetID("##left");
        ImGui::PopID();
        ws.DrawTool(ws.Selected());
        for (ImGuiWindow* child : parent->DC.ChildWindows)
            if (child->ChildId == palette_id) palette = child;
        const auto padding = ImGui::GetStyle().WindowPadding;
        lower_left = {ImGui::GetItemRectMin().x + padding.x + 8, ImGui::GetItemRectMax().y - padding.y - 8};
    };
    auto& io = ImGui::GetIO();
    const auto move = [&](ImVec2 p) { io.AddMousePosEvent(p.x, p.y); ui.Frame(draw); };
    const auto button = [&](ImGuiMouseButton which, bool down) { io.AddMouseButtonEvent(which, down); ui.Frame(draw); };
    const auto click = [&] { button(ImGuiMouseButton_Left, true); button(ImGuiMouseButton_Left, false); };
    ui.Frame(draw);
    if (!palette) { CHECK(false); return; }
    const ImGuiID second_rule = ImHashStr("##autotile", 0, palette->GetID(1001));
    const auto clip = palette->InnerClipRect;
    for (f32 y = clip.Min.y + 2; y < clip.Max.y; y += 4) {
        move({clip.Min.x + 18, y});
        if (ImGui::GetHoveredID() != second_rule) continue;
        click();
        break;
    }
    CHECK(doc.brush == AutotileCell(1));
    const ImVec2 original_start{lower_left.x + 12, lower_left.y - 12};
    move(original_start);
    io.AddMouseWheelEvent(0, 2);
    ui.Frame(draw);
    button(ImGuiMouseButton_Middle, true);
    move({original_start.x + 340, original_start.y - 120});
    button(ImGuiMouseButton_Middle, false);

    CHECK(!ws.OpenAsset("Missing.atilemap", &error));
    CHECK(ws.OpenAsset("First.atilemap", &error));
    move(original_start);
    click();
    CHECK(!doc.dirty() && !doc.CanUndo()); // same view is still panned away

    if (!ws.OpenAsset("Second.atilemap", &error)) { CHECK(false); return; }
    const auto capture = project / "switch-view.txt";
    ui.Frame([&] { ImGui::LogToFile(0, capture.string().c_str()); draw(); ImGui::LogFinish(); });
    std::ifstream in(capture);
    const std::string log(std::istreambuf_iterator<char>(in), {});
    in.close();
    CHECK(log.find("Editing masks: New first") != std::string::npos);
    CHECK(log.find("Editing masks: New second") == std::string::npos);
    move({lower_left.x + 36, lower_left.y - 36});
    click();
    CHECK(doc.Map().Get(0, 1, 1) == 0 && doc.CanUndo());
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

AETHER_TEST(Openers_CreateTilemapFromExistingTilesetWithoutOverwriting) {
    ProjectPaths paths;
    FreshProject("create_tilemap", paths);
    sprite2d::Tileset tiles;
    tiles.texture_width = tiles.texture_height = 32;
    tiles.tile_width = tiles.tile_height = 16;
    std::string error;
    if (!sprite2d::SaveTileset(paths.content / "Tiles/Ground.atileset", tiles, &error)) { CHECK(false); return; }
    Write(paths.content / "Tiles/Ground.atilemap", "old map stays untouched");
    Write(paths.content / "Tiles/Ground_2.atilemap.ameta", "reserved sidecar");
    Write(paths.content / "Tiles/Broken.atileset", "not JSON");
    ContentBrowserPanel browser(paths.file);
    EditorWorkspace ws(paths.file);
    browser.SetAssetOpener([&](const std::string& path, std::string* why) { return ws.OpenAsset(path, why); });

    CHECK(!browser.CreateTilemap("Tiles/Broken.atileset", &error) && !stdfs::exists(paths.content / "Tiles/Broken.atilemap"));
    if (!browser.CreateTilemap("Tiles/Ground.atileset", &error)) { CHECK(false); return; }
    const auto result = paths.content / "Tiles/Ground_3.atilemap";
    CHECK(error.empty() && ShowingTool(ws, "Tilemap") && ws.Tilemap().TilemapFile() == result);
    sprite2d::TilemapData map;
    if (!sprite2d::LoadTilemap(result, map, &error) || map.layers.size() != 1) { CHECK(false); return; }
    CHECK(map.width == 16 && map.height == 16 && map.layers[0].name == "Ground");
    CHECK(map.Get(0, 0, 0) == sprite2d::kEmpty && map.Get(0, 15, 15) == sprite2d::kEmpty);
    const auto* source = browser.Database()->FindByPath("Tiles/Ground.atileset");
    CHECK(source && map.tileset == source->guid);

    std::ifstream old(paths.content / "Tiles/Ground.atilemap"), sidecar(paths.content / "Tiles/Ground_2.atilemap.ameta");
    std::string a, b;
    std::getline(old, a);
    std::getline(sidecar, b);
    CHECK(a == "old map stays untouched" && b == "reserved sidecar");
    ws.Tilemap().brush = 2;
    ws.Tilemap().Apply(1, 1);
    CHECK(browser.CreateTilemap("Tiles/Ground.atileset", &error));
    CHECK(stdfs::exists(paths.content / "Tiles/Ground_4.atilemap"));
    CHECK(ws.Tilemap().TilemapFile() == result && ws.Tilemap().dirty() && ws.Tilemap().Map().Get(0, 1, 1) == 2);
}

AETHER_TEST(Openers_CreatesAnEditableTilemapFromAPaddedSheet) {
    ProjectPaths paths;
    FreshProject("tileset_from_sheet", paths);
    const auto image = assets::EncodePngRgba8(20, 20, std::vector<u8>(20 * 20 * 4, 255));
    std::ofstream(paths.content / "Sheet.png", std::ios::binary)
        .write(reinterpret_cast<const char*>(image.data()), static_cast<std::streamsize>(image.size()));
    ContentBrowserPanel browser(paths.file);
    std::string error;
    const auto set_file = paths.content / "Sheet.atileset";
    CHECK(!browser.CreateTileset("Sheet.png", 8, 8, 0, 2, &error) && !stdfs::exists(set_file));
    if (!browser.CreateTileset("Sheet.png", 8, 8, 1, 2, &error)) { CHECK(false); return; }
    sprite2d::Tileset tiles;
    if (!sprite2d::LoadTileset(set_file, tiles, &error)) { CHECK(false); return; }
    const auto* source = browser.Database()->FindByPath("Sheet.png");
    CHECK(source && tiles.texture == source->guid);
    CHECK(tiles.TileCount() == 4 && tiles.margin == 1 && tiles.spacing == 2);
    f32 uv[4];
    CHECK(tiles.Uv(3, uv) && uv[0] == 11.0f/20 && uv[1] == 11.0f/20 && uv[2] == 19.0f/20 && uv[3] == 19.0f/20);
    CHECK(!browser.CreateTileset("Sheet.png", 20, 20, 0, 0, &error));
    sprite2d::Tileset unchanged;
    CHECK(sprite2d::LoadTileset(set_file, unchanged) && sprite2d::TilesetToJson(unchanged) == sprite2d::TilesetToJson(tiles));

    EditorWorkspace ws(paths.file);
    browser.SetAssetOpener([&](const std::string& path, std::string* why) { return ws.OpenAsset(path, why); });
    if (!browser.CreateTilemap("Sheet.atileset", &error)) { CHECK(false); return; }
    auto& doc = ws.Tilemap();
    CHECK(doc.TilemapFile() == paths.content / "Sheet.atilemap" && doc.GetTileset().texture == tiles.texture);
    doc.brush = 3;
    doc.Apply(2, 2);
    CHECK(doc.Save(&error));
    sprite2d::TilemapData map;
    CHECK(sprite2d::LoadTilemap(paths.content / "Sheet.atilemap", map) && map.Get(0, 2, 2) == 3);
}

AETHER_TEST(Openers_DroppedSpriteAtlasSurvivesStrictShipping) {
    ProjectPaths paths;
    FreshProject("drop_sprite_atlas", paths);
    const auto png = assets::EncodePngRgba8(18, 8, std::vector<u8>(18 * 8 * 4, 255));
    std::ofstream(paths.content / "Hero.png", std::ios::binary)
        .write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
    ContentBrowserPanel browser(paths.file);
    std::string error;
    ContentBrowserPanel::SpriteAtlasGrid grid{8, 8, 0, 0, 2, 0};
    grid.clips = {{"idle", 0, 1, 12, true}, {"walk", 0, 2, 12, true}};
    if (!browser.CreateSpriteAtlas("Hero.png", grid, &error)) { CHECK(false); return; }
    const auto* atlas = browser.Database()->FindByPath("Hero.aatlas");
    if (!atlas) { CHECK(false); return; }
    const auto guid = atlas->guid;

    SceneDocument scene;
    scene.Commands().Clear();
    const auto before = scene.Entities().size();
    bool placed = true;
    PlaceContentAsset(scene, paths.content, "Missing.aatlas", Vec3{}, &placed);
    CHECK(!placed && scene.Entities().size() == before && !scene.CanUndo());
    const auto notice = PlaceContentAsset(scene, paths.content, "Hero.aatlas", Vec3(2, 3, 0), &placed);
    if (!placed || scene.Selection().size() != 1) { std::fprintf(stderr, "%s\n", notice.c_str()); CHECK(false); return; }
    const Entity entity = scene.Selection().front();
    const auto* sprite = scene.GetWorld().GetComponent<sprite2d::Sprite>(entity);
    CHECK(sprite && sprite->atlas.guid == guid && std::string_view(sprite->frame) == "frame_0");
    CHECK(!scene.GetWorld().GetComponent<sprite2d::SpriteAnimator>(entity));
    CHECK(scene.Undo() && !scene.CanUndo() && scene.Entities().size() == before);
    CHECK(scene.Redo() && scene.Entities().size() == before + 1);
    SpriteAtlasPanel panel;
    if (!panel.Open(paths.content / "Hero.aatlas", browser.Database(), &error)) { CHECK(false); return; }
    Entity restored = kNullEntity;
    for (const Entity item : scene.Entities())
        if (scene.GetWorld().GetComponent<sprite2d::Sprite>(item)) restored = item;
    if (restored.IsNull()) { CHECK(false); return; }
    scene.ClearSelection();
    CHECK(!panel.ApplyFrame(scene, "frame_1"));
    scene.SetSelection({restored});
    CHECK(!panel.ApplyFrame(scene, "unknown"));
    CHECK(!panel.ApplyFrame(scene, "frame_0"));
    const auto original_depth = scene.Commands().UndoCount();
    CHECK(scene.SetComponentField(restored, GetComponentId<sprite2d::Sprite>(), "atlas",
                                  reflect::Any(assets::AssetRef<sprite2d::SpriteAtlasAsset>{assets::NewAssetGuid()})));
    CHECK(!panel.ApplyFrame(scene, "frame_1") && scene.Undo());
    CHECK(scene.Commands().UndoCount() == original_depth);
    if (!panel.ApplyFrame(scene, "frame_1")) { CHECK(false); return; }
    CHECK(scene.Commands().UndoCount() == original_depth + 1);
    CHECK(!panel.ApplyFrame(scene, "frame_1") && scene.Commands().UndoCount() == original_depth + 1);
    const auto frame = [&] {
        const auto* current = scene.GetWorld().GetComponent<sprite2d::Sprite>(restored);
        return current ? std::string(current->frame) : std::string{};
    };
    CHECK(frame() == "frame_1");
    CHECK(scene.Undo() && frame() == "frame_0");
    CHECK(scene.Redo() && frame() == "frame_1");
    CHECK(!panel.ApplyClip(scene, "walk")); // a static Sprite is never converted implicitly
    PlaceContentAsset(scene, paths.content, "Hero.aatlas", Vec3(4, 3, 0), &placed);
    if (!placed || scene.Selection().size() != 1) { CHECK(false); return; }
    const Entity animated = scene.Selection().front();
    const auto animator_id = GetComponentId<sprite2d::SpriteAnimator>();
    if (!scene.AddComponent(animated, animator_id)) { CHECK(false); return; }
    CHECK(scene.SetComponentField(animated, animator_id, "speed", reflect::Any(2.0f)));
    CHECK(panel.ApplyClip(scene, "idle"));
    const auto depth = scene.Commands().UndoCount();
    CHECK(!panel.ApplyClip(scene, "unknown") && !panel.ApplyClip(scene, "idle"));
    CHECK(scene.Commands().UndoCount() == depth && !panel.ApplyFrame(scene, "frame_1"));
    const auto authored_clip = [&] {
        const auto* animator = scene.GetWorld().GetComponent<sprite2d::SpriteAnimator>(animated);
        return animator ? std::string(animator->clip) : std::string{};
    };
    if (!panel.ApplyClip(scene, "walk")) { CHECK(false); return; }
    CHECK(scene.Commands().UndoCount() == depth + 1 && authored_clip() == "walk");
    CHECK(scene.Undo() && authored_clip() == "idle");
    CHECK(scene.Redo() && authored_clip() == "walk");
    sprite2d::Sprite invalid;
    sprite2d::SetSpriteFrame(invalid, "deleted-frame");
    using FrameStorage = decltype(invalid.frame);
    CHECK(scene.SetComponentField(restored, GetComponentId<sprite2d::Sprite>(), "frame",
                                  reflect::Any::CopyOf(reflect::Reflect<FrameStorage>(), invalid.frame)));
    // An animated Sprite can intentionally start on an unrendered placeholder
    // before its valid clip supplies the first frame.
    sprite2d::SetSpriteFrame(invalid, "pre-clip");
    CHECK(scene.SetComponentField(animated, GetComponentId<sprite2d::Sprite>(), "frame",
                                  reflect::Any::CopyOf(reflect::Reflect<FrameStorage>(), invalid.frame)));
    const auto scene_file = paths.content / "Placed.ascene";
    if (!scene.Save(scene_file, &error)) { CHECK(false); return; }

    ProjectSettings project;
    if (!LoadProject(paths.file, project, &error)) { CHECK(false); return; }
    project.startup_scene = "Placed.ascene";
    if (!SaveProject(paths.file, project, &error)) { CHECK(false); return; }
    const auto root = paths.root.parent_path();
    cook::PackageOptions options;
    options.cook.project_file = paths.file;
    options.cook.configuration = cook::BuildConfiguration::Shipping;
    options.cook.strict_validation = true;
    options.output_dir = root / "Shipping";
    options.player_executable = AETHER_TEST_PLAYER_PATH;
    options.output_dir = root / "Bad Shipping";
    const auto missing = cook::Package(options);
    CHECK(!missing.ok && missing.error.find("Placed.ascene: Sprite") != std::string::npos &&
          missing.error.find("frame 'deleted-frame' is missing from Hero.aatlas") != std::string::npos);
    CHECK(!stdfs::exists(options.output_dir / "Paks" / "Game.apak"));
    scene.SetSelection({restored});
    if (!panel.ApplyFrame(scene, "frame_1") || frame() != "frame_1" || !scene.Save(scene_file, &error)) { CHECK(false); return; }
    sprite2d::SpriteAnimator missing_animator;
    sprite2d::SetName(missing_animator.clip, "deleted-clip");
    using ClipStorage = decltype(missing_animator.clip);
    CHECK(scene.SetComponentField(animated, animator_id, "clip",
                                  reflect::Any::CopyOf(reflect::Reflect<ClipStorage>(), missing_animator.clip)));
    if (!scene.Save(scene_file, &error)) { CHECK(false); return; }
    options.output_dir = root / "Bad Clip Shipping";
    const auto missing_clip = cook::Package(options);
    CHECK(!missing_clip.ok && missing_clip.error.find("Placed.ascene: Sprite Animator") != std::string::npos &&
          missing_clip.error.find("clip 'deleted-clip' is missing from Hero.aatlas") != std::string::npos);
    CHECK(!stdfs::exists(options.output_dir / "Paks" / "Game.apak"));
    // Blank assignments remain valid for scenes that start clips at runtime.
    sprite2d::SetName(missing_animator.clip, "");
    CHECK(scene.SetComponentField(animated, animator_id, "clip",
                                  reflect::Any::CopyOf(reflect::Reflect<ClipStorage>(), missing_animator.clip)));
    if (!scene.Save(scene_file, &error)) { CHECK(false); return; }
    options.output_dir = root / "Deferred Clip Shipping";
    CHECK(cook::Package(options).ok);
    scene.SetSelection({animated});
    if (!panel.ApplyClip(scene, "walk") || !scene.Save(scene_file, &error)) { CHECK(false); return; }
    options.output_dir = root / "Shipping";
    const auto staged = cook::Package(options);
    if (!staged.ok) { std::fprintf(stderr, "%s\n", staged.error.c_str()); CHECK(false); return; }
    std::error_code ec;
    stdfs::rename(paths.root, root / "Unavailable Source", ec);
    if (ec || stdfs::exists(paths.file)) { CHECK(false); return; }

    player::GamePackage package;
    const auto archives = player::GamePackage::FindPaks(staged.paks_dir);
    if (archives.empty()) { CHECK(false); return; }
    for (const auto& archive : archives)
        if (!package.Mount(archive, 0, &error)) { CHECK(false); return; }
    if (!package.LoadManifest(&error)) { CHECK(false); return; }
    const auto* asset = package.FindAssetByGuid(assets::ToString(guid));
    const auto* texture = package.FindAsset("Hero.png");
    CHECK(asset && asset->path == "Hero.aatlas" && texture && !texture->cooked.empty());
    player::Game game(package);
    game.SetUserPaths(player::ResolveUserPaths("PlacedSprite", root / "Profile"));
    if (!game.LoadStartupScene(&error)) { CHECK(false); return; }
    usize matches = 0;
    game.GetWorld().ForEach<sprite2d::Sprite, Transform>([&](auto& s, auto& transform) {
        if (s.atlas.guid == guid && std::string_view(s.frame) == "frame_1" &&
            transform.position.x == 2.0f && transform.position.y == 3.0f && transform.position.z == 0.0f)
            ++matches;
    });
    CHECK(matches == 1);
    const auto runtime_frame = [&] {
        std::string name;
        game.GetWorld().ForEach<sprite2d::Sprite, sprite2d::SpriteAnimator, Transform>([&](auto& s, auto& anim, auto& t) {
            if (t.position.x == 4.0f && std::string_view(anim.clip) == "walk" && anim.speed == 2.0f)
                name = s.frame;
        });
        return name;
    };
    CHECK(runtime_frame() == "pre-clip");
    game.Tick(1.0f / 60.0f);
    CHECK(runtime_frame() == "frame_0");
    for (int i = 0; i < 2; ++i) game.Tick(1.0f / 60.0f);
    CHECK(runtime_frame() == "frame_1");
}

AETHER_TEST(Openers_AuthoredTilemapWorksInAnIndependentStrictShippingPackage) {
    ProjectPaths paths;
    FreshProject("place_authored_tilemap", paths);
    const auto image = assets::EncodePngRgba8(36, 36, std::vector<u8>(36 * 36 * 4, 255));
    std::ofstream(paths.content / "Ground.png", std::ios::binary)
        .write(reinterpret_cast<const char*>(image.data()), static_cast<std::streamsize>(image.size()));
    std::ofstream(paths.content / "Unused.png", std::ios::binary)
        .write(reinterpret_cast<const char*>(image.data()), static_cast<std::streamsize>(image.size()));
    ContentBrowserPanel browser(paths.file);
    std::string error;
    if (!browser.CreateTileset("Ground.png", 16, 16, 1, 2, &error) ||
        !browser.CreateTilemap("Ground.atileset", &error)) { CHECK(false); return; }
    const auto* record = browser.Database()->FindByPath("Ground.atilemap");
    if (!record) { CHECK(false); return; }
    // Save one real solid cell through the tilemap editor, not the cook fixture.
    EditorWorkspace ws(paths.file);
    if (!ws.OpenAsset("Ground.atilemap", &error)) { CHECK(false); return; }
    auto& painted = ws.Tilemap();
    painted.brush = 3;
    painted.Apply(2, 2);
    painted.SetSolid(3, true);
    if (!painted.Save(&error)) { CHECK(false); return; }

    SceneDocument scene;
    scene.Commands().Clear();
    bool placed = true;
    const auto before = scene.Entities().size();
    PlaceContentAsset(scene, paths.content, "Missing.atilemap", Vec3{}, &placed);
    CHECK(!placed && scene.Entities().size() == before && !scene.CanUndo());
    const Vec3 at(2.0f, 3.0f, 0.0f);
    const auto note = PlaceContentAsset(scene, paths.content, "Ground.atilemap", at, &placed);
    if (!placed || scene.Selection().size() != 1) { CHECK(false); return; }
    CHECK(note == "Placed Ground.atilemap" && scene.Entities().size() == before + 1);
    const Entity e = scene.Selection().front();
    const auto* renderer = scene.GetWorld().GetComponent<sprite2d::TilemapRenderer>(e);
    const auto* transform = scene.GetWorld().GetComponent<Transform>(e);
    CHECK(renderer && renderer->tilemap.guid == record->guid && renderer->pixels_per_unit == 16.0f);
    CHECK(transform && transform->position.x == 2.0f && transform->position.y == 3.0f && transform->position.z == 0.0f);
    CHECK(scene.Undo() && !scene.CanUndo() && scene.Entities().size() == before);
    CHECK(scene.Redo() && scene.Entities().size() == before + 1);

    const auto saved = paths.content / "Placed.ascene";
    if (!scene.Save(saved, &error)) { CHECK(false); return; }
    SceneDocument restored;
    if (!restored.Load(saved, &error)) { CHECK(false); return; }
    usize matches = 0;
    for (const Entity entity : restored.Entities()) {
        const auto* tilemap = restored.GetWorld().GetComponent<sprite2d::TilemapRenderer>(entity);
        if (tilemap && tilemap->tilemap.guid == record->guid) ++matches;
    }
    CHECK(matches == 1);

    ProjectSettings settings;
    if (!LoadProject(paths.file, settings, &error)) { CHECK(false); return; }
    settings.startup_scene = "Placed.ascene";
    if (!SaveProject(paths.file, settings, &error)) { CHECK(false); return; }
    cook::PackageOptions options;
    options.cook.project_file = paths.file;
    options.cook.configuration = cook::BuildConfiguration::Shipping;
    options.cook.strict_validation = true;
    options.output_dir = paths.root.parent_path() / "Rejected";
    options.player_executable = AETHER_TEST_PLAYER_PATH;
    const auto map_path = paths.content / "Ground.atilemap";
    const auto set_path = paths.content / "Ground.atileset";
    auto bad_map = painted.Map();
    auto bad_set = painted.GetTileset();
    f32 missing_uv[4];
    CHECK(bad_set.TileCount() == 4 && !bad_set.Uv(4, missing_uv));
    bad_map.Set(0, 2, 2, 4);
    // A non-solid out-of-sheet tile would silently disappear even when the
    // tileset has no invalid flags. A strict package must reject that too.
    if (!sprite2d::SaveTilemap(map_path, bad_map, &error)) { CHECK(false); return; }
    options.output_dir = paths.root.parent_path() / "Rejected Visible Tile";
    const auto painted_tile = cook::Package(options);
    CHECK(!painted_tile.ok && painted_tile.error.find(
          "Ground.atilemap: layer 0 cell (2, 2) uses tile index 4 outside the 4-tile grid in Ground.atileset") != std::string::npos);
    CHECK(!stdfs::exists(options.output_dir / "Paks/Game.apak"));
    options.output_dir = paths.root.parent_path() / "Rejected";
    bad_set.SetSolid(4, true);
    if (!sprite2d::SaveTilemap(map_path, bad_map, &error) || !sprite2d::SaveTileset(set_path, bad_set, &error)) {
        CHECK(false); return;
    }
    const auto rejected = cook::Package(options);
    if (rejected.ok) {
        // Diagnose a regression using only the artifact that incorrectly shipped.
        player::GamePackage shipped;
        if (shipped.Mount(rejected.paks_dir / "Game.apak", 0, &error) && shipped.LoadManifest(&error)) {
            player::Game game(shipped);
            game.SetUserPaths(player::ResolveUserPaths("InvalidTile", options.output_dir / "Invalid Profile"));
            sprite2d::RayHit2D contact;
            if (game.LoadStartupScene(&error) && game.Physics2D() &&
                game.Physics2D()->Raycast({4.5f, 8.0f}, {0.0f, -1.0f}, 4.0f, contact) &&
                contact.tile && contact.tile_x == 2 && contact.tile_y == 2)
                std::fprintf(stderr, "Invalid Shipping artifact has collision on tile 4 with no valid texture coordinates.\n");
        }
    }
    CHECK(!rejected.ok && rejected.error.find("Ground.atileset: solid tile index 4 is outside its 4-tile grid") != std::string::npos);
    CHECK(!stdfs::exists(options.output_dir / "Paks/Game.apak"));
    if (!sprite2d::SaveTilemap(map_path, painted.Map(), &error) ||
        !sprite2d::SaveTileset(set_path, painted.GetTileset(), &error)) { CHECK(false); return; }

    // A missing autotile rule resolves to empty even on a solid layer.
    auto rule_map = painted.Map();
    auto rule_set = painted.GetTileset();
    sprite2d::Autotile rule;
    rule.name = "Ground";
    rule.tiles.fill(3);
    rule_set.autotiles.push_back(rule);
    rule_map.Set(0, 5, 5, sprite2d::AutotileCell(1));
    CHECK(sprite2d::ResolveTile(rule_map, rule_set, 0, 5, 5) == sprite2d::kEmpty);
    if (!sprite2d::SaveTileset(set_path, rule_set, &error) ||
        !sprite2d::SaveTilemap(map_path, rule_map, &error)) { CHECK(false); return; }
    options.output_dir = paths.root.parent_path() / "Rejected Missing Autotile";
    const auto missing_rule = cook::Package(options);
    CHECK(!missing_rule.ok && missing_rule.error.find(
          "Ground.atilemap: layer 0 cell (5, 5) uses missing autotile rule 1 in Ground.atileset") != std::string::npos);
    CHECK(!stdfs::exists(options.output_dir / "Paks/Game.apak"));
    rule_map.Set(0, 5, 5, sprite2d::AutotileCell(0));
    CHECK(sprite2d::ResolveTile(rule_map, rule_set, 0, 5, 5) == 3);
    if (!sprite2d::SaveTilemap(map_path, rule_map, &error)) { CHECK(false); return; }

    options.output_dir = paths.root.parent_path() / "Shipping";
    const auto staged = cook::Package(options);
    if (!staged.ok) { std::fprintf(stderr, "strict authored tilemap package: %s\n", staged.error.c_str()); CHECK(false); return; }
    std::vector<std::string> problems;
    CHECK(cook::VerifyPackageManifest(options.output_dir, problems) && problems.empty());

    // Exclude the source project before exercising both the runtime and staged exe.
    std::error_code move_error;
    stdfs::rename(paths.root, paths.root.parent_path() / "Unavailable Source", move_error);
    if (move_error || stdfs::exists(paths.file)) { CHECK(false); return; }

    player::GamePackage package;
    const auto archives = player::GamePackage::FindPaks(staged.paks_dir);
    if (archives.empty()) { CHECK(false); return; }
    for (const auto& archive : archives)
        if (!package.Mount(archive, 0, &error)) { CHECK(false); return; }
    if (!package.LoadManifest(&error)) { CHECK(false); return; }
    CHECK(package.Manifest().configuration == cook::BuildConfiguration::Shipping);
    const auto* map_asset = package.FindAssetByGuid(assets::ToString(record->guid));
    if (!map_asset || map_asset->path != "Ground.atilemap") { CHECK(false); return; }
    const auto read = [&](const std::string& path) {
        std::vector<u8> bytes;
        if (!package.ReadContent(path, bytes)) return nlohmann::json{};
        return nlohmann::json::parse(bytes.begin(), bytes.end(), nullptr, false);
    };
    sprite2d::TilemapData map_from_pak;
    if (!sprite2d::TilemapFromJson(read(map_asset->path), map_from_pak) || map_from_pak.Get(0, 2, 2) != 3) { CHECK(false); return; }
    const auto* set_asset = package.FindAssetByGuid(assets::ToString(map_from_pak.tileset));
    if (!set_asset || set_asset->path != "Ground.atileset") { CHECK(false); return; }
    sprite2d::Tileset set_from_pak;
    if (!sprite2d::TilesetFromJson(read(set_asset->path), set_from_pak) || !set_from_pak.IsSolid(3)) { CHECK(false); return; }
    CHECK(sprite2d::ResolveTile(map_from_pak, set_from_pak, 0, 5, 5) == 3);
    f32 uv[4];
    CHECK(set_from_pak.Uv(3, uv) && uv[0] == 19.0f/36 && uv[1] == 19.0f/36 && uv[2] == 35.0f/36 && uv[3] == 35.0f/36);
    const auto* texture = package.FindAssetByGuid(assets::ToString(set_from_pak.texture));
    CHECK(texture && texture->path == "Ground.png" && !package.FindAsset("Unused.png"));
    std::vector<u8> cooked_pixels;
    CHECK(texture && !texture->cooked.empty() && package.Files().Read(texture->cooked, cooked_pixels) && !cooked_pixels.empty());

    player::Game game(package);
    game.SetUserPaths(player::ResolveUserPaths("AuthoredMap", paths.root.parent_path() / "Runtime Profile"));
    if (!game.LoadStartupScene(&error) || !game.Physics2D()) { CHECK(false); return; }
    sprite2d::RayHit2D hit;
    CHECK(game.Physics2D()->Raycast(sprite2d::Vec2{4.5f, 8.0f}, sprite2d::Vec2{0.0f, -1.0f}, 4.0f, hit) &&
          hit.tile && hit.tile_x == 2 && hit.tile_y == 2 && hit.distance == 2.0f);
    CHECK(!game.Physics2D()->Raycast(sprite2d::Vec2{5.5f, 8.0f}, sprite2d::Vec2{0.0f, -1.0f}, 4.0f, hit));

    platform::ChildProcess player;
    const auto user = paths.root.parent_path() / "Staged Profile";
    if (!player.Start(staged.game_executable, {"--headless", "--frames", "3", "--user-dir", user.string()},
                      options.output_dir, &error)) { CHECK(false); return; }
    int code = -1;
    CHECK(player.Wait(&code) && code == 0);
}

AETHER_TEST(Openers_JumpProofDetectsAChangeToAnAuthoredTile) {
    ProjectPaths paths;
    FreshProject("authored_jump_proof", paths);
    const auto image = assets::EncodePngRgba8(16, 16, std::vector<u8>(16 * 16 * 4, 255));
    std::ofstream(paths.content / "Floor.png", std::ios::binary)
        .write(reinterpret_cast<const char*>(image.data()), static_cast<std::streamsize>(image.size()));
    ContentBrowserPanel browser(paths.file);
    std::string error;
    if (!browser.CreateTileset("Floor.png", 16, 16, 0, 0, &error) ||
        !browser.CreateTilemap("Floor.atileset", &error)) { CHECK(false); return; }
    EditorWorkspace ws(paths.file);
    if (!ws.OpenAsset("Floor.atilemap", &error)) { CHECK(false); return; }
    auto& map = ws.Tilemap();
    map.brush = 0;
    map.Apply(1, 1);
    map.SetSolid(0, true);
    if (!map.Save(&error)) { CHECK(false); return; }

    SceneDocument scene;
    bool placed = false;
    PlaceContentAsset(scene, paths.content, "Floor.atilemap", Vec3(0, 0, 0), &placed);
    if (!placed) { CHECK(false); return; }
    sprite2d::Collider2D body;
    body.half_extents = {0.25f, 0.5f};
    (void)scene.GetWorld().CreateEntity(EntityName{"Player"}, IdComponent{NewEntityGuid()}, Tags{{"Player"}},
        Transform{Vec3(1.5f, 2.55f, 0), Quaternion::Identity()}, sprite2d::Rigidbody2D{}, body, sprite2d::PlatformerController2D{});
    if (!scene.Save(paths.content / "Jump.ascene", &error)) { CHECK(false); return; }

    const auto input_dir = paths.content / "Input";
    stdfs::create_directories(input_dir);
    input::InputMappingContext context;
    context.name = "Gameplay";
    input::InputBinding jump;
    jump.action = "Jump";
    jump.key = input::Key::Space;
    context.bindings.push_back(jump);
    if (!input::SaveInputAction({"Jump", input::ActionValueType::Bool}, input_dir / "Jump.aaction", &error) ||
        !input::SaveMappingContext(context, input_dir / "Gameplay.amapping", &error)) { CHECK(false); return; }
    ProjectSettings settings;
    if (!LoadProject(paths.file, settings, &error)) { CHECK(false); return; }
    settings.startup_scene = "Jump.ascene";
    settings.always_cook = {"Input/"};
    if (!SaveProject(paths.file, settings, &error)) { CHECK(false); return; }

    const auto root = paths.root.parent_path();
    cook::PackageOptions options;
    options.cook.project_file = paths.file;
    options.cook.configuration = cook::BuildConfiguration::Shipping;
    options.cook.strict_validation = true;
    options.player_executable = AETHER_TEST_PLAYER_PATH;
    options.output_dir = root / "Original";
    const auto before = cook::Package(options);
    if (!before.ok) { std::fprintf(stderr, "%s\n", before.error.c_str()); CHECK(false); return; }
    // Only this authored cell differs; both builds use the same scene and input.
    map.tool = editor::TileTool::Erase;
    map.Apply(1, 1);
    if (!map.Save(&error)) { CHECK(false); return; }
    options.output_dir = root / "Edited";
    const auto after = cook::Package(options);
    if (!after.ok) { std::fprintf(stderr, "%s\n", after.error.c_str()); CHECK(false); return; }

    const auto route = root / "jump.json";
    Write(route, R"({"frames":40,"events":[{"frame":15,"key":"Space","value":1},{"frame":25,"key":"Space","value":0}],"target":{"min_x":1.4,"max_x":1.6,"min_y":3.2,"max_y":9}})");
    std::error_code move_error;
    stdfs::rename(paths.root, root / "Unavailable Source", move_error);
    if (move_error || stdfs::exists(paths.file)) { CHECK(false); return; }
    const auto run = [&](const cook::PackageReport& build, const stdfs::path& profile) {
        platform::ChildProcess child;
        if (!child.Start(build.game_executable, {"--jump-proof", route.string(), "--user-dir", profile.string()},
                         build.game_executable.parent_path(), &error)) return -1;
        int code = -1;
        return child.Wait(&code) ? code : -1;
    };
    CHECK(run(before, root / "Original Profile") == 0);
    CHECK(run(after, root / "Edited Profile") == 5); // a completed route missed, not a setup failure
}
