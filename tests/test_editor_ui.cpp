// Headless tests for the portable editor UI (editor/src/ui): a real Dear
// ImGui context with no window or renderer, driven by synthetic input.

#include "test_framework.h"
#include "ui/reflected_inspector.h"

#include <imgui.h>

#include <functional>
#include <string>

using namespace aether;
using namespace aether::reflect;

namespace inspect_test {

enum class Shape : u8 { Sphere, Box, Capsule };

struct Settings {
    std::string title = "untitled";
    f32 speed = 1.0f;
    i32 count = 3;
    u16 small = 7;
    f64 precise = 0.5;
    bool enabled = true;
    Shape shape = Shape::Box;
    Vec3 offset{1, 2, 3};
    Quaternion rotation;
    Mat4 matrix;
    char tag[16] = "tag";
    i32 hidden_internal = 42;  // no flags: not shown
    f32 computed = 9.0f;       // ReadOnly: shown, not editable
};

struct Empty {
    i32 internal = 0;
};

} // namespace inspect_test

AETHER_ENUM(inspect_test::Shape, 1, AETHER_ENUM_VALUE(Sphere), AETHER_ENUM_VALUE(Box), AETHER_ENUM_VALUE(Capsule))
AETHER_REFLECT(inspect_test::Settings, 1,
    AETHER_FIELD(title, Field_EditAnywhere, {.tooltip = "Shown in the title bar"}),
    AETHER_FIELD(speed, Field_EditAnywhere, {.category = "Motion", .range_min = 0, .range_max = 10, .units = "m/s"}),
    AETHER_FIELD(count, Field_EditAnywhere, {.category = "Motion"}),
    AETHER_FIELD(small, Field_EditAnywhere),
    AETHER_FIELD(precise, Field_EditAnywhere),
    AETHER_FIELD(enabled, Field_EditAnywhere),
    AETHER_FIELD(shape, Field_EditAnywhere),
    AETHER_FIELD(offset, Field_EditAnywhere, {.units = "m"}),
    AETHER_FIELD(rotation, Field_EditAnywhere),
    AETHER_FIELD(matrix, Field_EditAnywhere),
    AETHER_FIELD(tag, Field_EditAnywhere),
    AETHER_FIELD(hidden_internal),
    AETHER_FIELD(computed, Field_ReadOnly)
)
AETHER_REFLECT(inspect_test::Empty, 1, AETHER_FIELD(internal))

namespace {

// Owns an ImGui context for one test. No platform or renderer backend: the
// font atlas is built on the CPU and draw data is generated but never drawn.
class HeadlessImGui {
public:
    HeadlessImGui() {
        context_ = ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(1024, 768);
        io.DeltaTime = 1.0f / 60.0f;
        unsigned char* pixels = nullptr;
        int width = 0;
        int height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    }
    ~HeadlessImGui() { ImGui::DestroyContext(context_); }

    // Runs one frame with `body` drawn inside a window.
    void Frame(const std::function<void()>& body) {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(600, 700));
        ImGui::Begin("Test");
        body();
        ImGui::End();
        ImGui::Render();
    }

    void Type(const char* text) { ImGui::GetIO().AddInputCharactersUTF8(text); }
    void Key(ImGuiKey key, bool down) { ImGui::GetIO().AddKeyEvent(key, down); }

private:
    ImGuiContext* context_ = nullptr;
};

// Focuses the Nth focusable widget inside the inspector (0 = first shown
// field), types `text` into it and presses Enter. Returns every result seen.
std::vector<editor::InspectResult> TypeIntoField(HeadlessImGui& ui, inspect_test::Settings& settings, int widget,
                                                 const char* text) {
    std::vector<editor::InspectResult> results;
    auto draw = [&](bool focus) {
        ui.Frame([&] {
            if (focus) {
                ImGui::SetKeyboardFocusHere(widget);
            }
            results.push_back(editor::InspectObject(settings, "settings"));
        });
    };
    draw(true);  // request focus
    draw(false); // widget becomes active (text input mode)
    ui.Type(text);
    draw(false);
    ui.Key(ImGuiKey_Enter, true);
    draw(false);
    ui.Key(ImGuiKey_Enter, false);
    draw(false);
    return results;
}

bool AnyCommitted(const std::vector<editor::InspectResult>& results, const char* field_name) {
    for (const auto& r : results) {
        if (r.committed && r.changed_field != nullptr && std::string(r.changed_field->name) == field_name) {
            return true;
        }
    }
    return false;
}

} // namespace

AETHER_TEST(EditorUI_InspectorDrawsEveryKindWithoutInput) {
    HeadlessImGui ui;
    inspect_test::Settings settings;
    for (int frame = 0; frame < 3; ++frame) {
        ui.Frame([&] {
            editor::InspectResult result = editor::InspectObject(settings, "settings");
            AETHER_CHECK(!result.Changed() && !result.committed);
        });
    }
    // Nothing changed without input.
    AETHER_CHECK(settings.title == "untitled" && settings.speed == 1.0f && settings.count == 3);
    AETHER_CHECK(ImGui::GetDrawData() != nullptr && ImGui::GetDrawData()->TotalVtxCount > 0);

    AETHER_CHECK(editor::HasInspectableFields(Reflect<inspect_test::Settings>()));
    AETHER_CHECK(!editor::HasInspectableFields(Reflect<inspect_test::Empty>()));
}

AETHER_TEST(EditorUI_TypingIntoStringFieldEditsAndCommits) {
    HeadlessImGui ui;
    inspect_test::Settings settings;
    auto results = TypeIntoField(ui, settings, /*widget=*/0, "Hero");
    AETHER_CHECK(settings.title == "Hero"); // focusing selects all, so typing replaces
    AETHER_CHECK(AnyCommitted(results, "title"));
}

AETHER_TEST(EditorUI_NumberFieldsRespectMetaRange) {
    HeadlessImGui ui;
    inspect_test::Settings settings;
    // Widget 1 is `speed` (range 0..10): typing 500 is clamped to 10.
    auto results = TypeIntoField(ui, settings, /*widget=*/1, "500");
    AETHER_CHECK(settings.speed == 10.0f);
    AETHER_CHECK(AnyCommitted(results, "speed"));

    // Widget 2 is `count` (no range).
    TypeIntoField(ui, settings, /*widget=*/2, "-12");
    AETHER_CHECK(settings.count == -12);
}

AETHER_TEST(EditorUI_FixedStringFieldIsEditable) {
    HeadlessImGui ui;
    inspect_test::Settings settings;
    // Focusable widgets in order: title, speed, count, small, precise,
    // enabled, shape, offset x/y/z, rotation x/y/z/w, matrix (tree node), tag.
    auto results = TypeIntoField(ui, settings, /*widget=*/15, "renamed");
    AETHER_CHECK(std::string(settings.tag) == "renamed");
    AETHER_CHECK(AnyCommitted(results, "tag"));
    AETHER_CHECK(settings.hidden_internal == 42 && settings.computed == 9.0f);
}

AETHER_TEST(EditorUI_AddComponentButtonPicksNothingWithoutInput) {
    HeadlessImGui ui;
    const TypeInfo* candidates[] = {&Reflect<inspect_test::Settings>(), &Reflect<inspect_test::Empty>()};
    ui.Frame([&] { AETHER_CHECK(editor::AddComponentButton(candidates) == -1); });
    ui.Frame([&] { AETHER_CHECK(editor::AddComponentButton({}) == -1); });
}
