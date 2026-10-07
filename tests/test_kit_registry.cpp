#include "test_framework.h"

#include "aether/gameplay/gameplay_kit.h"
#include "aether/kit/kit.h"
#include "aether/kit/event_bus.h"
#if AETHER_KIT_INVENTORY
#include "aether/inventory/inventory_kit.h"
#endif
#if AETHER_KIT_INTERACTION
#include "aether/interaction/interaction_kit.h"
#endif
#if AETHER_KIT_QUESTS
#include "aether/quests/quests_kit.h"
#endif

#include <algorithm>

// The kit registry (Phase 37 step 1): dependency order, duplicate and missing names, cycles, stage order.

using namespace aether;
using namespace aether::kit;

namespace {

class TestKit : public IKit {
public:
    TestKit(std::string name, std::vector<std::string> deps, std::vector<std::string>* log = nullptr)
        : name_(std::move(name)), deps_(std::move(deps)), log_(log) {}
    const char* Name() const override { return name_.c_str(); }
    std::vector<std::string> Deps() const override { return deps_; }
    void RegisterComponents() override {
        if (log_) log_->push_back(name_);
    }
    void LoadAssets(const KitAssetContext&) override {
        if (log_) log_->push_back("load-" + name_);
    }
    void InstallBlueprintNodes() override {
        if (log_) log_->push_back("blueprint-" + name_);
    }
    void InstallScriptApi(void*) override {
        if (log_) log_->push_back("script-" + name_);
    }
    std::vector<KitStage> Stages() const override { return {{"Player." + name_, {}}}; }

private:
    std::string name_;
    std::vector<std::string> deps_;
    std::vector<std::string>* log_;
};

std::string Names(const KitRegistry& registry) {
    std::string out;
    for (IKit* kit : registry.Order()) out += std::string(out.empty() ? "" : ",") + kit->Name();
    return out;
}

} // namespace

AETHER_TEST(Kit_DependenciesComeFirstAndTiesAreAlphabetical) {
    KitRegistry registry;
    registry.Add(std::make_unique<TestKit>("Quests", std::vector<std::string>{"Inventory", "Gameplay"}));
    registry.Add(std::make_unique<TestKit>("Interaction", std::vector<std::string>{}));
    registry.Add(std::make_unique<TestKit>("Inventory", std::vector<std::string>{"Gameplay"}));
    registry.Add(std::make_unique<TestKit>("Gameplay", std::vector<std::string>{}));
    AETHER_CHECK(registry.Resolve());
    AETHER_CHECK(Names(registry) == "Gameplay,Interaction,Inventory,Quests");
}

AETHER_TEST(Kit_OrderDoesNotDependOnRegistrationOrder) {
    const char* names[] = {"A", "B", "C", "D"};
    std::string first;
    std::vector<int> perm = {0, 1, 2, 3};
    do {
        KitRegistry registry;
        for (int i : perm) {
            // C needs A, D needs C and B.
            std::vector<std::string> deps;
            if (i == 2) deps = {"A"};
            if (i == 3) deps = {"C", "B"};
            registry.Add(std::make_unique<TestKit>(names[i], deps));
        }
        AETHER_CHECK(registry.Resolve());
        if (first.empty()) first = Names(registry);
        AETHER_CHECK(Names(registry) == first);
    } while (std::next_permutation(perm.begin(), perm.end()));
    AETHER_CHECK(first == "A,B,C,D");
}

AETHER_TEST(Kit_ReportsDuplicateMissingAndCycles) {
    {
        KitRegistry registry;
        AETHER_CHECK(registry.Add(std::make_unique<TestKit>("A", std::vector<std::string>{})));
        AETHER_CHECK(!registry.Add(std::make_unique<TestKit>("A", std::vector<std::string>{})));
        AETHER_CHECK(!registry.Add(nullptr));
        AETHER_CHECK(registry.Errors().size() == 2);
        AETHER_CHECK(registry.Count() == 1);
    }
    {
        KitRegistry registry;
        registry.Add(std::make_unique<TestKit>("A", std::vector<std::string>{"Ghost"}));
        AETHER_CHECK(!registry.Resolve());
        AETHER_CHECK(registry.Order().empty());
        AETHER_CHECK(registry.Errors().size() == 1);
        AETHER_CHECK(registry.Errors()[0].find("Ghost") != std::string::npos);
    }
    {
        KitRegistry registry;
        registry.Add(std::make_unique<TestKit>("A", std::vector<std::string>{"B"}));
        registry.Add(std::make_unique<TestKit>("B", std::vector<std::string>{"A"}));
        registry.Add(std::make_unique<TestKit>("C", std::vector<std::string>{}));
        AETHER_CHECK(!registry.Resolve());
        AETHER_CHECK(registry.Order().empty());
        AETHER_CHECK(registry.Errors()[0].find("cycle") != std::string::npos);
        AETHER_CHECK(registry.Errors()[0].find("A") != std::string::npos);
    }
}

AETHER_TEST(Kit_InstallHooksRunInDependencyOrder) {
    std::vector<std::string> log;
    KitRegistry registry;
    registry.Add(std::make_unique<TestKit>("B", std::vector<std::string>{"A"}, &log));
    registry.Add(std::make_unique<TestKit>("A", std::vector<std::string>{}, &log));
    AETHER_CHECK(registry.Resolve());
    registry.RegisterComponents();
    AETHER_CHECK(log.size() == 2 && log[0] == "A" && log[1] == "B");
    log.clear();
    registry.LoadAssets({});
    AETHER_CHECK(log.size() == 2 && log[0] == "load-A" && log[1] == "load-B");
    log.clear();
    registry.InstallBlueprintNodes();
    AETHER_CHECK(log.size() == 2 && log[0] == "blueprint-A" && log[1] == "blueprint-B");
    log.clear();
    registry.InstallScriptApi(nullptr);
    AETHER_CHECK(log.size() == 2 && log[0] == "script-A" && log[1] == "script-B");
    AETHER_CHECK(registry.Has("A") && !registry.Has("Z"));
    AETHER_CHECK(registry.Find("B") != nullptr);
}

AETHER_TEST(Kit_StagesFollowTheirDependencies) {
    KitRegistry registry;
    registry.Add(std::make_unique<TestKit>("Quests", std::vector<std::string>{"Inventory"}));
    registry.Add(std::make_unique<TestKit>("Inventory", std::vector<std::string>{}));
    AETHER_CHECK(registry.Resolve());
    const std::vector<KitStage> stages = registry.Stages();
    AETHER_CHECK(stages.size() == 2);
    AETHER_CHECK(stages[0].name == "Player.Inventory" && stages[0].after.empty());
    AETHER_CHECK(stages[1].name == "Player.Quests");
    AETHER_CHECK(stages[1].after.size() == 1 && stages[1].after[0] == "Player.Inventory");
}

AETHER_TEST(Kit_EventBusDrainsTypedEventsInOrder) {
    struct ItemEvent { int item; int count; };
    struct QuestEvent { int id; };
    KitEventBus bus;
    bus.Publish(ItemEvent{3, 2});
    bus.Publish(QuestEvent{9});
    bus.Publish(ItemEvent{5, 1});

    AETHER_CHECK(bus.HasPending<ItemEvent>());
    AETHER_CHECK(bus.HasPending<QuestEvent>());
    const std::vector<ItemEvent> items = bus.Drain<ItemEvent>();
    AETHER_CHECK(items.size() == 2);
    AETHER_CHECK(items[0].item == 3 && items[0].count == 2);
    AETHER_CHECK(items[1].item == 5 && items[1].count == 1);
    AETHER_CHECK(!bus.HasPending<ItemEvent>());
    AETHER_CHECK(bus.Drain<ItemEvent>().empty());
    const std::vector<QuestEvent> quests = bus.Drain<QuestEvent>();
    AETHER_CHECK(quests.size() == 1 && quests[0].id == 9);
}

// The gameplay kits resolve with Gameplay first and expose their scheduler stages through the registry.
AETHER_TEST(Kit_TheGameplayKitsResolveInOrder) {
    KitRegistry registry;
    registry.Add(gas::MakeGameplayKit());
#if AETHER_KIT_QUESTS
    registry.Add(quest::MakeQuestsKit());
#endif
#if AETHER_KIT_INVENTORY
    registry.Add(inv::MakeInventoryKit());
#endif
#if AETHER_KIT_INTERACTION
    registry.Add(interact::MakeInteractionKit());
#endif
    AETHER_CHECK(registry.Resolve() && registry.Errors().empty());
    AETHER_CHECK(!registry.Order().empty() && std::string(registry.Order()[0]->Name()) == "Gameplay");
    registry.RegisterComponents(); // idempotent, and safe to run in any order
    std::vector<std::string> names;
    for (const KitStage& stage : registry.Stages()) {
        names.push_back(stage.name);
        if (stage.name != "Player.Effects") {
            AETHER_CHECK(std::find(stage.after.begin(), stage.after.end(), "Player.Effects") != stage.after.end());
        }
    }
#if AETHER_KIT_INVENTORY && AETHER_KIT_INTERACTION && AETHER_KIT_QUESTS
    AETHER_CHECK((names == std::vector<std::string>{"Player.Attributes", "Player.Abilities", "Player.Effects", "Player.Interaction", "Player.Inventory", "Player.Quests"}));
#endif
}
