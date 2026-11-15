// The Inspector's property menu (parity Stage 4): copying a value, pasting it into a field of the same type anywhere,
// and resetting a field to the default a default-constructed component has. Headless.

#include "ui/reflected_inspector.h"
#include "test_framework.h"

#include <imgui.h>

#include <string>
#include <vector>

using namespace aether;
using namespace aether::editor;

namespace values_test {

struct Thermostat {
    f32 target = 21.5f;
    i32 mode = 2;
    std::string label = "lab";
    std::vector<f32> history;
};

} // namespace values_test

AETHER_REFLECT(values_test::Thermostat, 1,
    AETHER_FIELD(target, Field_EditAnywhere, {.range_min = 0, .range_max = 100}),
    AETHER_FIELD(mode, Field_EditAnywhere),
    AETHER_FIELD(label, Field_EditAnywhere),
    AETHER_FIELD(history, Field_EditAnywhere)
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

const reflect::TypeInfo& ThermostatType() { return reflect::Reflect<values_test::Thermostat>(); }
const reflect::FieldInfo& Field(const char* name) { return *ThermostatType().FindField(name); }

} // namespace

AETHER_TEST(InspectorValues_CopyAndPasteOnlyWithinOneType) {
    values_test::Thermostat a;
    values_test::Thermostat b;
    b.target = 3.0f;
    CopyFieldValue(Field("target"), &a);
    AETHER_CHECK(FieldClipboardFits(Field("target")));
    AETHER_CHECK(!FieldClipboardFits(Field("mode"))); // another type: cannot take it
    AETHER_CHECK(PasteFieldValue(Field("target"), &b));
    AETHER_CHECK(b.target == 21.5f);
    AETHER_CHECK(!PasteFieldValue(Field("target"), &b)); // already that value: nothing to undo

    b.target = 40.0f;
    AETHER_CHECK(!PasteFieldValue(Field("mode"), &b));
    AETHER_CHECK(b.target == 40.0f && b.mode == 2); // a refused paste writes nothing

    ClearFieldClipboard();
    AETHER_CHECK(!FieldClipboardFits(Field("target")));
    AETHER_CHECK(!PasteFieldValue(Field("target"), &b));
    AETHER_CHECK(b.target == 40.0f);
}

AETHER_TEST(InspectorValues_TheClipboardOwnsItsValue) {
    values_test::Thermostat a;
    values_test::Thermostat b;
    a.history = {1.0f, 2.0f};
    CopyFieldValue(Field("history"), &a);
    a.history[0] = 9.0f; // the source changing later does not change what was copied
    b.history = {5.0f};
    AETHER_CHECK(PasteFieldValue(Field("history"), &b));
    AETHER_CHECK(b.history.size() == 2 && b.history[0] == 1.0f && b.history[1] == 2.0f);

    a.label = "cold";
    CopyFieldValue(Field("label"), &a);
    b.label = "a much longer string";
    AETHER_CHECK(PasteFieldValue(Field("label"), &b));
    AETHER_CHECK(b.label == "cold");
    AETHER_CHECK(a.label == "cold"); // pasting is a copy in, not a move
    ClearFieldClipboard();
}

AETHER_TEST(InspectorValues_ResetUsesTheDefaultConstructedValue) {
    values_test::Thermostat live;
    live.target = 90.0f;
    live.mode = 7;
    live.label = "hot";
    live.history = {1.0f, 2.0f, 3.0f};

    const reflect::Any default_target = DefaultFieldValue(ThermostatType(), Field("target"));
    AETHER_CHECK(default_target.HasValue() && default_target.Get<f32>() == 21.5f);
    AETHER_CHECK(ResetFieldValue(Field("target"), &live, default_target));
    AETHER_CHECK(live.target == 21.5f);
    AETHER_CHECK(!ResetFieldValue(Field("target"), &live, default_target)); // already the default: no undo step

    AETHER_CHECK(ResetFieldValue(Field("mode"), &live, DefaultFieldValue(ThermostatType(), Field("mode"))));
    AETHER_CHECK(live.mode == 2);
    AETHER_CHECK(ResetFieldValue(Field("label"), &live, DefaultFieldValue(ThermostatType(), Field("label"))));
    AETHER_CHECK(live.label == "lab");
    AETHER_CHECK(ResetFieldValue(Field("history"), &live, DefaultFieldValue(ThermostatType(), Field("history"))));
    AETHER_CHECK(live.history.empty());

    // A default of another type is refused, not memcpy'd in.
    AETHER_CHECK(!ResetFieldValue(Field("target"), &live, DefaultFieldValue(ThermostatType(), Field("mode"))));
    AETHER_CHECK(!ResetFieldValue(Field("target"), &live, reflect::Any{}));
    AETHER_CHECK(live.target == 21.5f);
}

AETHER_TEST(InspectorValues_DrawingAFreePropertyMenuChangesNothing) {
    HeadlessImGui ui;
    values_test::Thermostat a;
    values_test::Thermostat b;
    b.target = 3.0f;
    CopyFieldValue(Field("target"), &a);
    InspectOptions options;
    options.default_value = [](const reflect::FieldInfo& field) { return DefaultFieldValue(ThermostatType(), field); };
    std::vector<void*> objects{&a, &b};
    for (int i = 0; i < 3; ++i) {
        ui.Frame([&] { InspectObjects(ThermostatType(), objects, "thermostat", options); });
    }
    AETHER_CHECK(a.target == 21.5f && b.target == 3.0f); // a menu value applies when picked, not when drawn
    ClearFieldClipboard();
}
