#include "workspace/editor_workspace.h"

#include "ai/bt_editor.h"
#include "ai/nav_panel.h"
#include "anim/anim_graph_editor.h"
#include "anim/anim_viewer.h"
#include "anim/blend_space_editor.h"
#include "audio/cue_editor.h"
#include "audio/mixer_panel.h"
#include "devtools/console_panel.h"
#include "packaging/build_window.h"
#include "packaging/new_project_panel.h"
#include "packaging/plugins_panel.h"
#include "devtools/crash_reporter.h"
#include "devtools/profiler_panel.h"
#include "graph/blueprint_editor.h"
#include "graph/material_editor.h"
#include "net/net_panels.h"
#include "ui/code_editor.h"
#include "uidesign/ui_designer.h"
#include "vfx/vfx_editor.h"
#include "world/world_panels.h"

#include "aether/blueprint/nodes.h"
#include "aether/core/console.h"
#include "aether/nav/components.h"
#include "aether/nav/crowd.h"
#include "aether/project/project.h"
#include "aether/reflection/serialize.h"
#include "aether/scene/gameplay.h"
#include "aether/scene/components.h"
#include "aether/streaming/partition.h"

#include <imgui.h>

#include <cctype>
#include <cstring>
#include <fstream>
#include <functional>
#include <vector>

namespace aether::editor {

using nlohmann::json;

namespace {

constexpr const char* kLuauSample = R"(-- A door that opens when the player comes near.
local Door = {}

Door.open_distance = 3.0
Door.speed = 2.0

function Door:BeginPlay()
    self.angle = 0
    self.target = 0
    print("Door ready: " .. self.entity:Name())
end

function Door:Tick(dt)
    local player = world:FindPlayer()
    if player then
        local d = (player.position - self.transform.position):Length()
        self.target = d < Door.open_distance and 90 or 0
    end
    self.angle = self.angle + (self.target - self.angle) * math.min(1, dt * Door.speed)
    self.transform.rotation = Quaternion.FromEuler(0, self.angle, 0)
end

return Door
)";

// The floor a navigation sample bakes on: a 20 m square.
bool SampleFloor(const ModelRenderer& m, std::vector<Vec3>& v, std::vector<u32>& i) {
    if (std::strcmp(m.asset_path, "floor") != 0) return false;
    nav::NavGeometry g;
    g.AddPlane(Vec3(0, 0, 0), 10, 10);
    v = g.vertices;
    i = g.indices;
    return true;
}

bool StartsWithNoCase(const char* text, std::string_view prefix) {
    usize k = 0;
    for (; k < prefix.size() && text[k]; ++k) {
        if (std::tolower(static_cast<unsigned char>(text[k])) != std::tolower(static_cast<unsigned char>(prefix[k]))) {
            return false;
        }
    }
    return k == prefix.size();
}

} // namespace

struct EditorWorkspace::Impl {
    struct Tool {
        const char* name;
        const char* category;
        std::function<void()> draw;
        bool window_open = false;
    };
    std::vector<Tool> tools;
    usize selected = 0;

    // Scripting.
    std::unique_ptr<BlueprintDocument> blueprint_doc;
    std::unique_ptr<BlueprintEditor> blueprint_editor;
    std::unique_ptr<CodeDocument> script_doc;
    CodeEditorState script_state;
    // Rendering.
    std::unique_ptr<MaterialDocument> material_doc;
    std::unique_ptr<MaterialEditor> material_editor;
    std::unique_ptr<ParticleSystemDocument> particle_doc;
    std::unique_ptr<ParticleEditor> particle_editor;
    // Animation.
    std::unique_ptr<AnimGraphDocument> anim_doc;
    std::unique_ptr<AnimGraphEditor> anim_editor;
    std::unique_ptr<BlendSpaceDocument> blend_doc;
    std::unique_ptr<BlendSpaceEditor> blend_editor;
    anim::Skeleton skeleton;
    anim::AnimationClip clip;
    std::unique_ptr<ClipViewer> clip_viewer;
    // AI.
    std::unique_ptr<BehaviorTreeDocument> bt_doc;
    std::unique_ptr<BehaviorTreeEditor> bt_editor;
    World nav_level;
    std::unique_ptr<nav::NavWorld> nav_world;
    std::unique_ptr<nav::NavCrowd> nav_crowd;
    std::unique_ptr<NavigationPanel> nav_panel;
    // Audio.
    std::unique_ptr<SoundCueDocument> cue_doc;
    std::unique_ptr<SoundCueEditor> cue_editor;
    std::unique_ptr<audio::Mixer> mixer;
    std::unique_ptr<MixerPanel> mixer_panel;
    // UI.
    std::unique_ptr<UILayoutDocument> ui_doc;
    std::unique_ptr<UIDesigner> ui_designer;
    // World.
    terrain::TerrainData terrain_data;
    terrain::TerrainSettings terrain_settings;
    std::unique_ptr<TerrainEditDocument> terrain_doc;
    std::unique_ptr<TerrainToolPanel> terrain_panel;
    terrain::FoliageLayer foliage_layer;
    std::unique_ptr<FoliagePaintDocument> foliage_doc;
    std::unique_ptr<FoliagePanel> foliage_panel;
    std::unique_ptr<SplineEditDocument> spline_doc;
    std::unique_ptr<SplinePanel> spline_panel;
    streaming::PartitionResult partition;
    World streamed_world;
    std::unique_ptr<streaming::WorldStreamer> streamer;
    std::unique_ptr<WorldPartitionPanel> partition_panel;
    // Networking.
    World net_level;
    std::unique_ptr<NetPlaySession> net_session;
    std::unique_ptr<NetPlayPanel> net_panel;
    std::unique_ptr<NetProfilerPanel> net_profiler;
    // Debug.
    Console console;
    std::unique_ptr<ConsolePanel> console_panel;
    std::unique_ptr<ProfilerPanel> profiler_panel;
    std::unique_ptr<CrashReporterDialog> crash_dialog;
    // Project.
    std::filesystem::path project_file;
    std::unique_ptr<ProjectSettingsPanel> project_settings;
    std::unique_ptr<BuildPackageWindow> build_window;
    std::unique_ptr<PluginsPanel> plugins_panel;
    NewProjectPanel new_project;
    plugin::PluginManager plugins; // the project's, with their modules started

    explicit Impl(std::filesystem::path project) : project_file(std::move(project)) {
        BuildBlueprint();
        BuildScript();
        BuildMaterial();
        BuildParticles();
        BuildAnimation();
        BuildBehaviorTree();
        BuildNavigation();
        BuildAudio();
        BuildUI();
        BuildWorld();
        BuildNetworking();
        BuildDebug();
        BuildProject();

        tools = {
            {"Blueprint - BP_Door", "Scripting", [this] { blueprint_editor->Draw(); }},
            {"Luau Script - door.luau", "Scripting", [this] { DrawCodeEditor("##luau", *script_doc, script_state); }},
            {"Material - M_Lit", "Rendering", [this] { material_editor->Draw(); }},
            {"Particle System - PS_Sparks", "Rendering", [this] { particle_editor->Draw(); }},
            {"Animation - ABP_Character", "Animation", [this] { DrawAnimation(); }}, // graph, blend space, clip,
            {"Behavior Tree - BT_Guard", "AI", [this] { bt_editor->Draw(); }},
            {"Navigation", "AI", [this] { nav_panel->Draw(); }},
            {"Sound Cue - SC_Blip", "Audio", [this] { cue_editor->Draw(); }},
            {"Mixer", "Audio", [this] { mixer_panel->Draw(); }},
            {"UI Designer - WBP_MainMenu", "UI", [this] { ui_designer->Draw(); }},
            {"Terrain", "World", [this] { terrain_panel->Draw(); }},
            {"Foliage", "World", [this] { foliage_panel->Draw(); }},
            {"Spline", "World", [this] { spline_panel->Draw(); }},
            {"World Partition", "World", [this] { partition_panel->Draw(); }},
            {"Net Play", "Networking", [this] { net_panel->Draw(); }},
            {"Net Profiler", "Networking", [this] { net_profiler->Draw(); }},
            {"Console", "Debug", [this] { console_panel->Draw(); }},
            {"Profiler", "Debug", [this] { profiler_panel->Draw(); }},
            {"Crash Reports", "Debug", [this] { DrawCrashReports(); }},
            {"New Project", "Project", [this] { new_project.Draw(); }},
            {"Project Settings", "Project", [this] { DrawProjectTool(true); }},
            {"Build and Package", "Project", [this] { DrawProjectTool(false); }},
            {"Plugins", "Project", [this] {
                 if (plugins_panel) {
                     plugins_panel->Draw();
                 } else {
                     ImGui::TextDisabled("No project is open.");
                 }
             }},
        };
    }

    void BuildBlueprint() {
        bp::Blueprint blueprint;
        bp::Variable open;
        open.name = "IsOpen";
        open.type = bp::PinType::Of(bp::ValueType::Bool);
        open.default_value = false;
        blueprint.variables = {open};
        bp::Graph g;
        g.name = "EventGraph";
        bp::GraphBuilder b(g);
        const auto begin = b.Add("Event.BeginPlay", json::object(), 0, 0);
        const auto branch = b.Add("Flow.Branch", json::object(), 280, 0);
        const auto get = b.Add("Var.Get:IsOpen", json::object(), 80, 150);
        const auto print = b.Add("Debug.Print", json::object(), 560, 0);
        b.Connect(begin, "then", branch, "exec")
            .Connect(get, "value", branch, "condition")
            .Connect(branch, "false", print, "exec");
        const auto tick = b.Add("Event.Tick", json::object(), 0, 260);
        const auto mul = b.Add("Math.Multiply:float", json::object(), 280, 330);
        const auto set = b.Add("Var.Set:IsOpen", json::object(), 560, 260);
        b.Connect(tick, "then", set, "exec").Connect(tick, "delta_seconds", mul, "a");
        blueprint.graphs.push_back(g);
        blueprint_doc = std::make_unique<BlueprintDocument>(blueprint);
        blueprint_editor = std::make_unique<BlueprintEditor>(*blueprint_doc);
    }

    void BuildScript() { script_doc = std::make_unique<CodeDocument>(kLuauSample); }

    void BuildMaterial() {
        using mat::PinType;
        mat::Material m;
        m.parameters.push_back({"Albedo", PinType::Texture, {}, "guid", ""});
        m.parameters.push_back({"Tint", PinType::Float3, {1, 0.85f, 0.7f, 0}, "", "Surface"});
        m.parameters.push_back({"Roughness", PinType::Float, {0.5f, 0, 0, 0}, "", "Surface"});
        m.parameters.push_back({"Metallic", PinType::Float, {0.1f, 0, 0, 0}, "", "Surface"});
        const auto out = m.Add("Material.Output", {}, 760, 0);
        const auto tex = m.Add("Param.Texture:Albedo", {}, 0, 0);
        const auto sample = m.Add("Texture.Sample", {}, 200, 0);
        const auto mul = m.Add("Math.Multiply", {}, 470, 0);
        const auto tint = m.Add("Param.Vector:Tint", {}, 200, 190);
        const auto rough = m.Add("Param.Scalar:Roughness", {}, 470, 230);
        const auto metal = m.Add("Param.Scalar:Metallic", {}, 470, 330);
        m.Connect(tex, "value", sample, "texture").Connect(sample, "rgb", mul, "a").Connect(tint, "value", mul, "b");
        m.Connect(mul, "result", out, "BaseColor")
            .Connect(rough, "value", out, "Roughness")
            .Connect(metal, "value", out, "Metallic");
        material_doc = std::make_unique<MaterialDocument>(m);
        material_editor = std::make_unique<MaterialEditor>(*material_doc);
    }

    void BuildParticles() {
        particle_doc = std::make_unique<ParticleSystemDocument>();
        particle_editor = std::make_unique<ParticleEditor>(*particle_doc);
    }

    void BuildAnimation() {
        using namespace aether::anim;
        anim_doc = std::make_unique<AnimGraphDocument>();
        AnimGraphDocument& doc = *anim_doc;
        doc.AddVariable(VarType::Float);
        doc.RenameVariable("NewVar", "Speed");
        doc.AddVariable(VarType::Bool);
        doc.RenameVariable("NewVar", "IsFalling");
        const u32 slot = doc.AddNode(AnimNodeKind::Slot, 420, 0);
        u32 machine_node = 0;
        const std::string machine = doc.AddMachine(100, 0, &machine_node);
        doc.ConnectPose(machine_node, slot, 0);
        doc.SetOutput(slot);
        doc.AddState(machine, 0, 0);
        doc.AddState(machine, 320, -60);
        doc.AddState(machine, 320, 140);
        doc.RenameState(machine, "State", "Idle");
        doc.RenameState(machine, "State_1", "Walk");
        doc.RenameState(machine, "State_2", "Fall");
        const i32 walk = doc.AddTransition(machine, 0, 1);
        doc.Edit("Condition", [&](AnimGraph& g) {
            g.machines[0].transitions[static_cast<usize>(walk)].conditions.push_back(
                {"Speed", CompareOp::Greater, 0.1f});
        });
        const i32 idle = doc.AddTransition(machine, 1, 0);
        doc.Edit("Condition", [&](AnimGraph& g) {
            g.machines[0].transitions[static_cast<usize>(idle)].conditions.push_back({"Speed", CompareOp::Less, 0.1f});
        });
        doc.AddTransition(machine, 1, 2);
        doc.AddTransition(machine, 2, 0);
        doc.Edit("Clips", [&](AnimGraph& g) {
            const auto& states = g.machines[0].states;
            for (AnimNode& n : g.nodes) {
                if (n.kind != AnimNodeKind::Clip) continue;
                n.asset = n.id == states[0].pose ? "Idle" : n.id == states[1].pose ? "Walk" : "Fall";
            }
        });
        anim_editor = std::make_unique<AnimGraphEditor>(doc);
        anim_editor->OpenTab(machine);

        BlendSpace space;
        space.dimensions = 2;
        space.x = {"Right", -1, 1};
        space.y = {"Forward", 0, 2};
        blend_doc = std::make_unique<BlendSpaceDocument>(space);
        blend_doc->AddSample("Idle", 0, 0);
        blend_doc->AddSample("Walk_Fwd", 0, 1);
        blend_doc->AddSample("Run_Fwd", 0, 2);
        blend_doc->AddSample("Strafe_L", -1, 1);
        blend_doc->AddSample("Strafe_R", 1, 1);
        blend_editor = std::make_unique<BlendSpaceEditor>(*blend_doc);
        blend_editor->preview_x = 0.3f;
        blend_editor->preview_y = 1.4f;

        skeleton.bones = {{"root", -1, {}, Mat4::Identity()},  {"spine", 0, {}, Mat4::Identity()},
                          {"arm_l", 1, {}, Mat4::Identity()},  {"hand_l", 2, {}, Mat4::Identity()},
                          {"arm_r", 1, {}, Mat4::Identity()},  {"leg_l", 0, {}, Mat4::Identity()},
                          {"leg_r", 0, {}, Mat4::Identity()}};
        clip.duration = 1.2f;
        clip.tracks.resize(skeleton.bones.size());
        clip.tracks[2].translation.times = {0.0f, 0.3f, 0.6f, 0.9f, 1.2f};
        clip.tracks[2].translation.values = {Vec3(0, 0, 0), Vec3(0, 1.5f, 0), Vec3(0, 0.2f, 0), Vec3(0, 1.2f, 0),
                                             Vec3(0, 0, 0)};
        clip.notifies = {{"Footstep_L", 0.3f}, {"Footstep_R", 0.9f}};
        clip_viewer = std::make_unique<ClipViewer>(skeleton, clip);
        clip_viewer->selected_bone = 2;
        clip_viewer->SetTime(0.45f);
    }

    // The graph above, with the blend space and the clip side by side under it.
    void DrawAnimation() {
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        if (ImGui::BeginChild("##graph", ImVec2(avail.x, avail.y * 0.6f), ImGuiChildFlags_Borders)) {
            anim_editor->Draw();
        }
        ImGui::EndChild();
        if (ImGui::BeginChild("##blend", ImVec2(avail.x * 0.55f, 0), ImGuiChildFlags_Borders)) {
            blend_editor->Draw();
        }
        ImGui::EndChild();
        ImGui::SameLine();
        if (ImGui::BeginChild("##clip", ImVec2(0, 0), ImGuiChildFlags_Borders)) {
            clip_viewer->Draw();
        }
        ImGui::EndChild();
    }

    void BuildBehaviorTree() {
        bt_doc = std::make_unique<BehaviorTreeDocument>();
        BehaviorTreeDocument& bt = *bt_doc;
        bt.AddKey("Enemy", ai::BlackboardType::Entity);
        bt.AddKey("Health", ai::BlackboardType::Float);
        bt.AddNode({}, ai::BtNodeType::Sequence, 0);
        bt.AddNode({0}, ai::BtNodeType::MoveTo);
        bt.SetNodeField({0, 0}, "key", "Enemy");
        bt.AddNode({0}, ai::BtNodeType::Log);
        bt.AddDecorator({0}, ai::BtDecoratorType::BlackboardCondition);
        bt.AddNode({}, ai::BtNodeType::Wait);
        bt_editor = std::make_unique<BehaviorTreeEditor>(bt);
    }

    // A floor with a crate on it, baked.
    void BuildNavigation() {
        ModelRenderer floor;
        SetModelPath(floor, "floor");
        nav_level.CreateEntity(Transform{}, floor);
        nav_level.CreateEntity(Transform{Vec3(3, 0.5f, 3), Quaternion::Identity()}, NavObstacle{});
        nav_world = std::make_unique<nav::NavWorld>(nav_level, SampleFloor);
        nav_crowd = std::make_unique<nav::NavCrowd>(nav_level, *nav_world);
        nav_panel = std::make_unique<NavigationPanel>(nav_level, *nav_world, nav_crowd.get());
        nav_panel->Bake();
    }

    void BuildAudio() {
        cue_doc = std::make_unique<SoundCueDocument>();
        cue_doc->SetSounds({"blip", "hum"});
        const u32 random = cue_doc->AddNode(audio::CueNodeType::Random, 300, 0);
        const u32 a = cue_doc->AddNode(audio::CueNodeType::Wave, 0, 0, "blip");
        const u32 b = cue_doc->AddNode(audio::CueNodeType::Wave, 0, 100, "hum");
        cue_doc->Connect(a, random, 0);
        cue_doc->Connect(b, random, 1);
        cue_editor = std::make_unique<SoundCueEditor>(*cue_doc);
        mixer = std::make_unique<audio::Mixer>(48000);
        mixer->AddDefaultBuses();
        mixer_panel = std::make_unique<MixerPanel>(*mixer);
    }

    void BuildUI() {
        ui_doc = std::make_unique<UILayoutDocument>();
        const auto play = ui_doc->AddWidget("Button", {});
        const auto quit = ui_doc->AddWidget("Button", {});
        if (play) {
            ui_doc->Rename(*play, "Play");
            if (const auto label = ui_doc->AddWidget("Text", *play)) ui_doc->SetProperty(*label, "text", "Play");
        }
        if (quit) {
            ui_doc->Rename(*quit, "Quit");
            if (const auto label = ui_doc->AddWidget("Text", *quit)) ui_doc->SetProperty(*label, "text", "Quit");
        }
        ui_designer = std::make_unique<UIDesigner>(*ui_doc);
    }

    void BuildWorld() {
        using namespace aether::terrain;
        Heightmap hm;
        hm.width = hm.height = 63;
        hm.cell_size = 1.0f;
        hm.heights.assign(63u * 63u, 0.0f);
        terrain_settings.verts_per_chunk = 32;
        terrain_settings.chunk_world_size = 31.0f;
        InitTerrainData(terrain_data, hm, terrain_settings);
        terrain_data.layers.push_back(TerrainLayer{"Grass", Vec3(0.3f, 0.6f, 0.2f), 0.0f, 0.9f, 1.0f});
        terrain_data.layers.push_back(TerrainLayer{"Rock", Vec3(0.5f, 0.5f, 0.5f), 0.0f, 0.9f, 1.0f});
        std::vector<SplatmapLayer> layers(2);
        terrain_doc =
            std::make_unique<TerrainEditDocument>(terrain_data, terrain_settings, BuildSplatmap(64, layers, {}));
        terrain_panel = std::make_unique<TerrainToolPanel>(*terrain_doc);

        foliage_layer.types.resize(1);
        foliage_doc = std::make_unique<FoliagePaintDocument>(foliage_layer);
        foliage_panel = std::make_unique<FoliagePanel>(*foliage_doc);

        spline_doc = std::make_unique<SplineEditDocument>();
        spline_doc->AddPoint(Vec3());
        spline_doc->AddPoint(Vec3(10, 0, 0));
        spline_doc->AddPoint(Vec3(20, 0, 8));
        spline_panel = std::make_unique<SplinePanel>(*spline_doc);

        // A level partitioned into 64 m cells, streamed around a camera at the origin.
        RegisterStreamingComponents();
        World source;
        for (int i = 0; i < 6; ++i) {
            source.CreateEntity(Transform{Vec3(10.0f + 50.0f * static_cast<f32>(i), 0, 10.0f + 25.0f * static_cast<f32>(i % 3)),
                                          Quaternion::Identity()});
        }
        streaming::PartitionSettings ps;
        ps.cell_size = 64.0f;
        partition = streaming::PartitionWorld(source, ps);
        streamed_world.CreateEntity(Transform{Vec3(0, 0, 0), Quaternion::Identity()}, StreamingSource{90.0f, true});
        streamer = std::make_unique<streaming::WorldStreamer>(
            streamed_world, partition.index, [this](const streaming::CellCoord& c, std::vector<u8>& bytes) {
                const auto it = partition.cells.find(c);
                if (it == partition.cells.end()) return false;
                bytes = it->second;
                return true;
            });
        streamer->Update();
        partition_panel = std::make_unique<WorldPartitionPanel>(*streamer, &streamed_world);
    }

    // Three crates; Net Play's Start runs them on a server with clients.
    void BuildNetworking() {
        for (int i = 0; i < 3; ++i) {
            ModelRenderer model;
            SetModelPath(model, "models/crate.gltf");
            net_level.CreateEntity(Transform{Vec3(static_cast<f32>(i) * 3.0f, 0, 5), Quaternion{}}, model);
        }
        net_session = std::make_unique<NetPlaySession>();
        net_panel = std::make_unique<NetPlayPanel>(*net_session, net_level);
        net_panel->settings.clients = 2;
        net_profiler = std::make_unique<NetProfilerPanel>(*net_session);
    }

    void BuildDebug() {
        console.CaptureLog(true);
        console_panel = std::make_unique<ConsolePanel>(console);
        profiler_panel = std::make_unique<ProfilerPanel>();
        crash_dialog = std::make_unique<CrashReporterDialog>(CrashConfig{}.directory);
    }

    // A project made from a template becomes the one the Project tools work on.
    void BuildProject() {
        new_project.on_created = [this](const std::filesystem::path& file) { OpenProject(file); };
        std::string error;
        if (project_file.empty()) project_file = EditorWorkspace::SampleProject(&error);
        if (project_file.empty()) return;
        LoadProjectTools();
    }

    // Points the Project tools (and the plugins' modules) at `file`.
    void OpenProject(const std::filesystem::path& file) {
        // The running build, if any, is cancelled and joined by its window's destructor.
        build_window.reset();
        project_file = file;
        LoadProjectTools();
    }

    void LoadProjectTools() {
        std::string error;
        project_settings = std::make_unique<ProjectSettingsPanel>(project_file);
        build_window = std::make_unique<BuildPackageWindow>(project_file);
        plugins_panel = std::make_unique<PluginsPanel>(project_file);
        // The project's plugins: their runtime and editor modules run in the editor.
        if (plugin::ResolveProjectPlugins(project_file, plugins, &error)) plugins.StartModules(true, true);
    }

    void DrawProjectTool(bool settings) {
        if (!project_settings) {
            ImGui::TextDisabled("No project is open.");
            return;
        }
        if (settings) {
            project_settings->Draw();
        } else {
            build_window->Draw();
        }
    }

    void DrawCrashReports() {
        ImGui::TextWrapped("Crash reports from earlier runs are kept in \"%s\".", CrashConfig{}.directory.c_str());
        if (ImGui::Button("Check for crash reports")) {
            crash_dialog->Refresh();
            crash_dialog->open = !crash_dialog->Reports().empty();
        }
        ImGui::SameLine();
        if (crash_dialog->Reports().empty()) {
            ImGui::TextDisabled("No unseen reports.");
        } else {
            ImGui::Text("%zu report(s)", crash_dialog->Reports().size());
        }
    }
};

EditorWorkspace::EditorWorkspace(std::filesystem::path project_file)
    : impl_(std::make_unique<Impl>(std::move(project_file))) {}

const std::filesystem::path& EditorWorkspace::ProjectFile() const { return impl_->project_file; }

bool EditorWorkspace::OpenProject(const std::filesystem::path& project_file) {
    ProjectSettings settings;
    if (!LoadProject(project_file, settings, nullptr)) return false;
    impl_->OpenProject(project_file);
    return true;
}

std::filesystem::path EditorWorkspace::SampleProject(std::string* error) {
    namespace stdfs = std::filesystem;
    std::error_code ec;
    const stdfs::path parent = stdfs::temp_directory_path(ec) / "aether_editor";
    const ProjectPaths paths = ProjectPaths::ForFile(parent / "SampleGame" / "SampleGame.aproject");
    if (stdfs::is_regular_file(paths.file, ec)) return paths.file;
    stdfs::create_directories(parent, ec);
    stdfs::remove_all(paths.root, ec); // a half-made one from an earlier run
    std::string why;
    if (!CreateProject(parent, "SampleGame", nullptr, &why)) {
        if (error) *error = why;
        return {};
    }
    // A startup scene: a tagged player, a camera and a few crates.
    nlohmann::json entities = nlohmann::json::array();
    const auto add = [&](Vec3 at, const char* tag, bool camera) {
        nlohmann::json components = {{"Transform", reflect::ToJson(Transform{at, Quaternion::Identity()})}};
        Tags tags;
        tags.names = {tag};
        components["Tags"] = reflect::ToJson(tags);
        if (camera) components["Camera"] = reflect::ToJson(Camera{});
        entities.push_back({{"components", components}});
    };
    add(Vec3(0, 1, 0), "Player", false);
    add(Vec3(0, 3, 8), "MainCamera", true);
    for (int i = 0; i < 3; ++i) add(Vec3(static_cast<f32>(i) * 2.0f - 2.0f, 0.5f, -4.0f), "Crate", false);
    const nlohmann::json scene = {{"$type", "Scene"}, {"$version", 1}, {"entities", entities}};
    stdfs::create_directories(paths.content / "Scenes", ec);
    std::ofstream(paths.content / "Scenes" / "Main.ascene", std::ios::binary) << scene.dump(2);
    ProjectSettings settings;
    if (!LoadProject(paths.file, settings, &why)) {
        if (error) *error = why;
        return {};
    }
    settings.startup_scene = "Scenes/Main.ascene";
    settings.window_title = "Sample Game";
    if (!SaveProject(paths.file, settings, &why)) {
        if (error) *error = why;
        return {};
    }
    return paths.file;
}
EditorWorkspace::~EditorWorkspace() = default;

void EditorWorkspace::Update(f32 dt) {
    if (impl_->net_session->Running()) impl_->net_session->Tick(static_cast<f64>(dt));
    impl_->console.Pump();
}

usize EditorWorkspace::ToolCount() const { return impl_->tools.size(); }
const char* EditorWorkspace::ToolName(usize tool) const { return impl_->tools[tool].name; }
const char* EditorWorkspace::ToolCategory(usize tool) const { return impl_->tools[tool].category; }

i64 EditorWorkspace::FindTool(std::string_view name) const {
    if (name.empty()) return -1;
    for (usize i = 0; i < impl_->tools.size(); ++i) {
        if (StartsWithNoCase(impl_->tools[i].name, name)) return static_cast<i64>(i);
    }
    return -1;
}

usize EditorWorkspace::Selected() const { return impl_->selected; }
void EditorWorkspace::Select(usize tool) {
    if (tool < impl_->tools.size()) impl_->selected = tool;
}
bool EditorWorkspace::IsWindowOpen(usize tool) const { return impl_->tools[tool].window_open; }
void EditorWorkspace::SetWindowOpen(usize tool, bool open) {
    if (tool < impl_->tools.size()) impl_->tools[tool].window_open = open;
}

void EditorWorkspace::DrawTool(usize tool) {
    ImGui::PushID(static_cast<int>(tool));
    impl_->tools[tool].draw();
    ImGui::PopID();
}

void EditorWorkspace::DrawToolsMenu() {
    if (!ImGui::BeginMenu("Tools")) return;
    const char* category = nullptr;
    bool category_open = false;
    for (usize i = 0; i < impl_->tools.size(); ++i) {
        Impl::Tool& t = impl_->tools[i];
        if (!category || std::strcmp(category, t.category) != 0) {
            if (category_open) ImGui::EndMenu();
            category = t.category;
            category_open = ImGui::BeginMenu(category);
        }
        if (category_open && ImGui::MenuItem(t.name, nullptr, t.window_open)) {
            t.window_open = !t.window_open;
            if (t.window_open) ImGui::SetWindowFocus(t.name);
        }
    }
    if (category_open) ImGui::EndMenu();
    ImGui::EndMenu();
}

void EditorWorkspace::DrawHubContents() {
    const f32 list_w = 230.0f;
    if (ImGui::BeginChild("##tool_list", ImVec2(list_w, 0), ImGuiChildFlags_Borders)) {
        const char* category = nullptr;
        for (usize i = 0; i < impl_->tools.size(); ++i) {
            const Impl::Tool& t = impl_->tools[i];
            if (!category || std::strcmp(category, t.category) != 0) {
                category = t.category;
                ImGui::SeparatorText(category);
            }
            if (ImGui::Selectable(t.name, impl_->selected == i)) impl_->selected = i;
            if (ImGui::BeginPopupContextItem()) {
                if (ImGui::MenuItem("Open in its own window")) impl_->tools[i].window_open = true;
                ImGui::EndPopup();
            }
        }
    }
    ImGui::EndChild();
    ImGui::SameLine();
    if (ImGui::BeginChild("##tool", ImVec2(0, 0), ImGuiChildFlags_Borders)) {
        const usize sel = impl_->selected;
        if (ImGui::SmallButton("Pop out")) impl_->tools[sel].window_open = true;
        ImGui::SameLine();
        ImGui::TextUnformatted(impl_->tools[sel].name);
        ImGui::SameLine();
        ImGui::TextDisabled("(%s)", impl_->tools[sel].category);
        ImGui::Separator();
        // A popped-out tool draws in its own window, not twice.
        if (impl_->tools[sel].window_open) {
            ImGui::TextDisabled("Open in its own window.");
        } else {
            DrawTool(sel);
        }
    }
    ImGui::EndChild();
}

void EditorWorkspace::DrawHub(const char* title, bool* open) {
    ImGui::SetNextWindowSize(ImVec2(1100, 700), ImGuiCond_FirstUseEver);
    if (ImGui::Begin(title, open)) DrawHubContents();
    ImGui::End();
}

void EditorWorkspace::DrawWindows() {
    int cascade = 0;
    for (usize i = 0; i < impl_->tools.size(); ++i) {
        Impl::Tool& t = impl_->tools[i];
        if (!t.window_open) continue;
        const f32 offset = 30.0f * static_cast<f32>(cascade++ % 8);
        ImGui::SetNextWindowPos(ImVec2(120.0f + offset, 80.0f + offset), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(900, 600), ImGuiCond_FirstUseEver);
        if (ImGui::Begin(t.name, &t.window_open)) DrawTool(i);
        ImGui::End();
    }
    impl_->crash_dialog->Draw();
}

} // namespace aether::editor
