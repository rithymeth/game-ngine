// The Inspector's property search (parity Stage 4): only the properties whose field or component name matches are
// drawn, and searching is not an edit. Headless.

#include "ui/reflected_inspector.h"
#include "world/scene_panel.h"
#include "test_framework.h"

#include <imgui.h>

#include <string>
#include <vector>

using namespace aether;
using namespace aether::editor;

namespace search_test {

struct Beacon {
    f32 intensity = 1.0f;
    f32 range = 10.0f;
    bool enabled = true;
    i32 secret = 0; // not editable: never shown, never matched
};

} // namespace search_test

AETHER_REFLECT(search_test::Beacon, 1,
    AETHER_FIELD(intensity, Field_EditAnywhere),
    AETHER_FIELD(range, Field_EditAnywhere),
    AETHER_FIELD(enabled, Field_EditAnywhere),
    AETHER_FIELD(secret)
)

namespace {

class HeadlessImGui {
public:
    HeadlessImGui() {
        context_ = ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(1024, 768);
        io.DeltaTime = 1.0f / 60.0f;
        unsigned char* pixels = nullptr;
        int w = 0, h = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
    }
    ~HeadlessImGui() { ImGui::DestroyContext(context_); }
    template <typename F>
    void Frame(F&& body) {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(600, 700));
        ImGui::Begin("Inspector");
        body();
        ImGui::End();
        ImGui::Render();
    }

private:
    ImGuiContext* context_ = nullptr;
};

const reflect::TypeInfo& BeaconType() { return reflect::Reflect<search_test::Beacon>(); }
const reflect::FieldInfo& Field(const char* name) { return *BeaconType().FindField(name); }

} // namespace

AETHER_TEST(InspectorSearch_MatchesFieldAndComponentNames) {
    AETHER_CHECK(FieldMatchesSearch("Beacon", Field("range"), "ran"));       // a field name, part of it
    AETHER_CHECK(FieldMatchesSearch("Beacon", Field("range"), "RANGE"));     // case does not matter
    AETHER_CHECK(FieldMatchesSearch("Beacon", Field("range"), "bea"));       // the component name matches too
    AETHER_CHECK(!FieldMatchesSearch("Beacon", Field("range"), "light"));    // nothing in the row
    AETHER_CHECK(FieldMatchesSearch("Beacon", Field("range"), ""));          // no query matches everything

    AETHER_CHECK(HasMatchingFields(BeaconType(), "inten"));
    AETHER_CHECK(HasMatchingFields(BeaconType(), "Beacon"));
    AETHER_CHECK(!HasMatchingFields(BeaconType(), "secret")); // a field the Inspector does not show is not a match
    AETHER_CHECK(!HasMatchingFields(BeaconType(), "nothing"));
    AETHER_CHECK(HasMatchingFields(BeaconType(), "") == HasInspectableFields(BeaconType()));
}

AETHER_TEST(InspectorSearch_OnlyMatchingRowsAreDrawn) {
    HeadlessImGui ui;
    search_test::Beacon beacon;
    auto height_with = [&](const char* search) {
        f32 height = 0.0f;
        ui.Frame([&] {
            InspectOptions options;
            options.search = search;
            InspectObject(BeaconType(), &beacon, "beacon", options);
            height = ImGui::GetCursorPosY();
        });
        return height;
    };
    height_with(""); // the first frame sizes the window and the table
    const f32 all = height_with("");
    const f32 one = height_with("range");
    const f32 none = height_with("zzz");
    AETHER_CHECK(one < all);  // one row instead of three
    AETHER_CHECK(none < one); // no rows at all
    AETHER_CHECK(beacon.intensity == 1.0f && beacon.range == 10.0f && beacon.enabled);
}

AETHER_TEST(ScenePanel_APropertySearchFiltersAndEditsNothing) {
    HeadlessImGui ui;
    ScenePanel panel;
    const Entity e = panel.Document().GetWorld().CreateEntity(search_test::Beacon{});
    panel.Document().SetSelection({e});
    const usize records = panel.Document().Commands().UndoCount();
    panel.SetPropertySearch("range");
    AETHER_CHECK(panel.PropertySearch() == "range");
    for (int i = 0; i < 3; ++i) ui.Frame([&] { panel.Draw(); });
    AETHER_CHECK(panel.Document().Commands().UndoCount() == records); // searching is not an edit
    const auto* beacon = panel.Document().GetWorld().GetComponent<search_test::Beacon>(e);
    AETHER_CHECK(beacon != nullptr && beacon->range == 10.0f && beacon->intensity == 1.0f);
    panel.SetPropertySearch("");
    AETHER_CHECK(panel.PropertySearch().empty());
}
