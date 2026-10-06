#include "test_framework.h"

#include "aether/kit/kit.h"

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
