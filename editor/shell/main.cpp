// The portable editor shell (Phase 24 step 5, docs/design/PHASE_SPECS.md
// §24.4): the editor's portable panels - the Blueprint, Material,
// Animation and Behavior Tree editors, the terrain tools, the console and
// the profiler - in a window (GLFW on Linux and macOS, Win32 on Windows)
// or offscreen, drawn with Dear ImGui through the RHI's Vulkan backend.
// The D3D12 editor (editor/main.cpp) stays the Windows editor with the 3D
// viewport; this shell is what runs everywhere Vulkan does, lavapipe in CI
// included.
//
// Environment:
//   AETHER_EDITOR_HEADLESS=1     render offscreen (also when there's no display)
//   AETHER_EDITOR_MAX_FRAMES=N   quit after N frames
//   AETHER_EDITOR_SCREENSHOT=p   save the last frame as a PNG at p
//   AETHER_EDITOR_SIZE=WxH       the window or offscreen size (default 1600x960)
//   AETHER_EDITOR_TAB=name       the editor tab to open on (blueprint, material,
//                                animation, behavior)

#include "ai/bt_editor.h"
#include "anim/anim_graph_editor.h"
#include "anim/anim_viewer.h"
#include "anim/blend_space_editor.h"
#include "devtools/console_panel.h"
#include "devtools/profiler_panel.h"
#include "graph/blueprint_editor.h"
#include "graph/material_editor.h"
#include "host/imgui_vulkan_host.h"
#include "host/imgui_window_input.h"
#include "world/world_panels.h"

#include "aether/blueprint/nodes.h"
#include "aether/core/console.h"
#include "aether/core/log.h"
#include "aether/core/profiler.h"
#include "aether/gfx/rhi/device.h"
#include "aether/platform/window.h"

#include <imgui.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <string>
#include <vector>

using namespace aether;
using namespace aether::editor;
using nlohmann::json;
namespace rhi = aether::gfx::rhi;

namespace {

// Sample documents for every panel, so the shell opens on something real.
struct SampleProject {
    // Blueprint: BeginPlay -> Branch(IsOpen) -> false -> Print, and a Tick that sets IsOpen.
    std::unique_ptr<BlueprintDocument> blueprint_doc;
    std::unique_ptr<BlueprintEditor> blueprint_editor;
    // Material: Albedo * Tint -> BaseColor, with roughness and metallic.
    std::unique_ptr<MaterialDocument> material_doc;
    std::unique_ptr<MaterialEditor> material_editor;
    // Animation: Slot <- a locomotion state machine; a 2D blend space; a clip.
    std::unique_ptr<AnimGraphDocument> anim_doc;
    std::unique_ptr<AnimGraphEditor> anim_editor;
    std::unique_ptr<BlendSpaceDocument> blend_doc;
    std::unique_ptr<BlendSpaceEditor> blend_editor;
    anim::Skeleton skeleton;
    anim::AnimationClip clip;
    std::unique_ptr<ClipViewer> clip_viewer;
    // Behavior Tree: Selector [ Sequence(if Enemy is set) [MoveTo Enemy, Log], Wait ].
    std::unique_ptr<BehaviorTreeDocument> bt_doc;
    std::unique_ptr<BehaviorTreeEditor> bt_editor;
    // Terrain.
    terrain::TerrainData terrain_data;
    terrain::TerrainSettings terrain_settings;
    std::unique_ptr<TerrainEditDocument> terrain_doc;
    std::unique_ptr<TerrainToolPanel> terrain_panel;

    SampleProject() {
        BuildBlueprint();
        BuildMaterial();
        BuildAnimation();
        BuildBehaviorTree();
        BuildTerrain();
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

    void BuildTerrain() {
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
    }
};

void ApplyEditorStyle() {
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 4.0f;
    style.FrameRounding = 3.0f;
    style.TabRounding = 3.0f;
    style.WindowPadding = ImVec2(8, 8);
    style.ItemSpacing = ImVec2(8, 5);
    ImVec4* colors = style.Colors;
    colors[ImGuiCol_TitleBgActive] = ImVec4(0.16f, 0.29f, 0.48f, 1.0f);
    colors[ImGuiCol_Tab] = ImVec4(0.18f, 0.22f, 0.28f, 1.0f);
    colors[ImGuiCol_TabSelected] = ImVec4(0.26f, 0.42f, 0.66f, 1.0f);
}

const char* const kEditorTabs[] = {"Blueprint - BP_Door", "Material - M_Lit", "Animation - ABP_Character",
                                   "Behavior Tree - BT_Guard"};

// The tab AETHER_EDITOR_TAB asks for (matched against the start of the tab
// names, case-insensitively), selected on the first frame only; -1 for none.
int g_open_tab = -1;

int TabFromName(const char* name) {
    if (!name || !*name) return -1;
    for (int i = 0; i < 4; ++i) {
        const char* tab = kEditorTabs[i];
        usize k = 0;
        while (name[k] && tab[k] && std::tolower(static_cast<unsigned char>(name[k])) ==
                                        std::tolower(static_cast<unsigned char>(tab[k]))) {
            ++k;
        }
        if (!name[k]) return i;
    }
    return -1;
}

ImGuiTabItemFlags TabFlags(int index) {
    return g_open_tab == index ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
}

void DrawEditor(SampleProject& project, ConsolePanel& console_panel, ProfilerPanel& profiler_panel,
                const char* backend_label, bool& quit) {
    const ImGuiIO& io = ImGui::GetIO();
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Quit")) quit = true;
            ImGui::EndMenu();
        }
        for (const char* menu : {"Edit", "View", "Window", "Help"}) {
            if (ImGui::BeginMenu(menu)) ImGui::EndMenu();
        }
        ImGui::TextDisabled("   Aether Editor  -  %s", backend_label);
        ImGui::EndMainMenuBar();
    }

    const f32 top = ImGui::GetFrameHeight();
    const f32 side = io.DisplaySize.x > 1100 ? 420.0f : io.DisplaySize.x * 0.35f;
    const f32 main_w = io.DisplaySize.x - side;
    const f32 main_h = io.DisplaySize.y - top;
    constexpr ImGuiWindowFlags kFixed = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                                        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus;

    ImGui::SetNextWindowPos(ImVec2(0, top));
    ImGui::SetNextWindowSize(ImVec2(main_w, main_h));
    ImGui::Begin("Editors", nullptr, kFixed | ImGuiWindowFlags_NoTitleBar);
    if (ImGui::BeginTabBar("##editors")) {
        if (ImGui::BeginTabItem(kEditorTabs[0], nullptr, TabFlags(0))) {
            project.blueprint_editor->Draw();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(kEditorTabs[1], nullptr, TabFlags(1))) {
            project.material_editor->Draw();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(kEditorTabs[2], nullptr, TabFlags(2))) {
            const ImVec2 avail = ImGui::GetContentRegionAvail();
            if (ImGui::BeginChild("##graph", ImVec2(avail.x, avail.y * 0.6f), ImGuiChildFlags_Borders)) {
                project.anim_editor->Draw();
            }
            ImGui::EndChild();
            if (ImGui::BeginChild("##blend", ImVec2(avail.x * 0.55f, 0), ImGuiChildFlags_Borders)) {
                project.blend_editor->Draw();
            }
            ImGui::EndChild();
            ImGui::SameLine();
            if (ImGui::BeginChild("##clip", ImVec2(0, 0), ImGuiChildFlags_Borders)) {
                project.clip_viewer->Draw();
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(kEditorTabs[3], nullptr, TabFlags(3))) {
            project.bt_editor->Draw();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
    g_open_tab = -1;

    ImGui::SetNextWindowPos(ImVec2(main_w, top));
    ImGui::SetNextWindowSize(ImVec2(side, main_h * 0.5f));
    ImGui::Begin("Terrain", nullptr, kFixed);
    project.terrain_panel->Draw();
    ImGui::End();

    ImGui::SetNextWindowPos(ImVec2(main_w, top + main_h * 0.5f));
    ImGui::SetNextWindowSize(ImVec2(side, main_h * 0.5f));
    ImGui::Begin("Tools", nullptr, kFixed);
    if (ImGui::BeginTabBar("##tools")) {
        if (ImGui::BeginTabItem("Console")) {
            console_panel.Draw();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Profiler")) {
            profiler_panel.Draw();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

bool HasDisplay() {
#if defined(_WIN32)
    return true;
#else
    return std::getenv("DISPLAY") != nullptr || std::getenv("WAYLAND_DISPLAY") != nullptr;
#endif
}

} // namespace

int main() {
    i32 max_frames = -1;
    if (const char* env = std::getenv("AETHER_EDITOR_MAX_FRAMES")) max_frames = std::atoi(env);
    const char* screenshot_path = std::getenv("AETHER_EDITOR_SCREENSHOT");
    bool headless = std::getenv("AETHER_EDITOR_HEADLESS") != nullptr || !HasDisplay();
    u32 width = 1600, height = 960;
    if (const char* env = std::getenv("AETHER_EDITOR_SIZE")) {
        unsigned w = 0, h = 0;
        if (std::sscanf(env, "%ux%u", &w, &h) == 2 && w >= 320 && h >= 240) width = w, height = h;
    }

    g_open_tab = TabFromName(std::getenv("AETHER_EDITOR_TAB"));

    try {
        std::unique_ptr<Window> window;
        void* native_window = nullptr;
        if (!headless) {
            WindowDesc desc;
            desc.title = "Aether Editor";
            desc.width = width, desc.height = height;
            window = std::make_unique<Window>(desc);
            if (window->IsValid()) {
#if defined(_WIN32)
                native_window = window->NativeHandle();
#else
                native_window = window->PlatformWindow();
#endif
            } else {
                AETHER_LOG_WARN("Editor", "No window could be opened; rendering offscreen");
                window.reset();
                headless = true;
            }
        }

        std::unique_ptr<rhi::IDevice> device = rhi::CreateDevice(rhi::Backend::Vulkan, false);
        if (!device) {
            std::fprintf(stderr, "aether_editor_shell: no Vulkan device\n");
            return 1;
        }
        std::unique_ptr<rhi::ISwapChain> swap_chain = device->CreateSwapChain(native_window, width, height, 2);
        bool resized = false;
        if (window) {
            window->on_resize = [&](u32, u32) { resized = true; };
        }

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        // Shortcuts use Ctrl everywhere: the engine's key set has no Cmd key
        // yet, so ImGui's macOS Ctrl/Cmd swap would leave none reachable.
        io.ConfigMacOSXBehaviors = false;
        ApplyEditorStyle();
        ImGuiVulkanHost host(*device, *swap_chain);

        Console console;
        console.CaptureLog(true);
        ConsolePanel console_panel(console);
        ProfilerPanel profiler_panel;
        SampleProject project;
        AETHER_LOG_INFO("Editor", "Aether editor shell: Vulkan, %s, %ux%u", headless ? "offscreen" : "window",
                        swap_chain->Width(), swap_chain->Height());
        const char* backend_label = headless ? "Vulkan (offscreen)" : "Vulkan";

        std::unique_ptr<rhi::ICommandList> cmd = device->CreateCommandList();
        u64 fence = 0;
        auto last = std::chrono::steady_clock::now();
        bool quit = false;
        for (i32 frame = 0; !quit && (max_frames < 0 || frame < max_frames); ++frame) {
            if (window) {
                if (!window->PumpMessages()) break;
                const std::vector<WindowEvent> events = window->TakeEvents();
                FeedImGuiEvents(events, io);
                if (window->IsMinimized()) continue;
                if (resized) {
                    device->WaitForFence(fence);
                    swap_chain->Resize(window->Width(), window->Height());
                    resized = false;
                }
            }
            const auto now = std::chrono::steady_clock::now();
            io.DeltaTime = std::max(std::chrono::duration<f32>(now - last).count(), 1.0f / 1000.0f);
            last = now;
            io.DisplaySize = ImVec2(static_cast<f32>(swap_chain->Width()), static_cast<f32>(swap_chain->Height()));

            Profiler::Get().BeginFrame();
            host.NewFrame();
            ImGui::NewFrame();
            console.Pump();
            DrawEditor(project, console_panel, profiler_panel, backend_label, quit);
            ImGui::Render();

            device->WaitForFence(fence); // one command list: the last frame must be done with it
            swap_chain->AcquireNextImage();
            cmd->Reset();
            cmd->BeginRenderPass(*swap_chain, {0.06f, 0.07f, 0.09f, 1.0f});
            host.Render(*cmd, ImGui::GetDrawData());
            cmd->EndRenderPass();
            cmd->Close();
            fence = device->Submit(*cmd, swap_chain.get());

            const bool last_frame = quit || (max_frames >= 0 && frame + 1 >= max_frames);
            if (last_frame && screenshot_path) {
                device->WaitForFence(fence);
                std::vector<u8> pixels;
                if (swap_chain->ReadBack(pixels)) {
                    const int w = static_cast<int>(swap_chain->Width()), h = static_cast<int>(swap_chain->Height());
                    if (stbi_write_png(screenshot_path, w, h, 4, pixels.data(), w * 4)) {
                        AETHER_LOG_INFO("Editor", "Saved a screenshot to %s", screenshot_path);
                    } else {
                        AETHER_LOG_ERROR("Editor", "Couldn't write %s", screenshot_path);
                    }
                } else {
                    AETHER_LOG_WARN("Editor", "This swap chain can't be read back; no screenshot");
                }
            }
            swap_chain->Present(true);
            Profiler::Get().EndFrame();
        }
        device->WaitForFence(fence);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "aether_editor_shell: %s\n", e.what());
        return 1;
    }
    if (ImGui::GetCurrentContext()) ImGui::DestroyContext();
    return 0;
}
