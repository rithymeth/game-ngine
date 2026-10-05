#include "test_framework.h"

#include "aether/blueprint/compiler.h"
#include "aether/blueprint/vm.h"
#include "aether/gameplay/tag_container.h"
#include "aether/gameplay/tag_library.h"
#include "aether/gameplay/tag_query.h"
#include "aether/ecs/world.h"
#include "aether/reflection/registry.h"
#include "aether/reflection/serialize.h"
#include "aether/scene/components.h"

// Phase 30 step 1 (§30.1): gameplay tags, containers, queries.

using namespace aether;
using namespace aether::gas;

#define CHECK(...) AETHER_CHECK((__VA_ARGS__))

namespace {
GameplayTag T(const char* n) { return GameplayTag::Make(n); }
} // namespace

AETHER_TEST(GameplayTag_NamesAreValidated) {
    for (const char* ok : {"State", "State.Stunned", "Damage.Fire.Burning", "a_b.C3", "X"}) CHECK(GameplayTag::ValidName(ok) && T(ok).IsValid());
    for (const char* bad : {"", ".", ".State", "State.", "State..Stunned", "State Stunned", "State-Stunned", "Stät", "a.b c"}) {
        CHECK(!GameplayTag::ValidName(bad));
        CHECK(!T(bad).IsValid() && T(bad).name.empty());
    }
    CHECK(!GameplayTag::ValidName(std::string(300, 'a')));
}

AETHER_TEST(GameplayTag_ParentAndDepth) {
    CHECK(T("State.Stunned.Frozen").Parent().name == "State.Stunned");
    CHECK(T("State.Stunned").Parent().name == "State");
    CHECK(T("State").Parent().name.empty() && GameplayTag{}.Parent().name.empty());
    CHECK(T("State").Depth() == 1 && T("State.Stunned").Depth() == 2 && T("a.b.c.d").Depth() == 4 && GameplayTag{}.Depth() == 0);
}

AETHER_TEST(GameplayTag_MatchingIsHierarchical) {
    CHECK(T("Damage.Fire.Burning").Matches(T("Damage.Fire")) && T("Damage.Fire.Burning").Matches(T("Damage")));
    CHECK(T("Damage.Fire").Matches(T("Damage.Fire")));
    CHECK(!T("Damage").Matches(T("Damage.Fire"))); // a parent doesn't match its child
    // The prefix traps.
    CHECK(!T("State.Stunned").Matches(T("State.Stun")) && !T("StateX").Matches(T("State")) && !T("State2.Foo").Matches(T("State")));
    CHECK(!T("Damage.Fire2").Matches(T("Damage.Fire")));
    // Exact.
    CHECK(T("Damage.Fire").MatchesExact(T("Damage.Fire")) && !T("Damage.Fire.Burning").MatchesExact(T("Damage.Fire")));
    // The empty tag matches nothing, and nothing matches it.
    CHECK(!GameplayTag{}.Matches(GameplayTag{}) && !T("a").Matches(GameplayTag{}) && !GameplayTag{}.Matches(T("a")) && !GameplayTag{}.MatchesExact(GameplayTag{}));
}

AETHER_TEST(TagContainer_CountsAreReferenceCounts) {
    TagContainer c;
    CHECK(c.Empty() && !c.HasTag(T("State.Stunned")));
    CHECK(c.Add(T("State.Stunned")) && c.Add(T("State.Stunned")) && c.Count(T("State.Stunned")) == 2 && c.Size() == 1);
    CHECK(c.Remove(T("State.Stunned")) && c.HasTag(T("State.Stunned")) && c.Count(T("State.Stunned")) == 1); // one source left
    CHECK(c.Remove(T("State.Stunned")) && !c.HasTag(T("State.Stunned")) && c.Empty());
    CHECK(!c.Remove(T("State.Stunned")));                     // not there
    CHECK(!c.Add(GameplayTag{}) && !c.Add(T("a"), 0) && !c.Add(T("a"), -1) && c.Empty()); // invalid
    CHECK(c.Add(T("Buff.Haste"), 3) && c.Count(T("Buff.Haste")) == 3);
    CHECK(c.Remove(T("Buff.Haste"), 10) && c.Empty()); // removing more than it has removes it
    c.Add(T("a"), 5);
    CHECK(c.RemoveAll(T("a")) && !c.RemoveAll(T("a")) && c.Empty());
    CHECK(!c.Remove(T("a"), 0));
}

AETHER_TEST(TagContainer_QueriesAreHierarchicalAndSorted) {
    TagContainer c;
    c.Add(T("State.Stunned"));
    c.Add(T("Damage.Fire.Burning"));
    c.Add(T("Buff.Haste"));
    CHECK((c.Tags() == std::vector<GameplayTag>{T("Buff.Haste"), T("Damage.Fire.Burning"), T("State.Stunned")})); // sorted
    CHECK(c.HasTag(T("Damage.Fire")) && c.HasTag(T("Damage")) && c.HasTag(T("Damage.Fire.Burning")));
    CHECK(!c.HasTag(T("Damage.Fire.Burning.Intense")) && !c.HasTag(T("Damage.Ice")) && !c.HasTag(GameplayTag{}));
    CHECK(c.HasTagExact(T("Damage.Fire.Burning")) && !c.HasTagExact(T("Damage.Fire")));
    CHECK(!c.HasTag(T("State.Stun"))); // not a segment boundary
    const std::vector<GameplayTag> any{T("Nope"), T("Buff")};
    const std::vector<GameplayTag> all{T("Buff"), T("Damage.Fire")};
    const std::vector<GameplayTag> all_missing{T("Buff"), T("Nope")};
    const std::vector<GameplayTag> none;
    CHECK(c.HasAny(any) && c.HasAll(all) && !c.HasAll(all_missing) && !c.HasNone(any) && c.HasNone(all_missing) == false);
    CHECK(!c.HasAny(none) && c.HasAll(none) && c.HasNone(none)); // an empty list
    TagContainer empty;
    CHECK(!empty.HasAny(any) && !empty.HasAll(all) && empty.HasNone(any) && empty.HasAll(none));
}

AETHER_TEST(TagQuery_NestsAndRoundTripsThroughJson) {
    TagContainer alive;
    alive.Add(T("State.Alive"));
    alive.Add(T("Team.Red"));
    TagContainer stunned = alive;
    stunned.Add(T("State.Stunned"));
    const TagQuery q = TagQuery::And({TagQuery::All({T("State.Alive")}), TagQuery::None({T("State.Stunned")}), TagQuery::Or({TagQuery::Any({T("Team.Red"), T("Team.Blue")})})});
    CHECK(q.Matches(alive) && !q.Matches(stunned) && !q.Matches(TagContainer{}));
    CHECK(TagQuery::And({}).Matches(alive) && !TagQuery::Or({}).Matches(alive));
    CHECK(TagQuery::All({}).Matches(alive) && TagQuery::None({}).Matches(alive) && !TagQuery::Any({}).Matches(alive));
    const reflect::Json j = q.ToJson();
    CHECK(j["op"] == "and" && j["children"].size() == 3 && j["children"][0]["tags"][0] == "State.Alive");
    TagQuery back;
    std::string error;
    CHECK(TagQuery::FromJson(j, back, &error) && back == q && back.Matches(alive));
    // Bad input says why.
    for (const char* bad : {R"({"op":"nope"})", R"({"tags":["a"]})", R"([1])", R"({"op":"all","tags":["bad name"]})", R"({"op":"all","tags":"a"})",
                            R"({"op":"and","children":[{"op":"zzz"}]})", R"({"op":"and","children":5})", R"({"op":"all","tags":[3]})"}) {
        error.clear();
        CHECK(!TagQuery::FromJson(reflect::Json::parse(bad), back, &error) && !error.empty());
    }
    // Too deep.
    reflect::Json deep = {{"op", "all"}};
    for (int i = 0; i < 40; ++i) deep = {{"op", "and"}, {"children", reflect::Json::array({deep})}};
    CHECK(!TagQuery::FromJson(deep, back, &error));
}

AETHER_TEST(GameplayTag_ReflectedAndSaved) {
    TagContainer c;
    c.Add(T("State.Stunned"), 2);
    c.Add(T("Buff.Haste"));
    const reflect::Json j = reflect::ToJson(c);
    CHECK(j["tags"].size() == 2 && j["tags"][0]["tag"] == "Buff.Haste" && j["tags"][1]["count"] == 2); // sorted, with counts
    TagContainer back;
    CHECK(reflect::FromJson(back, j) && back.Count(T("State.Stunned")) == 2 && back.Count(T("Buff.Haste")) == 1 && back.Size() == 2);
    const reflect::Json tag = reflect::ToJson(T("Damage.Fire"));
    GameplayTag t2;
    CHECK(reflect::FromJson(t2, tag) && t2.name == "Damage.Fire");
    CHECK(reflect::TypeRegistry::Find("TagContainer") != nullptr && reflect::TypeRegistry::Find("GameplayTag") != nullptr);
    // A component like any other.
    World world;
    const Entity e = world.CreateEntity(Transform{Vec3(), Quaternion::Identity()}, c);
    CHECK(world.GetComponent<TagContainer>(e) != nullptr && world.GetComponent<TagContainer>(e)->HasTag(T("State")));
}

AETHER_TEST(GameplayTags_BlueprintLibrary) {
    const reflect::TypeInfo* type = reflect::TypeRegistry::Find("GameplayTags");
    CHECK(type != nullptr && type->functions.size() == 5);
    CHECK(GameplayTags::Matches("Damage.Fire.Burning", "Damage.Fire") && !GameplayTags::Matches("Damage", "Damage.Fire") && !GameplayTags::Matches("bad name", "a"));
    CHECK(GameplayTags::IsValid("a.b") && !GameplayTags::IsValid("a..b") && GameplayTags::MatchesExact("a.b", "a.b") && !GameplayTags::MatchesExact("a.b.c", "a.b"));
    CHECK(GameplayTags::GetParent("a.b.c") == "a.b" && GameplayTags::GetParent("a").empty() && GameplayTags::GetDepth("a.b.c") == 3 && GameplayTags::GetDepth("bad name") == 0);
    // As nodes in a real graph.
    bp::Blueprint blueprint;
    bp::Graph events;
    events.name = "EventGraph";
    blueprint.graphs.push_back(events);
    bp::GraphBuilder b(*blueprint.FindGraph("EventGraph"));
    const bp::NodeId begin = b.Add("Event.BeginPlay"), parent = b.Add("Call.Native:GameplayTags.GetParent"), print = b.Add("Debug.Print");
    b.Default(parent, "tag", "State.Stunned.Frozen");
    b.Connect(begin, "then", print, "exec").Connect(parent, "return", print, "text");
    bp::CompileResult compiled = bp::CompileBlueprint(blueprint);
    for (const bp::Diagnostic& d : compiled.diagnostics.diagnostics) std::printf("    %s: %s\n", d.code.c_str(), d.message.c_str());
    CHECK(compiled.Ok());
    World world;
    bp::BlueprintVM vm(world);
    std::vector<std::string> printed;
    vm.SetPrintHandler([&](Entity, const std::string& text) { printed.push_back(text); });
    const Entity e = world.CreateEntity(Transform{Vec3(), Quaternion::Identity()});
    CHECK(vm.Attach(e, compiled.blueprint));
    vm.BeginPlay();
    CHECK(printed == std::vector<std::string>{"State.Stunned"});
}
