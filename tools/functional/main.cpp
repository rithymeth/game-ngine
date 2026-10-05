// aether_functional: runs functional tests headless (Phase 23 step 5).
//
//   aether_functional [--list] [--filter TEXT]... [--tag TAG]... [--stop-on-failure]
//                     [--junit FILE] [--json FILE] [--quiet]
//
// Exits 0 when every selected test passes, 1 when one fails, 2 on bad arguments.

#include "aether/core/log.h"
#include "aether/testing/functional_test.h"

#include <cstdio>
#include <cstring>
#include <string>

int main(int argc, char** argv) {
    using namespace aether;
    FunctionalRunOptions options;
    std::string junit, json;
    bool list = false, quiet = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto value = [&](std::string& out) {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s needs a value\n", arg.c_str());
                return false;
            }
            out = argv[++i];
            return true;
        };
        std::string v;
        if (arg == "--list") list = true;
        else if (arg == "--quiet") quiet = true;
        else if (arg == "--stop-on-failure") options.stop_on_failure = true;
        else if (arg == "--filter") {
            if (!value(v)) return 2;
            options.filters.push_back(v);
        } else if (arg == "--tag") {
            if (!value(v)) return 2;
            options.tags.push_back(v);
        } else if (arg == "--junit") {
            if (!value(junit)) return 2;
        } else if (arg == "--json") {
            if (!value(json)) return 2;
        } else {
            std::fprintf(stderr, "unknown argument '%s'\n", arg.c_str());
            return 2;
        }
    }
    const FunctionalTestRegistry& registry = FunctionalTestRegistry::Get();
    if (list) {
        for (const FunctionalTest& t : registry.Matching(options)) {
            std::printf("%s", t.Name().c_str());
            for (const std::string& tag : t.Tags()) std::printf(" #%s", tag.c_str());
            std::printf("\n");
        }
        return 0;
    }
    if (quiet) Logger::Instance().SetStdout(false);
    const FunctionalReport report = registry.Run(options);
    Logger::Instance().SetStdout(true);
    std::printf("%s", FormatFunctionalSummary(report).c_str());
    if (!junit.empty() && !WriteJUnitXml(report, junit)) std::fprintf(stderr, "couldn't write %s\n", junit.c_str());
    if (!json.empty() && !WriteJsonReport(report, json)) std::fprintf(stderr, "couldn't write %s\n", json.c_str());
    if (report.results.empty()) std::fprintf(stderr, "no functional tests matched\n");
    return report.Ok() ? 0 : 1;
}
