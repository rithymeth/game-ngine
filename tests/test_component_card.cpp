// The Inspector's component card menu (parity Stage 4): copying a whole component, pasting its values into the same
// component elsewhere, and resetting a component — each one undo step, on one entity or a selection. Headless.

#include "aether/assets/asset_guid.h"
#include "aether/reflection/serialize.h"
#include "aether/scene/components.h"
#include "aether/scene/entity_guid.h"
#include "aether/scene/prefab.h"
#include "core/commands.h"
#include "ui/entity_inspector.h"
#include "ui/reflected_inspector.h"
#include "test_framework.h"

#include <imgui.h>

#include <algorithm>
#include <string>
#include <vector>

using namespace aether;
using namespace aether::editor;

namespace card_test {

struct Gauge {
    i32 level = 3;
    std::string label = "tank";
    f32 readout = 0.5f; // shown read-only, kept in step with level
    i32 cache = 11;     // hidden runtime state
};

struct Valve {
    i32 level = 3;
};

} // namespace card_test

AETHER_REFLECT(card_test::Gauge, 1,
    AETHER_FIELD(level, Field_EditAnywhere),
    AETHER_FIELD(label, Field_EditAnywhere),
    AETHER_FIELD(readout, Field_ReadOnly),
    AETHER_FIELD(cache, Field_None)
)
AETHER_REFLECT(card_test::Valve, 1, AETHER_FIELD(level, Field_EditAnywhere))

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

struct Fixture {
    World world;
    GuidIndex guids;
    EditorHooks hooks;
    CommandContext ctx{world, guids, &hooks};
    CommandStack stack;

    Fixture() { ClearObjectClipboard(); }
    ~Fixture() { ClearObjectClipboard(); }

    Entity Spawn(card_test::Gauge gauge) {
        const Entity entity = world.CreateEntity(gauge);
        EnsureGuid(world, entity, &guids);
        return entity;
    }
    card_test::Gauge& Gauge(Entity entity) { return *world.GetComponent<card_test::Gauge>(entity); }
};

ComponentId kGauge() { return GetComponentId<card_test::Gauge>(); }

} // namespace

AETHER_TEST(ComponentCard_PasteCopiesEveryShownFieldAsOneUndoStep) {
    Fixture f;
    const Entity source = f.Spawn({.level = 9, .label = "boiler", .readout = 0.9f, .cache = 99});
    const Entity target = f.Spawn({});
    CopyComponent(f.world, source, kGauge());
    AETHER_CHECK(ObjectClipboardFits(reflect::Reflect<card_test::Gauge>()));
    f.Gauge(source).level = 1; // the clipboard owns its copy

    const Entity one[] = {target};
    AETHER_CHECK(PasteComponentValues(f.ctx, f.stack, one, kGauge()));
    AETHER_CHECK(f.Gauge(target).level == 9 && f.Gauge(target).label == "boiler");
    AETHER_CHECK(f.Gauge(target).readout == 0.9f); // read-only data travels with what it follows
    AETHER_CHECK(f.Gauge(target).cache == 11);     // hidden runtime state is never written
    AETHER_CHECK(f.stack.UndoCount() == 1);
    AETHER_CHECK(f.stack.UndoLabel() == std::string("Paste ") + reflect::Reflect<card_test::Gauge>().name + " values");

    AETHER_CHECK(!PasteComponentValues(f.ctx, f.stack, one, kGauge())); // nothing differs: no step
    AETHER_CHECK(f.stack.UndoCount() == 1);

    AETHER_CHECK(f.stack.Undo(f.ctx));
    AETHER_CHECK(f.Gauge(target).level == 3 && f.Gauge(target).label == "tank" && f.Gauge(target).readout == 0.5f);
    AETHER_CHECK(f.stack.Redo(f.ctx));
    AETHER_CHECK(f.Gauge(target).level == 9 && f.Gauge(target).readout == 0.9f);
}

AETHER_TEST(ComponentCard_PasteIsRefusedForAnotherTypeOrAMissingComponent) {
    Fixture f;
    const Entity gauge = f.Spawn({.level = 4});
    const Entity valve = f.world.CreateEntity(card_test::Valve{.level = 8});
    EnsureGuid(f.world, valve, &f.guids);

    const Entity valves[] = {valve};
    AETHER_CHECK(!PasteComponentValues(f.ctx, f.stack, valves, GetComponentId<card_test::Valve>())); // empty clipboard

    CopyComponent(f.world, gauge, kGauge());
    AETHER_CHECK(!ObjectClipboardFits(reflect::Reflect<card_test::Valve>()));
    AETHER_CHECK(!PasteComponentValues(f.ctx, f.stack, valves, GetComponentId<card_test::Valve>())); // another type
    AETHER_CHECK(f.world.GetComponent<card_test::Valve>(valve)->level == 8);

    // The valve lacks a Gauge: refused for the whole selection, including the entity that has one.
    const Entity other = f.Spawn({.level = 1});
    const Entity mixed[] = {other, valve};
    AETHER_CHECK(!PasteComponentValues(f.ctx, f.stack, mixed, kGauge()));
    AETHER_CHECK(f.Gauge(other).level == 1);
    AETHER_CHECK(f.stack.UndoCount() == 0);

    // Copying from an entity without the component leaves the clipboard as it was.
    CopyComponent(f.world, valve, kGauge());
    AETHER_CHECK(ObjectClipboardFits(reflect::Reflect<card_test::Gauge>()));
}

AETHER_TEST(ComponentCard_ResetAcrossASelectionUndoesToEachOwnValue) {
    Fixture f;
    const Entity a = f.Spawn({.level = 7, .label = "a", .readout = 0.7f, .cache = 1});
    const Entity b = f.Spawn({.level = 2, .label = "tank", .readout = 0.2f, .cache = 2});
    const Entity selection[] = {a, b};
    AETHER_CHECK(ResetComponent(f.ctx, f.stack, selection, kGauge()));
    for (const Entity entity : selection) {
        AETHER_CHECK(f.Gauge(entity).level == 3 && f.Gauge(entity).label == "tank" && f.Gauge(entity).readout == 0.5f);
    }
    AETHER_CHECK(f.Gauge(a).cache == 1 && f.Gauge(b).cache == 2);
    AETHER_CHECK(f.stack.UndoCount() == 1);
    AETHER_CHECK(f.stack.UndoLabel() ==
                 std::string("Reset ") + reflect::Reflect<card_test::Gauge>().name + " on 2 entities");
    AETHER_CHECK(!ResetComponent(f.ctx, f.stack, selection, kGauge())); // already defaults

    AETHER_CHECK(f.stack.Undo(f.ctx));
    AETHER_CHECK(f.Gauge(a).level == 7 && f.Gauge(a).label == "a" && f.Gauge(a).readout == 0.7f);
    AETHER_CHECK(f.Gauge(b).level == 2 && f.Gauge(b).label == "tank" && f.Gauge(b).readout == 0.2f);
}

AETHER_TEST(ComponentCard_PastingIntoAPrefabInstanceRecordsOverrides) {
    Fixture f;
    (void)kGauge(); // the prefab names the component, so it must be registered before instantiating
    const char* component_name = reflect::Reflect<card_test::Gauge>().name;
    PrefabData prefab;
    PrefabEntity root;
    root.id = 1;
    root.components[component_name] = reflect::ToJson(card_test::Gauge{});
    prefab.entities.push_back(root);
    const assets::AssetGuid source = assets::NewAssetGuid();
    f.ctx.find_prefab = [&](const assets::AssetGuid& guid) { return guid == source ? &prefab : nullptr; };
    const Entity instance = InstantiatePrefab(f.world, f.guids, source, prefab);
    const auto overrides = [&] { return f.world.GetComponent<PrefabInstance>(instance)->overrides; };
    AETHER_CHECK(overrides().empty());

    const Entity donor = f.Spawn({.level = 12, .label = "tank"});
    CopyComponent(f.world, donor, kGauge());
    const Entity one[] = {instance};
    AETHER_CHECK(PasteComponentValues(f.ctx, f.stack, one, kGauge()));
    AETHER_CHECK(f.Gauge(instance).level == 12);
    const auto recorded = overrides();
    AETHER_CHECK(std::any_of(recorded.begin(), recorded.end(), [&](const PropertyOverride& o) {
        return o.component == component_name && o.field_path == "level";
    }));
    AETHER_CHECK(std::none_of(recorded.begin(), recorded.end(), [](const PropertyOverride& o) {
        return o.field_path == "label"; // equal to the prefab's: pasted over nothing, so no override
    }));

    AETHER_CHECK(f.stack.Undo(f.ctx));
    AETHER_CHECK(overrides().empty() && f.Gauge(instance).level == 3);
}

AETHER_TEST(ComponentCard_DrawingTheCardsChangesNothing) {
    HeadlessImGui ui;
    Fixture f;
    const Entity a = f.Spawn({.level = 5});
    const Entity b = f.Spawn({.level = 6});
    CopyComponent(f.world, a, kGauge());
    const std::vector<Entity> selection{a, b};
    for (int i = 0; i < 3; ++i) {
        ui.Frame([&] { InspectEntity(f.ctx, f.stack, a); });
        ui.Frame([&] { InspectEntities(f.ctx, f.stack, selection); });
    }
    AETHER_CHECK(f.Gauge(a).level == 5 && f.Gauge(b).level == 6); // a menu applies when picked, not when drawn
    AETHER_CHECK(f.stack.UndoCount() == 0);
}
