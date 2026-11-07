#include "test_framework.h"

#include "aether/ai/utility.h"

#include <string>

using namespace aether;

// Each malformed document from Utility_JsonRoundTripsAndRefusesBadInput gets its own test, so when one of them ends the
// process the crash reporter names which document did it instead of naming the whole loop.

namespace {

bool Loads(const char* text) {
    ai::UtilityBrain loaded;
    std::string error;
    return ai::UtilityBrain::FromJson(nlohmann::json::parse(text), loaded, &error);
}

} // namespace

AETHER_TEST(UtilityLoader_RejectsANewerFormatVersion) {
    AETHER_CHECK(!Loads(R"({"$type":"UtilityBrain","$version":9,"actions":[]})"));
}

AETHER_TEST(UtilityLoader_RejectsAnActionWithoutAName) {
    AETHER_CHECK(!Loads(R"({"$type":"UtilityBrain","actions":[{"name":""}]})"));
}

AETHER_TEST(UtilityLoader_RejectsAnUnknownCurve) {
    AETHER_CHECK(!Loads(R"({"$type":"UtilityBrain","actions":[{"name":"a","considerations":[{"input":"x","curve":{"kind":"wobbly"}}]}]})"));
}

AETHER_TEST(UtilityLoader_RejectsAConsiderationWithoutAnInput) {
    AETHER_CHECK(!Loads(R"({"$type":"UtilityBrain","actions":[{"name":"a","considerations":[{"input":""}]}]})"));
}

AETHER_TEST(UtilityLoader_RejectsANonArrayActionList) {
    AETHER_CHECK(!Loads(R"({"$type":"UtilityBrain","actions":5})"));
}

AETHER_TEST(UtilityLoader_RejectsANumericTypeTag) {
    AETHER_CHECK(!Loads(R"({"$type":5})"));
}

AETHER_TEST(UtilityLoader_RejectsANumericActionName) {
    AETHER_CHECK(!Loads(R"({"$type":"UtilityBrain","actions":[{"name":5}]})"));
}

AETHER_TEST(UtilityLoader_RejectsAJsonArray) {
    AETHER_CHECK(!Loads("[]"));
}
