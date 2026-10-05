#include "test_framework.h"

#include "gameplay/gameplay_debugger_document.h"
#include "gameplay/gameplay_debugger_panel.h"
#include "workspace/editor_workspace.h"

#include "aether/scene/components.h"

#include <imgui.h>

#include <cmath>
#include <functional>

// Phase 30 step 6 (§30.7): the Gameplay Debugger's rows, filter, actions and panel.

using namespace aether;
using namespace aether::gas;
using namespace aether::editor;

#define CHECK(...) AETHER_CHECK((__VA_ARGS__))

namespace {

bool Near(f32 a, f32 b, f32 eps = 1e-3f) { return std::fabs(a - b) <= eps; }

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

struct Rig {
    World world;
    AttributeSystem attrs{world};
    EffectLibrary effects;
    EffectSystem fx{world, attrs, effects};
    AbilityLibrary abilities;
    AbilitySystem ab{world, attrs, fx, abilities, effects};
    GameplayDebuggerDocument doc;
    Entity hero, other;

    Rig() {
        GameplayEffect haste;
        haste.name = "Haste";
        haste.duration_policy = GameplayEffect::Duration::Infinite;
        haste.modifiers = {{"Speed", GameplayEffect::Op::Multiply, 2.0f}};
        haste.granted_tags = {GameplayTag::Make("State.Hasted")};
        GameplayEffect burn;
        burn.name = "Burning";
        burn.duration_policy = GameplayEffect::Duration::Timed;
        burn.duration = 5.0f;
        effects.Register(haste);
        effects.Register(burn);
        GameplayAbility dash;
        dash.name = "Dash";
        abilities.Register(dash);
        hero = world.CreateEntity(Transform{Vec3(), Quaternion::Identity()});
        other = world.CreateEntity(Transform{Vec3(), Quaternion::Identity()});
        attrs.Define(hero, "Health", 100, 0, 100);
        attrs.Define(hero, "Speed", 5);
        attrs.Define(other, "Mana", 30);
        fx.Apply(hero, "Haste");
        fx.Apply(hero, "Burning");
        ab.Grant(hero, "Dash");
        ab.TryActivate(hero, "Dash");
        doc.SetWorld(&world);
        doc.SetLibraries(&effects, &abilities);
        doc.SetSystems(&attrs, &fx, &ab);
    }
};

} // namespace

AETHER_TEST(GameplayDebugger_RowsAreCompleteAndOrdered) {
    Rig r;
    r.doc.Rebuild();
    const auto& e = r.doc.Entities();
    CHECK(e.size() == 2 && r.doc.TotalEntities() == 2 && e[0].entity == r.hero && e[1].entity == r.other); // by entity index
    CHECK(e[0].attributes.size() == 2 && e[0].attributes[0].name == "Health" && e[0].attributes[1].name == "Speed"); // by name
    const DebugAttributeRow& speed = e[0].attributes[1];
    CHECK(Near(speed.base, 5) && Near(speed.current, 10) && Near(speed.mul, 2) && speed.Modified() && !e[0].attributes[0].Modified());
    CHECK(e[0].effects.size() == 2 && e[0].effects[0].effect == "Haste" && e[0].effects[0].policy == "infinite" && e[0].effects[1].policy == "timed" && e[0].effects[1].known);
    CHECK(Near(e[0].effects[1].remaining, 5));
    CHECK(e[0].abilities.size() == 1 && e[0].abilities[0].name == "Dash" && e[0].abilities[0].active && e[0].abilities[0].committed);
    CHECK(e[0].tags.size() == 1 && e[0].tags[0].tag == "State.Hasted" && e[0].tags[0].count == 1);
    CHECK(e[1].attributes.size() == 1 && e[1].effects.empty() && e[1].label == "Entity " + std::to_string(r.other.index));
    // The same world gives the same rows.
    GameplayDebuggerDocument again;
    again.SetWorld(&r.world);
    again.Rebuild();
    CHECK(again.Entities().size() == 2 && again.Entities()[0].effects.size() == 2 && again.Entities()[0].effects[0].effect == "Haste");
    // Without the libraries an effect is shown by its id, kind unknown.
    GameplayDebuggerDocument bare;
    bare.SetWorld(&r.world);
    bare.Rebuild();
    CHECK(!bare.Entities()[0].effects[0].known && bare.Entities()[0].effects[0].policy == "?");
    // No world, no rows.
    GameplayDebuggerDocument none;
    none.Rebuild();
    CHECK(none.Entities().empty());
}

AETHER_TEST(GameplayDebugger_FilterAndSelection) {
    Rig r;
    r.doc.SetFilter("SPEED");
    r.doc.Rebuild();
    CHECK(r.doc.Entities().size() == 1 && r.doc.Entities()[0].attributes.size() == 1 && r.doc.Entities()[0].effects.empty() && r.doc.TotalEntities() == 2);
    r.doc.SetFilter("hasted");
    r.doc.Rebuild();
    CHECK(r.doc.Entities().size() == 1 && r.doc.Entities()[0].tags.size() == 1 && r.doc.Entities()[0].attributes.empty());
    r.doc.SetFilter("zzz");
    r.doc.Rebuild();
    CHECK(r.doc.Entities().empty());
    r.doc.SetFilter("Entity " + std::to_string(r.other.index)); // an entity label keeps all its rows
    r.doc.Rebuild();
    CHECK(r.doc.Entities().size() == 1 && r.doc.Entities()[0].entity == r.other && r.doc.Entities()[0].attributes.size() == 1);
    r.doc.SetFilter("");
    r.doc.Select(r.other);
    r.doc.Rebuild();
    CHECK(r.doc.Entities().size() == 1 && r.doc.Entities()[0].entity == r.other && r.doc.TotalEntities() == 1);
}

AETHER_TEST(GameplayDebugger_ActionsGoThroughTheSystems) {
    Rig r;
    r.attrs.ClearEvents();
    CHECK(r.doc.SetBase(r.hero, "Health", 40).find("Set Health") == 0 && Near(r.attrs.Get(r.hero, "Health"), 40) && r.attrs.Events().size() == 1);
    CHECK(r.doc.SetBase(r.hero, "Health", 500).find("Set Health") == 0 && Near(r.attrs.Get(r.hero, "Health"), 100)); // clamped
    CHECK(r.doc.SetBase(r.hero, "Nope", 1).find("no attribute") != std::string::npos && r.doc.SetBase(Entity{}, "Health", 1) == "No such entity.");
    r.doc.Rebuild();
    const u32 haste = r.doc.Entities()[0].effects[0].handle;
    CHECK(r.doc.RemoveEffect(r.hero, haste) == "Removed the effect." && Near(r.attrs.Get(r.hero, "Speed"), 5)); // the modifier went with it
    CHECK(r.doc.RemoveEffect(r.hero, haste) == "No such effect.");
    const u32 dash = [&] { r.doc.Rebuild(); return r.doc.Entities()[0].abilities[0].handle; }();
    CHECK(r.doc.CancelAbility(r.hero, dash) == "Cancelled the ability." && !r.ab.IsActive(r.hero, "Dash") && r.doc.CancelAbility(r.hero, dash) == "No such running ability.");
    CHECK(r.doc.AddTag(r.other, "State.Stunned") == "Added State.Stunned." && r.doc.AddTag(r.other, "bad tag!").find("isn't a valid") != std::string::npos);
    CHECK(r.doc.RemoveTag(r.other, "State.Stunned").find("Removed") == 0 && r.doc.RemoveTag(r.other, "State.Stunned").find("doesn't have") != std::string::npos);
}

AETHER_TEST(GameplayDebugger_ActionsEditComponentsWithoutSystems) {
    Rig r;
    r.doc.SetSystems(nullptr, nullptr, nullptr);
    CHECK(r.doc.SetBase(r.hero, "Health", 25).find("Set Health") == 0 && Near(r.world.GetComponent<AttributeSet>(r.hero)->Get("Health"), 25));
    r.doc.Rebuild();
    CHECK(r.doc.RemoveEffect(r.hero, r.doc.Entities()[0].effects[1].handle).find("Removed the effect") == 0 && r.world.GetComponent<EffectContainer>(r.hero)->active.size() == 1);
    CHECK(r.doc.CancelAbility(r.hero, r.doc.Entities()[0].abilities[0].handle).find("Cancelled") == 0 && r.world.GetComponent<AbilityContainer>(r.hero)->active.empty());
}

AETHER_TEST(GameplayDebugger_PanelDrawsInEveryState) {
    HeadlessImGui imgui;
    GameplayDebuggerDocument none;
    GameplayDebuggerPanel no_world(none);
    imgui.Frame([&] { no_world.Draw(); }); // "No world"
    Rig r;
    GameplayDebuggerPanel panel(r.doc);
    imgui.Frame([&] { panel.Draw(); });
    r.doc.SetFilter("nothing matches this");
    imgui.Frame([&] { panel.Draw(); });
    r.doc.SetFilter("");
    r.world.DestroyEntity(r.other);
    imgui.Frame([&] { panel.Draw(); }); // an entity went away between frames
    r.doc.Select(r.other);
    imgui.Frame([&] { panel.Draw(); });
    CHECK(r.doc.Entities().empty());
}

AETHER_TEST(GameplayDebugger_WorkspaceHasTheTool) {
    EditorWorkspace ws;
    const i64 tool = ws.FindTool("Gameplay Debugger");
    CHECK(tool >= 0 && std::string(ws.ToolCategory(static_cast<usize>(tool))) == "Debug");
    HeadlessImGui imgui;
    imgui.Frame([&] { ws.DrawTool(static_cast<usize>(tool)); }); // the built-in sample world
    World other;
    ws.SetGameplayWorld(&other);
    imgui.Frame([&] { ws.DrawTool(static_cast<usize>(tool)); });
    ws.SetGameplayWorld(nullptr);
    imgui.Frame([&] { ws.DrawTool(static_cast<usize>(tool)); });
}
