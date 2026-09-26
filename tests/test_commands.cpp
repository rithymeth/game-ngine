// Tests for the editor's built-in undoable commands (editor/src/core/
// commands.h) and the undoable entity Inspector (editor/src/ui/
// entity_inspector.h), headless.

#include "aether/scene/entity_guid.h"
#include "core/commands.h"
#include "test_framework.h"
#include "ui/entity_inspector.h"

#include <imgui.h>

#include <string>

using namespace aether;
using namespace aether::editor;

namespace cmd_test {

struct Health {
    f32 current = 100.0f;
    std::string name = "unit";
};

struct Armor {
    i32 rating = 5;
};

// Not reflected: must still survive delete + undo through its raw bytes.
struct RawData {
    u32 bits = 0;
};

} // namespace cmd_test

AETHER_REFLECT(cmd_test::Health, 1,
    AETHER_FIELD(current, Field_EditAnywhere, {.range_min = 0, .range_max = 1000}),
    AETHER_FIELD(name, Field_EditAnywhere)
)
AETHER_REFLECT(cmd_test::Armor, 1, AETHER_FIELD(rating, Field_EditAnywhere))

namespace {

struct HookLog {
    int created = 0, destroying = 0, added = 0, removing = 0, field_changed = 0;
    std::string last_field;
};

struct Fixture {
    World world;
    GuidIndex guids;
    HookLog log;
    EditorHooks hooks;
    CommandContext ctx{world, guids, &hooks};
    CommandStack stack;

    Fixture() {
        hooks.on_entity_created = [this](Entity) { ++log.created; };
        hooks.on_entity_destroying = [this](Entity) { ++log.destroying; };
        hooks.on_component_added = [this](Entity, ComponentId) { ++log.added; };
        hooks.on_component_removing = [this](Entity, ComponentId) { ++log.removing; };
        hooks.on_field_changed = [this](Entity, ComponentId, const reflect::FieldInfo& f) {
            ++log.field_changed;
            log.last_field = f.name;
        };
    }

    Entity Live(const EntityGuid& guid) { return guids.Find(world, guid); }
};

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

} // namespace

AETHER_TEST(Commands_SetFieldUndoRedoAndMerge) {
    Fixture f;
    Entity e = f.world.CreateEntity(cmd_test::Health{});
    EntityGuid guid = EnsureGuid(f.world, e, &f.guids);
    const ComponentId health = GetComponentId<cmd_test::Health>();
    const reflect::FieldInfo& current = *reflect::Reflect<cmd_test::Health>().FindField("current");

    for (int v = 90; v >= 50; v -= 10) { // one "drag"
        f32 old_value = f.world.GetComponent<cmd_test::Health>(f.Live(guid))->current;
        f.stack.Execute(f.ctx,
                        std::make_unique<SetFieldCommand>(guid, health, current, reflect::Any(old_value),
                                                          reflect::Any(static_cast<f32>(v))),
                        MergePolicy::Allow);
    }
    AETHER_CHECK(f.stack.UndoCount() == 1);
    AETHER_CHECK(f.stack.UndoLabel() == "Edit Health.current");
    AETHER_CHECK(f.world.GetComponent<cmd_test::Health>(e)->current == 50.0f);
    AETHER_CHECK(f.log.field_changed == 5 && f.log.last_field == "current");

    f.stack.Undo(f.ctx);
    AETHER_CHECK(f.world.GetComponent<cmd_test::Health>(e)->current == 100.0f);
    AETHER_CHECK(f.log.field_changed == 6); // hooks fire on undo too
    f.stack.Redo(f.ctx);
    AETHER_CHECK(f.world.GetComponent<cmd_test::Health>(e)->current == 50.0f);
}

AETHER_TEST(Commands_DestroyAndUndoRestoresEveryComponent) {
    Fixture f;
    Entity e = f.world.CreateEntity(cmd_test::Health{42.0f, "a name long enough for the heap, not SSO"},
                                    cmd_test::RawData{0xBEEF});
    EntityGuid guid = EnsureGuid(f.world, e, &f.guids);

    f.stack.Execute(f.ctx, std::make_unique<DestroyEntityCommand>(guid));
    AETHER_CHECK(f.Live(guid).IsNull() && f.world.EntityCount() == 0);
    AETHER_CHECK(f.log.destroying == 1);

    f.stack.Undo(f.ctx);
    Entity restored = f.Live(guid);
    AETHER_CHECK(!restored.IsNull());
    AETHER_CHECK(f.world.GetComponent<cmd_test::Health>(restored)->current == 42.0f);
    AETHER_CHECK(f.world.GetComponent<cmd_test::Health>(restored)->name == "a name long enough for the heap, not SSO");
    AETHER_CHECK(f.world.GetComponent<cmd_test::RawData>(restored)->bits == 0xBEEF); // unreflected, raw bytes
    AETHER_CHECK(f.log.created == 1);

    f.stack.Redo(f.ctx);
    AETHER_CHECK(f.Live(guid).IsNull());
}

AETHER_TEST(Commands_CreateFromExistingEntityRecordsWithoutDoubleCreating) {
    Fixture f;
    Entity spawned = f.world.CreateEntity(cmd_test::Armor{9}); // spawned by other code
    f.stack.Record(CreateEntityCommand::FromExisting(f.ctx, spawned, "Spawn Armor"));
    AETHER_CHECK(f.world.EntityCount() == 1); // Record doesn't call Do
    AETHER_CHECK(f.stack.UndoLabel() == "Spawn Armor");
    EntityGuid guid = f.world.GetComponent<IdComponent>(spawned)->guid;

    f.stack.Undo(f.ctx);
    AETHER_CHECK(f.world.EntityCount() == 0);
    f.stack.Redo(f.ctx);
    AETHER_CHECK(f.world.EntityCount() == 1);
    Entity again = f.Live(guid);
    AETHER_CHECK(!again.IsNull() && f.world.GetComponent<cmd_test::Armor>(again)->rating == 9);
}

AETHER_TEST(Commands_AddAndRemoveComponent) {
    Fixture f;
    Entity e = f.world.CreateEntity(cmd_test::Health{});
    EntityGuid guid = EnsureGuid(f.world, e, &f.guids);
    const ComponentId armor = GetComponentId<cmd_test::Armor>();

    f.stack.Execute(f.ctx, std::make_unique<AddComponentCommand>(guid, armor));
    AETHER_CHECK(f.world.HasComponentRaw(f.Live(guid), armor));
    AETHER_CHECK(f.stack.UndoLabel() == "Add Armor" && f.log.added == 1);
    f.world.GetComponent<cmd_test::Armor>(f.Live(guid))->rating = 77;

    f.stack.Execute(f.ctx, std::make_unique<RemoveComponentCommand>(guid, armor));
    AETHER_CHECK(!f.world.HasComponentRaw(f.Live(guid), armor));
    f.stack.Undo(f.ctx); // restores with its data
    AETHER_CHECK(f.world.GetComponent<cmd_test::Armor>(f.Live(guid))->rating == 77);
    f.stack.Undo(f.ctx); // undo the add
    AETHER_CHECK(!f.world.HasComponentRaw(f.Live(guid), armor));
    AETHER_CHECK(f.world.GetComponent<cmd_test::Health>(f.Live(guid))->current == 100.0f);
}

AETHER_TEST(Commands_EnsureAllGuidsFixesMissingAndDuplicates) {
    World world;
    GuidIndex guids;
    EntityGuid shared = NewEntityGuid();
    world.CreateEntity(IdComponent{shared});
    world.CreateEntity(IdComponent{shared}); // duplicate
    world.CreateEntity(cmd_test::Armor{});   // missing
    world.CreateEntity(IdComponent{});       // present but null
    AETHER_CHECK(EnsureAllGuids(world, guids) == 3);
    AETHER_CHECK(guids.Size() == 4);
    AETHER_CHECK(EnsureAllGuids(world, guids) == 0); // idempotent
}

AETHER_TEST(EntityInspector_EditsBecomeUndoableCommands) {
    HeadlessImGui ui;
    Fixture f;
    Entity e = f.world.CreateEntity(cmd_test::Health{});
    auto frame = [&](bool focus_first_field) {
        ui.Frame([&] {
            if (focus_first_field) {
                ImGui::SetKeyboardFocusHere(1); // widget 0 is the collapsing header
            }
            InspectEntity(f.ctx, f.stack, e); // adding components migrates storage but keeps the handle
        });
    };
    frame(false); // first frame gives the entity a GUID (moves it to a new archetype)
    EntityGuid guid = f.world.GetComponent<IdComponent>(e)->guid;
    AETHER_CHECK(!guid.IsNull() && f.stack.UndoCount() == 0);

    // Type a value into Health.current and press Enter.
    frame(true);
    frame(false);
    ImGui::GetIO().AddInputCharactersUTF8("250");
    frame(false);
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, true);
    frame(false);
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, false);
    frame(false);

    Entity live = f.Live(guid);
    AETHER_CHECK(f.world.GetComponent<cmd_test::Health>(live)->current == 250.0f);
    AETHER_CHECK(f.stack.UndoCount() == 1);
    AETHER_CHECK(f.stack.UndoLabel() == "Edit Health.current");
    AETHER_CHECK(f.log.field_changed >= 1); // the edit went through the command (and its hook)

    f.stack.Undo(f.ctx);
    AETHER_CHECK(f.world.GetComponent<cmd_test::Health>(live)->current == 100.0f);
    frame(false); // drawing after undo shows the restored value and records nothing new
    AETHER_CHECK(f.stack.UndoCount() == 0 && f.stack.RedoCount() == 1);
}
