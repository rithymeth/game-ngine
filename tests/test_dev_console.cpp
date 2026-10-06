#include "aether/core/cvars.h"
#include "test_framework.h"

// Phase 23 step 3: the CVar registry and command parser - registration,
// typed round-trips, canonicalization/validation, on_change, and running
// command lines. All names are unique to this file so repeated test runs
// (and the built-ins) never collide.

using namespace aether;
using namespace aether::cvars;

namespace {

int g_change_count = 0;

} // namespace

AETHER_TEST(DevCvars_TypedRoundTrips) {
    RegisterCVar("test.speed", CVarType::Float, "4.25", "movement speed");
    RegisterCVar("test.count", CVarType::Int, "10");
    RegisterCVar("test.debug", CVarType::Bool, "0");
    RegisterCVar("test.name", CVarType::String, "hero");

    AETHER_CHECK(Find("test.speed") != nullptr);
    AETHER_CHECK_NEAR(GetFloat("test.speed"), 4.25, 1e-6);
    AETHER_CHECK(GetInt("test.count") == 10);
    AETHER_CHECK(!GetBool("test.debug"));
    AETHER_CHECK(GetStringValue("test.name") == "hero");

    // Fallbacks for unknowns.
    AETHER_CHECK(GetInt("test.missing", 7) == 7);
    AETHER_CHECK_NEAR(GetFloat("test.missing", 2.5), 2.5, 1e-6);
}

AETHER_TEST(DevCvars_SetValidatesAndCanonicalizes) {
    AETHER_CHECK(SetString("test.speed", "9"));
    AETHER_CHECK_NEAR(GetFloat("test.speed"), 9.0, 1e-6);
    AETHER_CHECK(GetString("test.speed") == "9");

    // An int CVar refuses non-numeric input.
    AETHER_CHECK(!SetString("test.count", "abc"));
    AETHER_CHECK(GetInt("test.count") == 10); // unchanged

    // Bool canonicalization.
    AETHER_CHECK(SetString("test.debug", "on"));
    AETHER_CHECK(GetBool("test.debug") == true);
    AETHER_CHECK(GetString("test.debug") == "1");
    AETHER_CHECK(SetString("test.debug", "off"));
    AETHER_CHECK(GetBool("test.debug") == false);
}

AETHER_TEST(DevCvars_OnChangeFiresOncePerSuccessfulSet) {
    RegisterCVar("test.change", CVarType::Int, "0");
    if (CVar* c = FindMutable("test.change")) {
        c->on_change = [] { ++g_change_count; };
    }
    g_change_count = 0;
    AETHER_CHECK(SetInt("test.change", 1));
    AETHER_CHECK(g_change_count == 1);
    AETHER_CHECK(SetInt("test.change", 2));
    AETHER_CHECK(g_change_count == 2);
    // Failed set must not fire.
    AETHER_CHECK(!SetString("test.change", "nope"));
    AETHER_CHECK(g_change_count == 2);
}

AETHER_TEST(DevCvars_SplitArgumentsRespectsQuotes) {
    const std::vector<std::string> args = SplitArguments("a b \"c d\" e \"f g h\"");
    AETHER_CHECK(args.size() == 5);
    AETHER_CHECK(args[0] == "a");
    AETHER_CHECK(args[1] == "b");
    AETHER_CHECK(args[2] == "c d");
    AETHER_CHECK(args[3] == "e");
    AETHER_CHECK(args[4] == "f g h");
    AETHER_CHECK(SplitArguments("   ").empty());
}

AETHER_TEST(DevCvars_CommandsParseAndRun) {
    RegisterCommand("test.sum", [](const std::vector<std::string>& args, std::string& out) {
        if (args.size() != 3) {
            out = "usage: test.sum <a> <b>";
            return;
        }
        out = std::to_string(std::atoi(args[1].c_str()) + std::atoi(args[2].c_str()));
    });

    std::string out;
    AETHER_CHECK(RunCommand("test.sum 3 4", out));
    AETHER_CHECK(out == "7");

    // A bare CVar name prints its value.
    out.clear();
    AETHER_CHECK(RunCommand("test.count", out));
    AETHER_CHECK(out == "10");

    // A CVar assignment sets it.
    out.clear();
    AETHER_CHECK(RunCommand("test.count 22", out));
    AETHER_CHECK(GetInt("test.count") == 22);

    // Unknown command fails.
    out.clear();
    AETHER_CHECK(!RunCommand("test.nonexistent", out));
    AETHER_CHECK(!out.empty());
}

AETHER_TEST(DevCvars_BuiltinsAvailable) {
    std::string out;
    AETHER_CHECK(RunCommand("help", out));
    AETHER_CHECK(out.find("help") != std::string::npos);
    out.clear();
    AETHER_CHECK(RunCommand("echo hello world", out));
    AETHER_CHECK(out == "hello world");
    out.clear();
    AETHER_CHECK(RunCommand("cvars", out));
    AETHER_CHECK(out.find("test.count") != std::string::npos);
}

AETHER_TEST(DevCvars_ReregisterReturnsSameInstance) {
    const CVar* before = Find("test.count");
    AETHER_CHECK(before != nullptr);
    RegisterCVar("test.count", CVarType::Int, "99");
    const CVar* after = Find("test.count");
    AETHER_CHECK(after == before);
    AETHER_CHECK(GetInt("test.count") == 22); // unchanged; registration is a no-op
}
