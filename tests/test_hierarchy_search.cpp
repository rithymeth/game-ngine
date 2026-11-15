// The hierarchy's search: the filter grammar (bare and name: terms against names, type: against a
// component type), the tree keeping a match's ancestors on screen, and the panel drawing a filtered
// hierarchy without editing anything.

#include "world/scene_panel.h"

#include "aether/reflection/reflect_macros.h"
#include "aether/scene/components.h"
#include "aether/scene/hierarchy.h"
#include "test_framework.h"

#include <imgui.h>

#include <algorithm>
#include <filesystem>
#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

using namespace aether;
using namespace aether::editor;
namespace hsdfs = std::filesystem;

namespace hierarchy_test {

struct Marker {
    i32 level = 3;
};

} // namespace hierarchy_test

AETHER_REFLECT(hierarchy_test::Marker, 1, AETHER_FIELD(level, Field_EditAnywhere))

namespace {

class HeadlessImGui {
public:
    HeadlessImGui() {
        context_ = ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
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
        ImGui::SetNextWindowSize(ImVec2(1400, 900));
        ImGui::Begin("Scene", nullptr, ImGuiWindowFlags_NoMove);
        body();
        ImGui::End();
        ImGui::Render();
    }

private:
    ImGuiContext* context_ = nullptr;
};

hsdfs::path Scratch(const char* name) {
    const hsdfs::path dir = hsdfs::temp_directory_path() / name;
    std::error_code ignored;
    hsdfs::remove_all(dir, ignored);
    hsdfs::create_directories(dir);
    return dir;
}

bool Has(const std::vector<Entity>& list, Entity entity) {
    return std::find(list.begin(), list.end(), entity) != list.end();
}

} // namespace

AETHER_TEST(HierarchyQuery_ParsesBareNameAndTypeTokens) {
    AETHER_CHECK(ParseHierarchyQuery("").Empty());
    AETHER_CHECK(ParseHierarchyQuery(" \t  ").Empty());
    AETHER_CHECK(ParseHierarchyQuery("type:").Empty()); // an empty directive is no term
    AETHER_CHECK(ParseHierarchyQuery("name:").Empty());

    const HierarchyQuery bare = ParseHierarchyQuery("Door"); // lowercased, so matching can ignore case
    AETHER_CHECK(bare.name_terms == std::vector<std::string>{"door"});
    AETHER_CHECK(bare.type_terms.empty() && !bare.Empty());

    const HierarchyQuery mixed = ParseHierarchyQuery("NAME:Main type:MODEL  door"); // directives in any case
    AETHER_CHECK(mixed.name_terms == (std::vector<std::string>{"main", "door"}));
    AETHER_CHECK(mixed.type_terms == std::vector<std::string>{"model"});
}

AETHER_TEST(HierarchySearch_MatchesNamesAndComponentTypes) {
    SceneDocument document;
    document.NewScene();
    const Entity zebra = document.CreateEntity("Zebra");
    const Entity leaf = document.CreateEntity("Leaf B", SceneEntityKind::Empty, {}, zebra);
    document.GetWorld().AddComponent(zebra, hierarchy_test::Marker{7});

    // No filter: everything, as before the grammar existed.
    AETHER_CHECK(MatchingEntities(document, "").size() == document.Entities().size());

    const std::vector<Entity> leaves = MatchingEntities(document, "LeAf");
    AETHER_CHECK(leaves.size() == 1 && leaves[0] == leaf);
    AETHER_CHECK(MatchingEntities(document, "nothing like it").empty());

    // type: matches any component's type name, case-insensitively and by substring.
    const std::vector<Entity> marked = MatchingEntities(document, "type:marker");
    AETHER_CHECK(marked.size() == 1 && marked[0] == zebra);
    AETHER_CHECK(MatchingEntities(document, "TYPE:MARK") == marked);

    // Terms all have to match: the name term fails on the marked entity, the type term on the other.
    AETHER_CHECK(MatchingEntities(document, "type:marker leaf").empty());
    AETHER_CHECK(MatchingEntities(document, "type:marker zeb").size() == 1);

    // Every entity made through the document has a Transform.
    const std::vector<Entity> transformed = MatchingEntities(document, "type:transform");
    AETHER_CHECK(Has(transformed, zebra) && Has(transformed, leaf));
}

AETHER_TEST(HierarchySearch_KeepsTheAncestorsOfAMatchVisible) {
    SceneDocument document;
    document.NewScene();
    const Entity root = document.CreateEntity("Aardvark");
    const Entity middle = document.CreateEntity("Beetle", SceneEntityKind::Empty, {}, root);
    const Entity leaf = document.CreateEntity("Caterpillar", SceneEntityKind::Empty, {}, middle);
    const Entity other = document.CreateEntity("Dragonfly");
    document.GetWorld().AddComponent(leaf, hierarchy_test::Marker{1});
    document.GetWorld().AddComponent(other, hierarchy_test::Marker{2});

    const std::vector<Entity> matches = MatchingEntities(document, "caterpillar");
    AETHER_CHECK(matches.size() == 1 && matches[0] == leaf);
    const std::unordered_set<Entity> shown = VisibleEntities(document, matches);
    AETHER_CHECK(shown.size() == 3); // the match and its two ancestors
    AETHER_CHECK(shown.count(leaf) == 1 && shown.count(middle) == 1 && shown.count(root) == 1);
    AETHER_CHECK(shown.count(other) == 0); // an unrelated root stays out

    // Two matches in different chains: both chains, and nothing in between.
    const std::unordered_set<Entity> both = VisibleEntities(document, MatchingEntities(document, "type:marker"));
    AETHER_CHECK(both.size() == 4);
    AETHER_CHECK(both.count(root) == 1 && both.count(middle) == 1 && both.count(leaf) == 1 && both.count(other) == 1);
}

AETHER_TEST(ScenePanel_AFilteredHierarchyDrawsWithoutEditing) {
    HeadlessImGui imgui;
    ScenePanel panel(Scratch("aether_hierarchy_search_panel"));
    SceneDocument& document = panel.Document();
    const Entity zebra = document.CreateEntity("Zebra");
    const Entity leaf = document.CreateEntity("Leaf B", SceneEntityKind::Empty, {}, zebra);
    document.GetWorld().AddComponent(zebra, hierarchy_test::Marker{1});
    auto frame = [&] { imgui.Frame([&] { panel.Draw(); }); };
    frame();
    frame();
    const usize entities = document.Entities().size();
    const usize undo = document.Commands().UndoCount();

    panel.SetHierarchyFilter("type:marker");
    AETHER_CHECK(panel.HierarchyFilter() == "type:marker");
    frame(); // the match and its ancestors draw; the leaf stays on screen under Zebra
    frame();
    AETHER_CHECK(document.Commands().UndoCount() == undo); // filtering is not an edit
    AETHER_CHECK(document.Entities().size() == entities);
    AETHER_CHECK(document.Selection().empty());

    panel.SetHierarchyFilter("nothing like it");
    frame(); // no matches: the empty state
    AETHER_CHECK(document.Commands().UndoCount() == undo);

    // The filtered tree is still a tree: selecting and editing work after the filter is cleared.
    panel.SetHierarchyFilter("");
    document.SetSelection({leaf});
    frame();
    AETHER_CHECK(document.Commands().UndoCount() == undo);
}
