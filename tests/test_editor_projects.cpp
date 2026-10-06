// Editor-side project helpers (editor/src/core/recent_projects.h).

#include "core/recent_projects.h"
#include "test_framework.h"

#include <filesystem>
#include <fstream>

using namespace aether::editor;
namespace stdfs = std::filesystem;

AETHER_TEST(RecentProjects_OrderDedupeLimitAndPersistence) {
    stdfs::path dir = stdfs::temp_directory_path() / "aether_test_recent";
    stdfs::remove_all(dir);
    stdfs::create_directories(dir);

    RecentProjects recent;
    for (int i = 0; i < 12; ++i) {
        recent.Add(dir / ("P" + std::to_string(i) + ".aproject"));
    }
    AETHER_CHECK(recent.Entries().size() == RecentProjects::kMaxEntries);
    AETHER_CHECK(recent.Entries().front() == dir / "P11.aproject"); // most recent first
    AETHER_CHECK(recent.Entries().back() == dir / "P2.aproject");   // oldest two dropped

    recent.Add(dir / "P5.aproject"); // re-opening moves it to the front, no duplicate
    AETHER_CHECK(recent.Entries().front() == dir / "P5.aproject");
    AETHER_CHECK(recent.Entries().size() == RecentProjects::kMaxEntries);

    stdfs::path file = dir / "config" / "recent_projects.json"; // folder created on save
    AETHER_CHECK(recent.Save(file));
    RecentProjects loaded;
    loaded.Load(file);
    AETHER_CHECK(loaded.Entries() == recent.Entries());

    // Only the ones that still exist survive RemoveMissing.
    std::ofstream(dir / "P5.aproject") << "{}";
    std::ofstream(dir / "P9.aproject") << "{}";
    AETHER_CHECK(loaded.RemoveMissing() == RecentProjects::kMaxEntries - 2);
    AETHER_CHECK(loaded.Entries().size() == 2 && loaded.Entries()[0] == dir / "P5.aproject");

    RecentProjects empty;
    empty.Load(dir / "does_not_exist.json");
    AETHER_CHECK(empty.Entries().empty());
    AETHER_CHECK(!RecentProjects::DefaultConfigDir().empty());
    stdfs::remove_all(dir);
}

#include "aether/project/project.h"
#include "ui/reflected_inspector.h"

#include <imgui.h>

AETHER_TEST(ProjectSettings_DrawInTheInspectorHeadless) {
    ImGuiContext* context = ImGui::CreateContext();
    ImGui::GetIO().ConfigMacOSXBehaviors = false; // tests press Ctrl on every platform
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(1024, 768);
    io.DeltaTime = 1.0f / 60.0f;
    unsigned char* pixels = nullptr;
    int w = 0, h = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);

    aether::ProjectSettings settings;
    settings.plugins = {"Aether.Physics", "Aether.Audio"};
    settings.layers = {"Default", "Player"};
    for (int frame = 0; frame < 3; ++frame) {
        ImGui::NewFrame();
        ImGui::Begin("Project Settings");
        // Array fields draw as collapsed "N elements" nodes by default; this
        // checks every project field (strings, numbers, Vec3, arrays) draws
        // cleanly, with ImGui's own assertions active in Debug builds.
        aether::editor::InspectResult result = aether::editor::InspectObject(settings, "settings");
        AETHER_CHECK(!result.Changed());
        ImGui::End();
        ImGui::Render();
    }
    AETHER_CHECK(settings.plugins.size() == 2 && settings.layers.size() == 2);
    ImGui::DestroyContext(context);
}

#include "aether/assets/asset_ref.h"
#include "aether/scene/components.h"

AETHER_TEST(Inspector_AssetRefFieldsUseTheProviderHeadless) {
    ImGuiContext* context = ImGui::CreateContext();
    ImGui::GetIO().ConfigMacOSXBehaviors = false; // tests press Ctrl on every platform
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(1024, 768);
    io.DeltaTime = 1.0f / 60.0f;
    unsigned char* pixels = nullptr;
    int w = 0, h = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);

    const aether::assets::AssetGuid crate = aether::assets::NewAssetGuid();
    int provider_calls = 0;
    std::string asked_for;
    aether::editor::SetAssetListProvider([&](const char* asset_type) {
        ++provider_calls;
        asked_for = asset_type;
        return std::vector<aether::editor::AssetChoice>{{crate, "models/crate.gltf"}};
    });

    aether::ModelRenderer renderer;
    renderer.model.guid = crate;
    for (int frame = 0; frame < 2; ++frame) {
        ImGui::NewFrame();
        ImGui::Begin("Inspector");
        AETHER_CHECK(!aether::editor::InspectObject(renderer, "model").Changed());
        ImGui::End();
        ImGui::Render();
    }
    AETHER_CHECK(provider_calls == 2 && asked_for == "Model"); // asked for Model assets, once per frame
    aether::editor::SetAssetListProvider(nullptr);
    ImGui::DestroyContext(context);
}
