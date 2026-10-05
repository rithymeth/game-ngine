#include "test_framework.h"

#include "aether/blueprint/graph.h"
#include "aether/core/json_util.h"
#include "aether/ecs/world.h"
#include "aether/scene/serialization.h"

#include <string>
#include <vector>

// Loaders must answer false on a malformed file instead of throwing (Phase 47 step 1: found by the fuzzers).

using namespace aether;

namespace {
std::vector<u8> Bytes(const std::string& s) { return std::vector<u8>(s.begin(), s.end()); }
} // namespace

AETHER_TEST(LoaderRobustness_JsonVersionIsSafe) {
    using nlohmann::json;
    AETHER_CHECK(JsonVersion(json{{"$version", 3}}) == 3);
    AETHER_CHECK(JsonVersion(json{{"$version", "3"}}) == 0 && JsonVersion(json{{"$version", json::array()}}) == 0 && JsonVersion(json{{"$version", 1.5}}) == 0);
    AETHER_CHECK(JsonVersion(json::object()) == 0 && JsonVersion(json::array()) == 0 && JsonVersion(json(5)) == 0 && JsonVersion(json::object(), 7) == 7);
    AETHER_CHECK(JsonVersion(json{{"$version", 9999999999ll}}) == 0); // outside int: not a version
}

AETHER_TEST(LoaderRobustness_SceneAndBlueprintWithWrongTypesDontThrow) {
    World world;
    for (const char* text : {R"({"$type":"Scene","$version":"1","entities":[]})", R"({"$type":"Scene","$version":[1],"entities":[]})", R"({"$type":["Scene"],"$version":1})",
                             R"({"$type":"Scene","$version":1,"entities":[{"guid":5,"components":{}}]})", R"({"$type":"Scene","$version":1,"entities":[{"components":[]}]})"}) {
        const std::vector<u8> bytes = Bytes(text);
        bool threw = false, loaded = true;
        try {
            loaded = LoadSceneJsonFromMemory(world, bytes, "test");
        } catch (...) {
            threw = true;
        }
        AETHER_CHECK(!threw);
        (void)loaded;
    }
    for (const char* text : {R"({"$type":"Blueprint","$version":"BP"})", R"({"$type":"Blueprint","$version":1,"parent":5})", R"({"$type":"Blueprint","$version":1,"variables":[5]})",
                             R"({"$type":"Blueprint","$version":1,"graphs":[{"name":7,"nodes":[]}]})", R"({"$type":"Blueprint","$version":1,"dispatchers":[{"name":3}]})"}) {
        const nlohmann::json j = nlohmann::json::parse(text);
        bp::Blueprint blueprint;
        std::string error;
        bool threw = false;
        try {
            (void)bp::BlueprintFromJson(j, blueprint, &error);
        } catch (...) {
            threw = true;
        }
        AETHER_CHECK(!threw);
    }
    // And the right shape still loads.
    AETHER_CHECK(LoadSceneJsonFromMemory(world, Bytes(R"({"$type":"Scene","$version":1,"entities":[]})"), "ok"));
}
