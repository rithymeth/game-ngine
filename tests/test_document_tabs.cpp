#include "workspace/editor_workspace.h"

#include "core/document_tabs.h"
#include "core/editor_document.h"
#include "core/scene_document.h"
#include "world/scene_panel.h"

#include "test_framework.h"

#include <imgui.h>

#include <filesystem>
#include <string>

// Stage 1 of docs/design/EDITOR_PARITY_AUDIT.md: document tabs. The strip's questions -- which documents are open,
// which is focused, what a close would throw away -- live in `DocumentTabs`, so they are testable without pixels; the
// workspace opens the scene as the first document and answers the close prompt through public methods, as the quit
// prompt does.

using namespace aether;
using namespace aether::editor;

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
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    }
    ~HeadlessImGui() { ImGui::DestroyContext(context_); }

private:
    ImGuiContext* context_ = nullptr;
    unsigned char* pixels = nullptr;
    int width = 0;
    int height = 0;
};

// A document with no panel, so the tab model is exercised without any tool: it records saves and can be made dirty.
class FakeDocument : public EditorDocument {
public:
    explicit FakeDocument(std::string name) : name_(std::move(name)) {}

    const std::filesystem::path& FilePath() const override { return path_; }
    std::string DisplayName() const override { return name_; }
    const char* Kind() const override { return "test"; }
    bool IsDirty() const override { return dirty_; }
    bool CanUndo() const override { return false; }
    bool CanRedo() const override { return false; }
    std::string UndoLabel() const override { return {}; }
    std::string RedoLabel() const override { return {}; }
    bool Undo() override { return false; }
    bool Redo() override { return false; }
    bool Save(const std::filesystem::path& file, std::string* error) override {
        ++saves;
        if (fail_save) {
            if (error != nullptr) *error = "The disk said no.";
            return false;
        }
        if (!file.empty()) path_ = file;
        dirty_ = false;
        return true;
    }

    void SetDirty(bool dirty) { dirty_ = dirty; }
    int saves = 0;
    bool fail_save = false;

private:
    std::string name_;
    std::filesystem::path path_;
    bool dirty_ = false;
};

} // namespace

// Opening a document focuses its tab rather than adding a second, and the last one stays open.
AETHER_TEST(DocumentTabs_OpensEachDocumentOnceAndKeepsTheLast) {
    FakeDocument a("A"), b("B");
    DocumentTabs tabs;
    bool opened = false;
    CHECK(tabs.Open(a, &opened) == 0 && opened);
    CHECK(tabs.Open(b, &opened) == 1 && opened);
    CHECK(tabs.Count() == 2 && tabs.ActiveIndex() == 1);

    CHECK(tabs.Open(a, &opened) == 0 && !opened); // focused, not added again
    CHECK(tabs.Count() == 2 && tabs.ActiveIndex() == 0);

    CHECK(tabs.Close(0, true));
    CHECK(tabs.Count() == 1 && tabs.ActiveIndex() == 0 && &tabs.At(0) == &b);
    CHECK(!tabs.Close(0, true)); // the last tab stays; its document still exists
    CHECK(tabs.Count() == 1);
}

// A tab closes without its document unless the document is dirty, in which case the caller has to discard explicitly.
AETHER_TEST(DocumentTabs_ClosesLikeANeighborAndProtectsDirtyDocuments) {
    FakeDocument a("A"), b("B"), c("C");
    DocumentTabs tabs;
    tabs.Open(a);
    tabs.Open(b);
    tabs.Open(c);
    tabs.Activate(1);
    CHECK(tabs.Close(1)); // clean: closes, neighbor becomes active
    CHECK(tabs.Count() == 2 && &tabs.At(tabs.ActiveIndex()) == &c);

    b.SetDirty(true);
    tabs.Open(b);
    CHECK(tabs.Count() == 3 && tabs.ActiveIndex() == 2);
    CHECK(!tabs.Close(2)); // dirty: refused
    CHECK(tabs.Count() == 3 && tabs.ActiveIndex() == 2);
    CHECK(tabs.Close(2, true)); // discarded on purpose
    CHECK(tabs.Count() == 2 && &tabs.At(tabs.ActiveIndex()) == &c);
}

// Close others keeps the chosen tab and refuses (nothing changes) when another document would lose unsaved changes.
AETHER_TEST(DocumentTabs_CloseOthersKeepsTheChosenOne) {
    FakeDocument a("A"), b("B"), c("C");
    DocumentTabs tabs;
    tabs.Open(a);
    tabs.Open(b);
    tabs.Open(c);
    c.SetDirty(true);
    CHECK(!tabs.CloseOthers(0)); // C is dirty: refused as a whole
    CHECK(tabs.Count() == 3);
    CHECK(tabs.CloseOthers(0, true));
    CHECK(tabs.Count() == 1 && &tabs.At(0) == &a && tabs.ActiveIndex() == 0);
    CHECK(!tabs.CloseOthers(5)); // out of range
}

// The workspace's scene is the first tab; closing it is refused while it is the only one.
AETHER_TEST(DocumentTabs_WorkspaceOpensTheSceneAsTheFirstTab) {
    EditorWorkspace workspace;
    CHECK(workspace.DocumentCount() == 1);
    CHECK(workspace.ActiveDocumentIndex() == 0);
    CHECK(&workspace.DocumentAt(0) == &workspace.Scene().Document());
    CHECK(std::string(workspace.DocumentAt(0).Kind()) == "scene");
    CHECK(workspace.DocumentAt(0).DisplayName() == workspace.ActiveDocument().DisplayName());

    CHECK(!workspace.CloseDocument(0));
    CHECK(workspace.DocumentCount() == 1);
    workspace.RequestCloseDocument(0); // no prompt for the last tab: there is nothing to close
    CHECK(!workspace.ClosePromptOpen());

    // Activating the scene tab brings the Scene tool back, so the tab does something visible.
    const i64 scene_tool = workspace.FindTool("Scene");
    CHECK(scene_tool >= 0);
    workspace.Select(0);
    workspace.ActivateDocument(0);
    CHECK(workspace.Selected() == static_cast<usize>(scene_tool));
}

// An asset editor's document opens as a second tab and is closed through the prompt, which asks before throwing
// unsaved changes away and saves through the document's own contract when told to.
AETHER_TEST(DocumentTabs_WorkspaceClosesADirtyAssetDocumentThroughThePrompt) {
    EditorWorkspace workspace;
    FakeDocument document("M_Door");
    document.SetDirty(true);
    CHECK(workspace.OpenDocument(document) == 1);
    CHECK(workspace.DocumentCount() == 2 && workspace.ActiveDocumentIndex() == 1);
    CHECK(&workspace.ActiveDocument() == &document);

    workspace.RequestCloseDocument(1);
    CHECK(workspace.ClosePromptOpen());
    CHECK(workspace.DocumentCount() == 2);

    CHECK(workspace.SaveAndCloseDocument());
    CHECK(document.saves == 1);
    CHECK(!document.IsDirty());
    CHECK(!workspace.ClosePromptOpen());
    CHECK(workspace.DocumentCount() == 1 && workspace.ActiveDocumentIndex() == 0);

    // A save that fails leaves the document open and the prompt up.
    FakeDocument second("M_Wall");
    second.SetDirty(true);
    second.fail_save = true;
    workspace.OpenDocument(second);
    workspace.RequestCloseDocument(1);
    CHECK(!workspace.SaveAndCloseDocument());
    CHECK(workspace.ClosePromptOpen());
    CHECK(workspace.DocumentCount() == 2);
    workspace.CloseDocumentWithoutSaving();
    CHECK(!workspace.ClosePromptOpen());
    CHECK(workspace.DocumentCount() == 1);

    // Closing without saving on a dirty document discards on purpose.
    FakeDocument third("M_Floor");
    third.SetDirty(true);
    workspace.OpenDocument(third);
    workspace.RequestCloseDocument(1);
    CHECK(workspace.ClosePromptOpen());
    workspace.CancelCloseDocument();
    CHECK(!workspace.ClosePromptOpen());
    CHECK(workspace.DocumentCount() == 2);
    workspace.RequestCloseDocument(1);
    workspace.CloseDocumentWithoutSaving();
    CHECK(workspace.DocumentCount() == 1);
}

// The strip draws with a dirty tab and with its close prompt up, without a host of its own.
AETHER_TEST(DocumentTabs_DrawsWithADirtyTabAndItsPrompt) {
    HeadlessImGui imgui;
    EditorWorkspace workspace;
    FakeDocument document("M_Dirty");
    document.SetDirty(true);
    workspace.OpenDocument(document);
    workspace.RequestCloseDocument(1);
    CHECK(workspace.ClosePromptOpen());

    ImGui::NewFrame();
    if (ImGui::Begin("##tabs_test")) workspace.DrawDocumentTabs(); // the strip and the modal both draw
    ImGui::End();
    ImGui::Render();
    workspace.CancelCloseDocument();

    FakeDocument clean("M_Clean");
    workspace.OpenDocument(clean);
    ImGui::NewFrame();
    if (ImGui::Begin("##tabs_test")) workspace.DrawDocumentTabs();
    ImGui::End();
    ImGui::Render();
    CHECK(workspace.DocumentCount() == 3);
}
