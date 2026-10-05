#include "test_framework.h"
#include "loc/localization_document.h"
#include "loc/localization_panel.h"
#include "ui/extensions.h"
#include "workspace/editor_workspace.h"

#include "aether/loc/localize.h"
#include "aether/loc/pseudo.h"

#include <imgui.h>

#include <filesystem>
#include <fstream>
#include <functional>

// The Localization dashboard (Phase 29 step 5, §29.5).

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
        io.ConfigMacOSXBehaviors = false;
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(1400, 900);
        io.DeltaTime = 1.0f / 60.0f;
        unsigned char* pixels = nullptr;
        int w = 0, h = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
    }
    ~HeadlessImGui() { ImGui::DestroyContext(context_); }
    void Frame(const std::function<void()>& body) {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(1300, 800));
        ImGui::Begin("Test");
        body();
        ImGui::End();
        ImGui::Render();
    }

private:
    ImGuiContext* context_ = nullptr;
};

stdfs::path Dir(const std::string& name) {
    const stdfs::path dir = stdfs::temp_directory_path() / "aether_loc_editor_tests" / name;
    stdfs::remove_all(dir);
    stdfs::create_directories(dir);
    return dir;
}

void Write(const stdfs::path& file, const std::string& text) {
    stdfs::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << text;
}

std::string Slurp(const stdfs::path& file) {
    std::ifstream in(file, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

stdfs::path MakeTable(const stdfs::path& dir) {
    const stdfs::path file = dir / "Content/Localization/strings.astrings";
    Write(file, "key,en,fr,de\nplay,Play,Jouer,Spielen\nquit,Quit,Quitter,\ncoins,{count} coin{count|s},,\n");
    return file;
}

} // namespace

AETHER_TEST(LocPseudo_AccentsExpandsAndKeepsPlaceholders) {
    const std::string p = loc::Pseudo("Play {name}!\nNow");
    CHECK(p.front() == '[' && p.back() == ']');
    CHECK(p.find("{name}") != std::string::npos && p.find('\n') != std::string::npos);
    CHECK(p.find("Play") == std::string::npos && p.find("\xC3\xA1") != std::string::npos); // "Pl\u00e1y"
    CHECK(loc::Pseudo("Play {name}!\nNow") == p); // deterministic
    // About a third longer (counting only the visible text).
    const std::string plain = "abcdefghijklmnopqrst"; // 20 characters
    const std::string longer = loc::Pseudo(plain, {0.35f, false, false});
    CHECK(longer.size() == 27); // 20 + ceil(20 * 0.35) = 27
    CHECK(loc::Pseudo("x", {0.0f, false, false}) == "x");
    CHECK(loc::Pseudo("{{a}}", {0.0f, false, false}) == "{{a}}");
    CHECK(loc::Pseudo("{count|one:file;other:files} {open", {0.0f, true, false}).find("{count|one:file;other:files}") != std::string::npos);
    CHECK(loc::Pseudo("").empty() == false); // just the brackets
    // A pseudo string still formats.
    loc::FormatArgs args{{"count", i64{2}}};
    CHECK(loc::Format(loc::Pseudo("{count} coin{count|s}"), "en", args).find("2") != std::string::npos);
    loc::StringTable t;
    t.Set("en", "k", "Hello");
    t.Set("fr", "k", "Bonjour");
    const loc::StringTable pt = loc::PseudoTable(t, "en");
    CHECK(*pt.Find("fr", "k") == "Bonjour" && pt.Find(loc::PseudoLanguage(), "k") != nullptr);
    CHECK(t.Find(loc::PseudoLanguage(), "k") == nullptr); // the original is untouched
}

AETHER_TEST(LocEditor_StatsMissingKeysAndEdits) {
    const stdfs::path file = MakeTable(Dir("edits"));
    LocalizationDocument doc;
    CHECK(doc.Open(file) && doc.IsOpen() && !doc.Dirty() && doc.SourceLanguage() == "en");
    const auto stats = doc.Stats();
    CHECK(stats.size() == 3 && stats[0].language == "de" && stats[0].count == 1 && stats[0].total == 3);
    CHECK(stats[1].language == "en" && stats[1].percent == 100.0f && stats[2].language == "fr" && stats[2].count == 2);
    CHECK((doc.MissingKeys("de") == std::vector<std::string>{"coins", "quit"}));
    CHECK((doc.MissingKeys("fr") == std::vector<std::string>{"coins"}));
    const u64 revision = doc.Revision();
    CHECK(doc.SetCell("de", "quit", "Beenden") && doc.Dirty() && doc.Revision() != revision);
    CHECK(doc.MissingKeys("de").size() == 1);
    CHECK(doc.SetCell("de", "quit", "")); // clearing
    CHECK(doc.MissingKeys("de").size() == 2);
    CHECK(!doc.SetCell("de", "nope", "x") && !doc.SetCell("", "quit", "x"));
    CHECK(doc.AddKey("new.key", "New text") && !doc.AddKey("new.key") && !doc.AddKey(""));
    CHECK(*doc.Table().Find("en", "new.key") == "New text" && doc.Stats()[0].total == 4);
    CHECK(doc.AddLanguage("es") && !doc.AddLanguage("es") && !doc.AddLanguage(""));
    CHECK(doc.Stats().size() == 4 && doc.MissingKeys("es").size() == 4); // a new column, all missing
    CHECK(doc.RemoveKey("quit") && !doc.RemoveKey("quit") && !doc.Table().HasKey("quit"));
}

AETHER_TEST(LocEditor_SaveReloadKeepsEverythingIncludingEmptyLanguages) {
    const stdfs::path file = MakeTable(Dir("save"));
    LocalizationDocument doc;
    CHECK(doc.Open(file));
    doc.SetCell("de", "quit", "Beenden");
    doc.AddLanguage("es");
    CHECK(doc.Save() && !doc.Dirty());
    LocalizationDocument again;
    CHECK(again.Open(file) && *again.Table().Find("de", "quit") == "Beenden");
    CHECK(again.Stats().size() == 4 && again.MissingKeys("es").size() == 3); // the empty es column survives
    CHECK(!again.Open(file.parent_path() / "missing.astrings") && !again.Errors().empty());
}

AETHER_TEST(LocEditor_PoRoundTripAndGather) {
    const stdfs::path root = Dir("po_gather");
    const stdfs::path file = MakeTable(root);
    Write(root / "Content/UI/menu.aui", R"({"root":{"type":"Text","text":"Settings","text_key":"menu.settings"}})");
    LocalizationDocument doc;
    CHECK(doc.Open(file));
    CHECK(doc.ExportPo("de", root / "po/de.po") && stdfs::exists(root / "po/de.po"));
    // The translator fills in a gap and sends it back.
    std::string po = Slurp(root / "po/de.po");
    const std::string empty_quit = "msgctxt \"quit\"\nmsgid \"Quit\"\nmsgstr \"\"";
    const usize at = po.find(empty_quit);
    CHECK(at != std::string::npos);
    po.replace(at, empty_quit.size(), "msgctxt \"quit\"\nmsgid \"Quit\"\nmsgstr \"Beenden\"");
    CHECK(doc.ImportPo("de", po) && *doc.Table().Find("de", "quit") == "Beenden" && doc.Dirty());
    // The gather step: a check changes nothing; a real one needs a saved table.
    CHECK(doc.Gather(root / "Content", true) && doc.LastGather().merge.added == 1);
    CHECK(!doc.Table().HasKey("menu.settings"));
    CHECK(!doc.Gather(root / "Content", false)); // dirty
    CHECK(doc.Save());
    CHECK(doc.Gather(root / "Content", false) && doc.Table().HasKey("menu.settings") && !doc.Dirty());
    CHECK(Slurp(file).find("menu.settings,,Settings,") != std::string::npos);
    CHECK(Slurp(file).find("quit,Beenden,Quit,Quitter") != std::string::npos);
}

AETHER_TEST(LocEditor_PanelDrawsAndPreviewsPseudoText) {
    HeadlessImGui ui;
    const stdfs::path file = MakeTable(Dir("panel"));
    LocalizationDocument doc;
    LocalizationPanel panel(doc);
    ui.Frame([&] { panel.Draw(); }); // nothing open
    CHECK(doc.Open(file));
    panel.SetContentDirectory(file.parent_path().parent_path());
    panel.SelectLanguage("de");
    for (int frame = 0; frame < 3; ++frame) ui.Frame([&] { panel.Draw(); });
    CHECK(ImGui::GetDrawData()->TotalVtxCount > 0);
    // The preview makes keyed text show the pseudo language, and gives the old one back.
    loc::Localization before;
    before.MakeActive();
    CHECK(loc::Localize::GetText("play", "Play") == "Play");
    panel.SetPreview(true);
    CHECK(panel.Previewing() && loc::Localization::Active() == &panel.PreviewLocalization());
    const std::string shown = loc::Localize::GetText("play", "Play");
    CHECK(shown.front() == '[' && shown != "Play");
    doc.SetCell("en", "play", "Go"); // a change to the table follows
    ui.Frame([&] { panel.Draw(); });
    CHECK(loc::Localize::GetText("play", "Play").find("G") != std::string::npos);
    panel.SetPreview(false);
    CHECK(!panel.Previewing() && loc::Localization::Active() == &before);
    CHECK(loc::Localize::GetText("play", "Play") == "Play");
    before.Clear();
}

AETHER_TEST(LocEditor_WorkspaceShowsTheSampleTable) {
    HeadlessImGui ui;
    EditorWorkspace ws;
    const i64 tool = ws.FindTool("Localization");
    CHECK(tool >= 0 && std::string(ws.ToolCategory(static_cast<usize>(tool))) == "Data");
    ws.Select(static_cast<usize>(tool));
    for (int frame = 0; frame < 2; ++frame) ui.Frame([&] { ws.DrawHubContents(); });
    CHECK(ImGui::GetDrawData()->TotalVtxCount > 0);
    CHECK(ws.Extensions().FindAssetTypeByExtension(".astrings") != nullptr);
    CHECK(ws.Extensions().OpenAsset(stdfs::temp_directory_path() / "aether_editor_loc_samples/Content/Localization/strings.astrings"));
}
