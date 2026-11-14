#include "core/file_dialog.h"
#include "core/notifications.h"
#include "workspace/editor_workspace.h"

#include "aether/project/project.h"

#include "test_framework.h"

#include <imgui.h>

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

// Stage 1 of docs/design/EDITOR_PARITY_AUDIT.md: the project hub. The recent list outlives the editor, a missing
// project can be pruned, and Open Project goes through the same picker the Scene tool does (with the hub naming the
// file where the platform has no picker).

using namespace aether;
using namespace aether::editor;
namespace stdfs = std::filesystem;

#define CHECK(...) AETHER_CHECK((__VA_ARGS__))

namespace {

class HeadlessImGui {
public:
    HeadlessImGui() {
        context_ = ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(1600, 960);
        io.DeltaTime = 1.0f / 60.0f;
        unsigned char* pixels = nullptr;
        int w = 0, h = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
    }
    ~HeadlessImGui() { ImGui::DestroyContext(context_); }

private:
    ImGuiContext* context_ = nullptr;
};

class FakeFileDialog final : public FileDialog {
public:
    bool available = true;
    std::filesystem::path answer;
    FileDialogOptions last;
    int shows = 0;

    bool Available() const override { return available; }
    std::filesystem::path Show(const FileDialogOptions& options) override {
        last = options;
        ++shows;
        return answer;
    }
};

stdfs::path Scratch(const char* name) {
    const stdfs::path dir = stdfs::temp_directory_path() / name;
    std::error_code ignored;
    stdfs::remove_all(dir, ignored);
    stdfs::create_directories(dir);
    return dir;
}

stdfs::path MakeProject(const stdfs::path& parent, const char* name) {
    ProjectPaths paths;
    std::string error;
    if (!CreateProject(parent, name, &paths, &error)) return {};
    return paths.file;
}

} // namespace

// The list is the editor's memory of what was opened: newest first, written when a project opens, and read back by the
// next session from the same file.
AETHER_TEST(ProjectHub_RecentProjectsSurviveARestart) {
    const stdfs::path root = Scratch("aether_project_hub_recents");
    const stdfs::path alpha = MakeProject(root, "Alpha");
    const stdfs::path beta = MakeProject(root, "Beta");
    CHECK(!alpha.empty() && !beta.empty());
    const stdfs::path recents = root / "recent_projects.json";

    {
        EditorWorkspace workspace;
        workspace.SetRecentProjectsFile(recents);
        CHECK(workspace.RecentProjectFiles().empty());
        CHECK(workspace.OpenProject(alpha));
        CHECK(workspace.OpenProject(beta));
        CHECK(workspace.ProjectFile() == beta);
        CHECK(workspace.RecentProjectFiles().size() == 2);
        CHECK(workspace.RecentProjectFiles().front() == beta);
    }
    CHECK(stdfs::is_regular_file(recents));

    EditorWorkspace next;
    next.SetRecentProjectsFile(recents);
    CHECK(next.RecentProjectFiles().size() == 2);
    CHECK(next.RecentProjectFiles().front() == beta);
    CHECK(next.RecentProjectFiles()[1] == alpha);
}

// A project that was deleted from disk is marked missing in the hub; one action drops those rows and saves the result.
AETHER_TEST(ProjectHub_RemoveMissingProjectsDropsGoneFiles) {
    const stdfs::path root = Scratch("aether_project_hub_missing");
    const stdfs::path alpha = MakeProject(root, "Alpha");
    const stdfs::path beta = MakeProject(root, "Beta");
    CHECK(!alpha.empty() && !beta.empty());
    const stdfs::path recents = root / "recent_projects.json";

    EditorWorkspace workspace;
    workspace.SetRecentProjectsFile(recents);
    CHECK(workspace.OpenProject(alpha));
    CHECK(workspace.OpenProject(beta));
    std::error_code ignored;
    stdfs::remove(beta, ignored);

    CHECK(workspace.RemoveMissingProjects() == 1);
    CHECK(workspace.RecentProjectFiles().size() == 1);
    CHECK(workspace.RecentProjectFiles().front() == alpha);

    EditorWorkspace next;
    next.SetRecentProjectsFile(recents);
    CHECK(next.RecentProjectFiles().size() == 1);
    CHECK(next.RecentProjectFiles().front() == alpha);
}

// File > Open Project asks the platform picker for a .aproject, opens what it returns, and remembers it.
AETHER_TEST(ProjectHub_OpenProjectCommandUsesThePicker) {
    HeadlessImGui imgui;
    const stdfs::path root = Scratch("aether_project_hub_open");
    const stdfs::path alpha = MakeProject(root, "Alpha");
    CHECK(!alpha.empty());

    EditorWorkspace workspace;
    workspace.SetRecentProjectsFile(root / "recent_projects.json");
    FakeFileDialog picker;
    picker.answer = alpha;
    workspace.SetFileDialog(&picker);

    CHECK(workspace.RunCommand("project.open"));
    CHECK(picker.shows == 1);
    CHECK(!picker.last.save);
    CHECK(picker.last.title == "Open project");
    CHECK(picker.last.default_extension == "aproject");
    CHECK(!picker.last.filters.empty() && picker.last.filters[0].extensions == "aproject");
    CHECK(workspace.ProjectFile() == alpha);
    CHECK(workspace.RecentProjectFiles().front() == alpha);
    CHECK(!workspace.project_hub_open);

    // A cancel leaves the open project and the list alone.
    picker.answer.clear();
    CHECK(workspace.RunCommand("project.open"));
    CHECK(workspace.ProjectFile() == alpha);
    CHECK(workspace.RecentProjectFiles().size() == 1);
}

// Without a platform picker the command opens the hub instead, and Show() is never called.
AETHER_TEST(ProjectHub_WithoutAPickerOpenProjectShowsTheHub) {
    HeadlessImGui imgui;
    const stdfs::path root = Scratch("aether_project_hub_nopicker");
    EditorWorkspace workspace;
    workspace.SetRecentProjectsFile(root / "recent_projects.json");
    FakeFileDialog picker;
    picker.available = false;
    workspace.SetFileDialog(&picker);

    CHECK(!workspace.project_hub_open);
    CHECK(workspace.RunCommand("project.open"));
    CHECK(picker.shows == 0);
    CHECK(workspace.project_hub_open);

    CHECK(workspace.RunCommand("project.hub"));
    CHECK(workspace.project_hub_open);
}

// A file that is not a project is refused out loud and leaves the open project and the recent list as they were.
AETHER_TEST(ProjectHub_ABadProjectIsReportedAndChangesNothing) {
    HeadlessImGui imgui;
    const stdfs::path root = Scratch("aether_project_hub_bad");
    const stdfs::path alpha = MakeProject(root, "Alpha");
    CHECK(!alpha.empty());
    const stdfs::path broken = root / "Broken.aproject";
    std::ofstream(broken) << "not a project";

    EditorWorkspace workspace;
    workspace.SetRecentProjectsFile(root / "recent_projects.json");
    CHECK(workspace.OpenProject(alpha));
    CHECK(!workspace.OpenProject(broken));
    CHECK(workspace.ProjectFile() == alpha);
    CHECK(workspace.RecentProjectFiles().size() == 1);

    const std::vector<const Notification*>& rows = workspace.Notifications().NewestFirst();
    CHECK(!rows.empty());
    if (!rows.empty()) {
        CHECK(rows.front()->severity == NotificationSeverity::Error);
        CHECK(rows.front()->title == "Open project failed");
        CHECK(rows.front()->target.kind == NotificationTarget::Kind::File);
        CHECK(rows.front()->target.value == broken.string());
    }
}

// The hub is drawn like any other window, so a headless host reaches it too; opening it does not lose the open project.
AETHER_TEST(ProjectHub_DrawsHeadlesslyOverTheOpenProject) {
    HeadlessImGui imgui;
    const stdfs::path root = Scratch("aether_project_hub_draw");
    const stdfs::path alpha = MakeProject(root, "Alpha");
    CHECK(!alpha.empty());

    EditorWorkspace workspace;
    workspace.SetRecentProjectsFile(root / "recent_projects.json");
    CHECK(workspace.OpenProject(alpha));
    workspace.OpenProjectHub();
    CHECK(workspace.project_hub_open);

    ImGui::NewFrame();
    workspace.DrawWindows();
    ImGui::Render();

    CHECK(workspace.project_hub_open);
    CHECK(workspace.ProjectFile() == alpha);
}
