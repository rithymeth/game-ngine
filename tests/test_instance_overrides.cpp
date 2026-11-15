// The Inspector's instance override list (parity Stage 4): every override of a prefab instance across its entities, with
// the prefab's value and the instance's, each revertible on its own as one undo step. Headless.

#include "aether/reflection/serialize.h"
#include "aether/scene/components.h"
#include "aether/scene/entity_guid.h"
#include "aether/scene/prefab.h"
#include "core/commands.h"
#include "ui/entity_inspector.h"
#include "test_framework.h"

#include <imgui.h>

#include <algorithm>
#include <string>
#include <vector>

using namespace aether;
using namespace aether::editor;

namespace ovr_test {

struct Gauge {
    i32 level = 3;
    std::string label = "tank";
};

} // namespace ovr_test

AETHER_REFLECT(ovr_test::Gauge, 1,
    AETHER_FIELD(level, Field_EditAnywhere),
    AETHER_FIELD(label, Field_EditAnywhere)
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

// A prefab of a tank (local 1) with a needle under it (local 2), both with a Gauge; one instance of it, with the tank's
// level, the needle's label and a field the prefab does not have overridden.
struct Fixture {
    World world;
    GuidIndex guids;
    CommandContext ctx{world, guids};
    CommandStack stack;
    PrefabData prefab;
    assets::AssetGuid source = assets::NewAssetGuid();
    Entity root = kNullEntity;
    Entity needle = kNullEntity;

    Fixture() {
        (void)GetComponentId<ovr_test::Gauge>(); // the prefab names the components: register them before resolving
        (void)GetComponentId<EntityName>();
        PrefabEntity tank;
        tank.id = 1;
        tank.components["Gauge"] = reflect::ToJson(ovr_test::Gauge{});
        tank.components[reflect::Reflect<EntityName>().name] = reflect::ToJson(EntityName{"Tank"});
        PrefabEntity part;
        part.id = 2;
        part.parent = 1;
        part.components["Gauge"] = reflect::ToJson(ovr_test::Gauge{});
        part.components[reflect::Reflect<EntityName>().name] = reflect::ToJson(EntityName{"Needle"});
        prefab.entities = {tank, part};
        ctx.find_prefab = [this](const assets::AssetGuid& guid) { return guid == source ? &prefab : nullptr; };

        ResolveReport report;
        root = InstantiatePrefab(world, guids, source, prefab, nullptr, &report);
        PrefabInstance& instance = *world.GetComponent<PrefabInstance>(root);
        SetOverride(instance, 1, "Gauge", "level", 7);
        SetOverride(instance, 2, "Gauge", "label", "cold");
        SetOverride(instance, 2, "Gauge", "pressure", 2.5); // the prefab has no such field: orphaned
        (void)ResolvePrefabInstance(world, guids, root, prefab);
        for (const Entity e : report.created_entities) {
            const PrefabLink* link = world.GetComponent<PrefabLink>(e);
            if (link != nullptr && link->local_id == 2) needle = e;
        }
    }

    const ovr_test::Gauge& GaugeOf(Entity e) { return *world.GetComponent<ovr_test::Gauge>(e); }
    usize OverrideCount() { return world.GetComponent<PrefabInstance>(root)->overrides.size(); }
};

const InstanceOverride* Row(const std::vector<InstanceOverride>& rows, PrefabLocalId local, const char* path) {
    const auto it = std::find_if(rows.begin(), rows.end(), [&](const InstanceOverride& r) { return r.local_id == local && r.field_path == path; });
    return it == rows.end() ? nullptr : &*it;
}

} // namespace

AETHER_TEST(InstanceOverrides_ListsEveryOverrideOfTheInstanceWithBothValues) {
    Fixture f;
    AETHER_CHECK(!f.needle.IsNull());
    if (f.needle.IsNull()) return;
    AETHER_CHECK(f.GaugeOf(f.root).level == 7 && f.GaugeOf(f.needle).label == "cold");

    // Asked from the needle, the list still covers the whole instance, the tank's override first.
    const std::vector<InstanceOverride> rows = InstanceOverrides(f.ctx, f.needle);
    AETHER_CHECK(rows.size() == 3);
    if (rows.size() != 3) return;
    AETHER_CHECK(rows[0].local_id == 1);
    const InstanceOverride* level = Row(rows, 1, "level");
    const InstanceOverride* label = Row(rows, 2, "label");
    const InstanceOverride* pressure = Row(rows, 2, "pressure");
    AETHER_CHECK(level != nullptr && label != nullptr && pressure != nullptr);
    if (level == nullptr || label == nullptr || pressure == nullptr) return;
    AETHER_CHECK(level->entity == f.root && level->entity_name == "Tank" && level->component == "Gauge");
    AETHER_CHECK(level->prefab_value == "3" && level->value == "7" && !level->orphaned);
    AETHER_CHECK(label->entity == f.needle && label->entity_name == "Needle");
    AETHER_CHECK(label->prefab_value == "\"tank\"" && label->value == "\"cold\"" && !label->orphaned);
    AETHER_CHECK(pressure->orphaned && pressure->prefab_value.empty());
    AETHER_CHECK(InstanceOverrides(f.ctx, f.root).size() == 3);

    // Not part of an instance, or no way to read the prefab: nothing to list.
    const Entity loose = f.world.CreateEntity(ovr_test::Gauge{});
    AETHER_CHECK(InstanceOverrides(f.ctx, loose).empty());
    CommandContext blind{f.world, f.guids};
    AETHER_CHECK(InstanceOverrides(blind, f.needle).empty());
}

AETHER_TEST(InstanceOverrides_RevertingOneRowIsOneUndoStep) {
    Fixture f;
    if (f.needle.IsNull()) return;
    const std::vector<InstanceOverride> rows = InstanceOverrides(f.ctx, f.root); // asked from the tank, reverting the needle's
    const InstanceOverride* label = Row(rows, 2, "label");
    AETHER_CHECK(label != nullptr);
    if (label == nullptr) return;
    AETHER_CHECK(RevertInstanceOverride(f.ctx, f.stack, f.root, *label));
    AETHER_CHECK(f.GaugeOf(f.needle).label == "tank" && f.GaugeOf(f.root).level == 7);
    AETHER_CHECK(f.OverrideCount() == 2);
    AETHER_CHECK(f.stack.UndoCount() == 1 && f.stack.UndoLabel() == "Revert Gauge.label");
    AETHER_CHECK(!RevertInstanceOverride(f.ctx, f.stack, f.root, *label)); // already gone: nothing recorded
    AETHER_CHECK(f.stack.UndoCount() == 1);

    AETHER_CHECK(f.stack.Undo(f.ctx));
    AETHER_CHECK(f.GaugeOf(f.needle).label == "cold" && f.OverrideCount() == 3);
    AETHER_CHECK(f.stack.Redo(f.ctx));
    AETHER_CHECK(f.GaugeOf(f.needle).label == "tank");
}

AETHER_TEST(InstanceOverrides_AnOrphanedRowRevertsAloneEvenForAnUnknownComponent) {
    Fixture f;
    if (f.needle.IsNull()) return;
    SetOverride(*f.world.GetComponent<PrefabInstance>(f.root), 2, "Vanished", "", nlohmann::json::object());
    AETHER_CHECK(f.OverrideCount() == 4);
    std::vector<InstanceOverride> rows = InstanceOverrides(f.ctx, f.needle);
    const auto vanished = std::find_if(rows.begin(), rows.end(), [](const InstanceOverride& r) { return r.component == "Vanished"; });
    AETHER_CHECK(vanished != rows.end() && vanished->orphaned);
    if (vanished == rows.end()) return;
    // Its component is not a registered type: reverting it must not widen into "every component of the needle".
    AETHER_CHECK(RevertInstanceOverride(f.ctx, f.stack, f.needle, *vanished));
    AETHER_CHECK(f.OverrideCount() == 3 && f.GaugeOf(f.needle).label == "cold");

    rows = InstanceOverrides(f.ctx, f.needle);
    const InstanceOverride* pressure = Row(rows, 2, "pressure");
    AETHER_CHECK(pressure != nullptr && RevertInstanceOverride(f.ctx, f.stack, f.needle, *pressure));
    AETHER_CHECK(f.OverrideCount() == 2 && f.GaugeOf(f.needle).label == "cold" && f.GaugeOf(f.root).level == 7);
}

AETHER_TEST(InstanceOverrides_RevertingTheWholeInstanceIsOneUndoStep) {
    Fixture f;
    if (f.needle.IsNull()) return;
    AETHER_CHECK(RevertWholeInstance(f.ctx, f.stack, f.needle));
    AETHER_CHECK(f.OverrideCount() == 0 && f.GaugeOf(f.root).level == 3 && f.GaugeOf(f.needle).label == "tank");
    AETHER_CHECK(f.stack.UndoCount() == 1);
    AETHER_CHECK(!RevertWholeInstance(f.ctx, f.stack, f.needle)); // nothing left to revert
    AETHER_CHECK(f.stack.Undo(f.ctx));
    AETHER_CHECK(f.OverrideCount() == 3 && f.GaugeOf(f.root).level == 7 && f.GaugeOf(f.needle).label == "cold");
}

AETHER_TEST(InstanceOverrides_DrawingTheOpenListChangesNothing) {
    HeadlessImGui ui;
    Fixture f;
    if (f.needle.IsNull()) return;
    for (int i = 0; i < 3; ++i) {
        ui.Frame([&] {
            ImGui::GetStateStorage()->SetInt(ImGui::GetID("instance_overrides"), 1); // the list open, as after a click on it
            InspectEntity(f.ctx, f.stack, f.needle);
        });
    }
    AETHER_CHECK(f.stack.UndoCount() == 0 && f.OverrideCount() == 3);
    AETHER_CHECK(f.GaugeOf(f.root).level == 7 && f.GaugeOf(f.needle).label == "cold");
}
