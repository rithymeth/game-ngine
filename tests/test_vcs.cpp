#include "test_framework.h"

#include "aether/assets/asset_database.h"
#include "aether/assets/content_browser.h"
#include "aether/assets/vcs.h"
#include "aether/project/project.h"
#include "content/content_browser_panel.h"
#include "workspace/editor_workspace.h"

#include <imgui.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>

// Version control status in the Content Browser (Phase 26 step 5, §26.7).

using namespace aether;
using namespace aether::assets;
using namespace aether::editor;
namespace stdfs = std::filesystem;

#define CHECK(...) AETHER_CHECK((__VA_ARGS__))

namespace {

stdfs::path Dir(const std::string& name) {
    const stdfs::path dir = stdfs::temp_directory_path() / "aether_vcs_tests" / name;
    std::error_code ec;
    stdfs::remove_all(dir, ec);
    stdfs::create_directories(dir, ec);
    return dir;
}

void Write(const stdfs::path& file, const std::string& text) {
    std::error_code ec;
    stdfs::create_directories(file.parent_path(), ec);
    std::ofstream(file, std::ios::binary) << text;
}

std::string Nul(std::initializer_list<const char*> parts) {
    std::string out;
    for (const char* p : parts) {
        out += p;
        out.push_back('\0');
    }
    return out;
}

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

} // namespace

AETHER_TEST(Vcs_ParsesGitStatusOutput) {
    const std::string out = Nul({" M a.txt", "A  b.txt", "?? c.txt", "UU d.txt", " D e.txt", "R  new.txt", "old.txt", "!! ignored.txt",
                                 "MM f.txt", "AM g.txt", "AA h.txt", "x", " T t.txt"});
    const VcsStatus s = ParseGitStatus(out);
    CHECK(s.available);
    CHECK(s.Of("a.txt") == VcsState::Modified && s.Of("b.txt") == VcsState::Added && s.Of("c.txt") == VcsState::Untracked);
    CHECK(s.Of("d.txt") == VcsState::Conflicted && s.Of("h.txt") == VcsState::Conflicted && s.Of("e.txt") == VcsState::Deleted);
    CHECK(s.Of("new.txt") == VcsState::Renamed && s.Of("old.txt") == VcsState::Deleted); // the old name is gone
    CHECK(s.Of("ignored.txt") == VcsState::Clean && s.files.count("ignored.txt") == 0);
    CHECK(s.Of("f.txt") == VcsState::Modified && s.Of("g.txt") == VcsState::Added && s.Of("t.txt") == VcsState::Modified);
    CHECK(s.Of("never.txt") == VcsState::Clean);
    CHECK(s.ChangedCount() == 11);
    CHECK(ParseGitStatus("").files.empty() && ParseGitStatus("").available);
    CHECK(ParseGitStatus(Nul({"short", "?"})).files.empty()); // malformed entries are skipped
}

AETHER_TEST(Vcs_FoldersAndLocalPathsAndNames) {
    VcsStatus s;
    s.available = true;
    s.prefix = "Game/Content/";
    s.files["Game/Content/Scripts/a.luau"] = VcsState::Added;
    s.files["Game/Content/Scripts/Deep/b.luau"] = VcsState::Modified;
    s.files["Game/Content/Scenes/main.ascene"] = VcsState::Untracked;
    s.files["Game/Other/x"] = VcsState::Conflicted; // not under Content
    CHECK(s.OfLocal("Scripts/a.luau") == VcsState::Added && s.OfLocal("Scripts/none.luau") == VcsState::Clean);
    CHECK(s.OfLocalFolder("Scripts") == VcsState::Modified); // the worst child
    CHECK(s.OfLocalFolder("Scripts/Deep") == VcsState::Modified && s.OfLocalFolder("Scenes") == VcsState::Untracked);
    CHECK(s.OfLocalFolder("Nope") == VcsState::Clean && s.OfLocalFolder("Scri") == VcsState::Clean); // whole folder names only
    CHECK(s.OfFolder("Game") == VcsState::Conflicted && s.OfFolder("") == VcsState::Conflicted);
    CHECK(WorseVcsState(VcsState::Added, VcsState::Modified) == VcsState::Modified);
    CHECK(WorseVcsState(VcsState::Clean, VcsState::Untracked) == VcsState::Untracked);
    CHECK(WorseVcsState(VcsState::Deleted, VcsState::Conflicted) == VcsState::Conflicted);
    for (VcsState st : {VcsState::Untracked, VcsState::Renamed, VcsState::Added, VcsState::Modified, VcsState::Deleted, VcsState::Conflicted}) {
        CHECK(std::string(VcsStateName(st)) != "?" && VcsStateLetter(st) != ' ');
    }
    CHECK(VcsStateLetter(VcsState::Clean) == ' ' && VcsStateLetter(VcsState::Modified) == 'M' && VcsStateLetter(VcsState::Conflicted) == '!');
}

AETHER_TEST(Vcs_ContentBrowserEntriesGetBadges) {
    const stdfs::path content = Dir("content") / "Content";
    Write(content / "Scripts/a.luau", "print(1)\n");
    Write(content / "Scripts/Deep/b.luau", "print(2)\n");
    Write(content / "Scenes/main.ascene", "{\"$type\":\"Scene\",\"$version\":1,\"entities\":[]}\n");
    Write(content / "Readme.luau", "print(3)\n");
    AssetDatabase db(content);
    db.Scan();

    VcsStatus s;
    s.available = true;
    s.prefix = "Game/Content/";
    s.files["Game/Content/Scripts/a.luau.ameta"] = VcsState::Untracked; // only the sidecar changed
    s.files["Game/Content/Scripts/Deep/b.luau"] = VcsState::Modified;
    s.files["Game/Content/Scenes/main.ascene"] = VcsState::Added;
    s.files["Game/Content/Scenes/main.ascene.ameta"] = VcsState::Added;

    ContentQuery query;
    std::vector<ContentEntry> root = ListContent(db, query);
    const usize changed = ApplyVcsStatus(root, s);
    const auto find = [&](const std::vector<ContentEntry>& list, const std::string& name) -> const ContentEntry* {
        for (const ContentEntry& e : list) {
            if (e.name == name) return &e;
        }
        return nullptr;
    };
    CHECK(find(root, "Scripts") && find(root, "Scripts")->is_folder && find(root, "Scripts")->vcs == VcsState::Modified); // the worst below
    CHECK(find(root, "Scenes") && find(root, "Scenes")->vcs == VcsState::Added);
    CHECK(find(root, "Readme.luau") && find(root, "Readme.luau")->vcs == VcsState::Clean);
    CHECK(changed == 2);

    query.folder = "Scripts";
    std::vector<ContentEntry> scripts = ListContent(db, query);
    ApplyVcsStatus(scripts, s);
    CHECK(find(scripts, "a.luau") && find(scripts, "a.luau")->vcs == VcsState::Untracked); // from its .ameta
    CHECK(find(scripts, "Deep") && find(scripts, "Deep")->vcs == VcsState::Modified);

    query.folder = "Scripts/Deep";
    std::vector<ContentEntry> deep = ListContent(db, query);
    ApplyVcsStatus(deep, s);
    CHECK(deep.size() == 1 && deep[0].vcs == VcsState::Modified);

    // No repository: everything stays clean.
    VcsStatus none;
    none.error = "Not in a git repository";
    ApplyVcsStatus(root, none);
    for (const ContentEntry& e : root) CHECK(e.vcs == VcsState::Clean);
}

AETHER_TEST(Vcs_AskingGitInARealRepository) {
    const stdfs::path nowhere = Dir("norepo");
    const VcsStatus none = QueryGitStatus(nowhere);
    // A folder in the temp directory isn't in a repository (unless the temp directory is inside one).
    if (none.available) std::printf("    (the temp folder is inside a git repository; skipping the not-a-repository check)\n");
    else CHECK(!none.error.empty() && none.files.empty());
    CHECK(!QueryGitStatus(nowhere / "quote\"name").available);

    const stdfs::path repo = Dir("repo");
    const std::string q = "\"" + repo.string() + "\"";
    if (std::system(("git init -q " + q).c_str()) != 0) {
        std::printf("    (git isn't available here; skipping the repository check)\n");
        return;
    }
    const std::string git = "git -C " + q + " -c user.name=T -c user.email=t@example.com ";
    Write(repo / "Project/Content/a.txt", "one\n");
    Write(repo / "Project/Content/Deep/b.txt", "two\n");
    Write(repo / "Project/Content/gone.txt", "three\n");
    Write(repo / "other.txt", "x\n");
    CHECK(std::system((git + "add -A >/dev/null 2>&1").c_str()) == 0 || std::system((git + "add -A").c_str()) == 0);
    CHECK(std::system((git + "commit -q -m first").c_str()) == 0);
    Write(repo / "Project/Content/a.txt", "changed\n");                         // modified
    Write(repo / "Project/Content/new.txt", "new\n");                           // untracked
    std::error_code ec;
    stdfs::remove(repo / "Project/Content/gone.txt", ec);                       // deleted
    Write(repo / "other.txt", "changed elsewhere\n");                           // outside the queried folder

    const VcsStatus s = QueryGitStatus(repo / "Project/Content");
    CHECK(s.available && s.prefix == "Project/Content/");
    CHECK(s.OfLocal("a.txt") == VcsState::Modified && s.OfLocal("new.txt") == VcsState::Untracked && s.OfLocal("gone.txt") == VcsState::Deleted);
    CHECK(s.OfLocal("Deep/b.txt") == VcsState::Clean);
    CHECK(s.Of("other.txt") == VcsState::Clean); // the query is for the content folder only
    CHECK(s.OfLocalFolder("Deep") == VcsState::Clean && s.ChangedCount() == 3);
    const VcsStatus at_root = QueryGitStatus(repo);
    CHECK(at_root.available && at_root.prefix.empty() && at_root.Of("other.txt") == VcsState::Modified && at_root.ChangedCount() == 4);
}

AETHER_TEST(Vcs_ContentBrowserPanelListsFiltersAndDraws) {
    const stdfs::path dir = Dir("panel");
    ProjectPaths paths;
    std::string error;
    CHECK(CreateProject(dir, "Game", &paths, &error));
    Write(paths.content / "Scripts/a.luau", "print(1)\n");
    Write(paths.content / "Scripts/Deep/b.luau", "print(2)\n");
    Write(paths.content / "Notes.luau", "print(3)\n");

    ContentBrowserPanel panel(paths.file);
    CHECK(panel.Error().empty());
    CHECK(!panel.Vcs().available); // a temp folder: no repository
    CHECK(panel.Rows().size() >= 2 && panel.ChangedRows() == 0);

    VcsStatus s;
    s.available = true;
    s.prefix = "Game/Content/";
    s.files["Game/Content/Scripts/Deep/b.luau"] = VcsState::Modified;
    s.files["Game/Content/Notes.luau.ameta"] = VcsState::Untracked;
    panel.SetVcsStatus(s);
    CHECK(panel.ChangedRows() == 2); // Scripts (worst child) and Notes
    panel.changed_only = true;
    panel.OpenFolder("");
    CHECK(panel.Rows().size() == 2);
    for (const assets::ContentEntry& e : panel.Rows()) CHECK(e.vcs != VcsState::Clean);
    panel.OpenFolder("Scripts");
    CHECK(panel.Rows().size() == 1 && panel.Rows()[0].name == "Deep"); // a.luau is clean and hidden
    panel.OpenFolder("Scripts/Deep");
    CHECK(panel.Folder() == "Scripts/Deep" && panel.Rows().size() == 1 && panel.Rows()[0].vcs == VcsState::Modified);
    panel.OpenParent();
    CHECK(panel.Folder() == "Scripts");
    panel.OpenParent();
    CHECK(panel.Folder().empty());
    panel.changed_only = false;
    panel.search = "b.luau";
    panel.OpenFolder("");
    CHECK(panel.Rows().size() == 1 && panel.Rows()[0].vcs == VcsState::Modified); // a search finds it in a subfolder

    HeadlessImGui imgui;
    for (int frame = 0; frame < 3; ++frame) imgui.Frame([&] { panel.Draw(); });
    CHECK(ImGui::GetDrawData()->TotalVtxCount > 0);
    CHECK(panel.Refresh() && panel.Vcs().available); // the injected status stays until a host asks git itself
    ContentBrowserPanel missing(dir / "Nope.aproject");
    CHECK(!missing.Error().empty() && missing.Rows().empty());
    imgui.Frame([&] { missing.Draw(); });
}

AETHER_TEST(Vcs_WorkspaceHasAContentBrowser) {
    HeadlessImGui imgui;
    EditorWorkspace ws;
    const i64 tool = ws.FindTool("Content Browser");
    CHECK(tool >= 0 && std::string(ws.ToolCategory(static_cast<usize>(tool))) == "Project");
    ws.Select(static_cast<usize>(tool));
    for (int frame = 0; frame < 2; ++frame) imgui.Frame([&] { ws.DrawHubContents(); });
    CHECK(ImGui::GetDrawData()->TotalVtxCount > 0);
}
