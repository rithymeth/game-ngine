// Multi-object editing in the Inspector (parity Stage 4): the values a selection shares, the mixed ones it does
// not, an edit across the selection as one undo step, and renaming a selection. Headless.

#include "aether/scene/components.h"
#include "aether/scene/entity_guid.h"
#include "core/commands.h"
#include "core/scene_document.h"
#include "ui/entity_inspector.h"
#include "ui/reflected_inspector.h"
#include "world/scene_panel.h"
#include "test_framework.h"

#include <imgui.h>

#include <algorithm>
#include <string>
#include <vector>

using namespace aether;
using namespace aether::editor;

namespace multi_test {

struct Charge {
    f32 current = 100.0f;
    std::string name = "unit";
    std::vector<f32> samples;
};

struct Shield {
    i32 rating = 5;
};

} // namespace multi_test

AETHER_REFLECT(multi_test::Charge, 1,
    AETHER_FIELD(current, Field_EditAnywhere, {.range_min = 0, .range_max = 1000}),
    AETHER_FIELD(name, Field_EditAnywhere),
    AETHER_FIELD(samples, Field_EditAnywhere)
)
AETHER_REFLECT(multi_test::Shield, 1, AETHER_FIELD(rating, Field_EditAnywhere))

namespace {

class HeadlessImGui {
public:
    HeadlessImGui() {
        context_ = ImGui::CreateContext();
        ImGui::GetIO().ConfigMacOSXBehaviors = false;
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

struct Fixture {
    World world;
    GuidIndex guids;
    EditorHooks hooks;
    CommandContext ctx{world, guids, &hooks};
    CommandStack stack;
};

} // namespace

AETHER_TEST(MultiEdit_ValuesEqualComparesByValueNotByBytes) {
    const reflect::TypeInfo& vitals = reflect::Reflect<multi_test::Charge>();
    multi_test::Charge a;
    multi_test::Charge b;
    AETHER_CHECK(ValuesEqual(vitals, &a, &b)); // same text in two separate buffers is the same value
    b.name = "unit ";
    AETHER_CHECK(!ValuesEqual(vitals, &a, &b));
    b.name = a.name;
    AETHER_CHECK(ValuesEqual(vitals, &a, &b));
    b.samples = {1.0f, 2.0f}; // an array of a different length is a different value
    AETHER_CHECK(!ValuesEqual(vitals, &a, &b));
    a.samples = {1.0f, 2.0f};
    AETHER_CHECK(ValuesEqual(vitals, &a, &b));
    b.samples[1] = 3.0f;
    AETHER_CHECK(!ValuesEqual(vitals, &a, &b));
    a.samples[1] = 3.0f;
    AETHER_CHECK(ValuesEqual(vitals, &a, &b));
    b.current = 99.0f;
    AETHER_CHECK(!ValuesEqual(vitals, &a, &b));

    // Struct fields are compared through their own fields (a quaternion has no bytes to memcmp usefully).
    const reflect::TypeInfo& transform = reflect::Reflect<Transform>();
    Transform ta;
    Transform tb;
    AETHER_CHECK(ValuesEqual(transform, &ta, &tb));
    tb.rotation = Quaternion::FromAxisAngle(Vec3(0.0f, 1.0f, 0.0f), 0.5f);
    AETHER_CHECK(!ValuesEqual(transform, &ta, &tb));
    ta.rotation = tb.rotation;
    AETHER_CHECK(ValuesEqual(transform, &ta, &tb));
}

AETHER_TEST(MultiEdit_DrawingAMixedSelectionLeavesItMixed) {
    HeadlessImGui ui;
    multi_test::Charge a;
    multi_test::Charge b;
    b.current = 40.0f;
    std::vector<void*> objects{&a, &b};
    const reflect::TypeInfo& vitals = reflect::Reflect<multi_test::Charge>();
    for (int i = 0; i < 3; ++i) {
        ui.Frame([&] { InspectObjects(vitals, objects, "vitals"); });
    }
    AETHER_CHECK(a.current == 100.0f && b.current == 40.0f); // showing a mixed field does not unify it
    AETHER_CHECK(a.name == "unit" && b.name == "unit");
}

AETHER_TEST(EntityInspector_EditAcrossSelectionIsOneUndoStep) {
    HeadlessImGui ui;
    Fixture f;
    const Entity a = f.world.CreateEntity(multi_test::Charge{});
    const Entity b = f.world.CreateEntity(multi_test::Charge{});
    const std::vector<Entity> selection{a, b};
    auto frame = [&](bool focus_first_field) {
        ui.Frame([&] {
            if (focus_first_field) ImGui::SetKeyboardFocusHere(1); // widget 0 is the component header
            InspectEntities(f.ctx, f.stack, selection);
        });
    };
    frame(false); // the first frame gives both entities GUIDs
    AETHER_CHECK(f.stack.UndoCount() == 0);
    EntityGuid guid_a = f.world.GetComponent<IdComponent>(a)->guid;
    EntityGuid guid_b = f.world.GetComponent<IdComponent>(b)->guid;
    AETHER_CHECK(!guid_a.IsNull() && !guid_b.IsNull());

    frame(true);
    frame(false);
    ImGuiIO& io = ImGui::GetIO();
    io.AddInputCharactersUTF8("250");
    frame(false);
    io.AddKeyEvent(ImGuiKey_Enter, true);
    frame(false);
    io.AddKeyEvent(ImGuiKey_Enter, false);
    frame(false);

    const Entity live_a = f.guids.Find(f.world, guid_a);
    const Entity live_b = f.guids.Find(f.world, guid_b);
    AETHER_CHECK(f.world.GetComponent<multi_test::Charge>(live_a)->current == 250.0f);
    AETHER_CHECK(f.world.GetComponent<multi_test::Charge>(live_b)->current == 250.0f);
    AETHER_CHECK(f.stack.UndoCount() == 1);
    AETHER_CHECK(f.stack.UndoLabel() == "Edit Charge.current on 2 entities");

    AETHER_CHECK(f.stack.Undo(f.ctx));
    AETHER_CHECK(f.world.GetComponent<multi_test::Charge>(live_a)->current == 100.0f);
    AETHER_CHECK(f.world.GetComponent<multi_test::Charge>(live_b)->current == 100.0f);
    AETHER_CHECK(f.stack.Redo(f.ctx));
    AETHER_CHECK(f.world.GetComponent<multi_test::Charge>(live_a)->current == 250.0f);
    AETHER_CHECK(f.world.GetComponent<multi_test::Charge>(live_b)->current == 250.0f);
}

AETHER_TEST(EntityInspector_SharedComponentsAreWhatEveryEntityHas) {
    Fixture f;
    const Entity a = f.world.CreateEntity(multi_test::Charge{}, multi_test::Shield{});
    const Entity b = f.world.CreateEntity(multi_test::Charge{});
    const std::vector<Entity> selection{a, b};
    const std::vector<ComponentId> shared = SharedComponents(f.world, selection);
    const ComponentId vitals = GetComponentId<multi_test::Charge>();
    const ComponentId armor = GetComponentId<multi_test::Shield>();
    AETHER_CHECK(std::find(shared.begin(), shared.end(), vitals) != shared.end());
    AETHER_CHECK(std::find(shared.begin(), shared.end(), armor) == shared.end()); // only one entity has it
    AETHER_CHECK(std::find(shared.begin(), shared.end(), GetComponentId<IdComponent>()) == shared.end()); // identity is not a card
}

AETHER_TEST(SceneDocument_RenameSelectionIsOneUndoStep) {
    SceneDocument document;
    const Entity a = document.CreateEntity("Crate A");
    const Entity b = document.CreateEntity("Crate B");
    const Entity c = document.CreateEntity("Lamp");
    const usize created = document.Commands().UndoCount(); // one entry per CreateEntity
    document.SetSelection({a, b});
    AETHER_CHECK(document.RenameSelection("Hero", true) == 2);
    AETHER_CHECK(document.Name(a) == "Hero" && document.Name(b) == "Hero" && document.Name(c) == "Lamp");
    AETHER_CHECK(document.Commands().UndoCount() == created + 1);
    AETHER_CHECK(document.Undo());
    AETHER_CHECK(document.Name(a) == "Crate A" && document.Name(b) == "Crate B"); // each keeps its own old name
    AETHER_CHECK(document.Redo());

    // Typing a name is still one step: the edits before the commit merge.
    document.SetSelection({a, b});
    AETHER_CHECK(document.RenameSelection("Hero", false) == 0); // already the shared name
    AETHER_CHECK(document.RenameSelection("H", false) == 2);
    AETHER_CHECK(document.RenameSelection("He", false) == 2);
    AETHER_CHECK(document.RenameSelection("Hex", true) == 2);
    AETHER_CHECK(document.Name(a) == "Hex" && document.Name(b) == "Hex");
    AETHER_CHECK(document.Commands().UndoCount() == created + 2);
    AETHER_CHECK(document.Undo());
    AETHER_CHECK(document.Name(a) == "Hero" && document.Name(b) == "Hero");
}

AETHER_TEST(ScenePanel_ADrawnMultiSelectionRecordsNothing) {
    HeadlessImGui ui;
    ScenePanel panel;
    const Entity a = panel.Document().CreateEntity("Crate A");
    const Entity b = panel.Document().CreateEntity("Crate B");
    panel.Document().SetSelection({a, b});
    const usize records = panel.Document().Commands().UndoCount();
    for (int i = 0; i < 3; ++i) ui.Frame([&] { panel.Draw(); });
    AETHER_CHECK(panel.Document().Commands().UndoCount() == records); // showing a mixed selection is not an edit
    AETHER_CHECK(panel.Document().Name(a) == "Crate A" && panel.Document().Name(b) == "Crate B");
}
