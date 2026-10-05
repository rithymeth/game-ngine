#include "test_framework.h"

#include "aether/core/base.h"

#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>

// The manual (Phase 26 step 5, §26.7): its chapters exist, every link between
// documents resolves, and what it tells you to run is something the build has.

#define CHECK(...) AETHER_CHECK((__VA_ARGS__))

using namespace aether;
namespace stdfs = std::filesystem;

namespace {

std::string Read(const stdfs::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::stringstream s;
    s << in.rdbuf();
    return s.str();
}

} // namespace

AETHER_TEST(Manual_ChaptersExistAndEveryLinkResolves) {
    const stdfs::path manual = stdfs::path(AETHER_REPO_DOCS_DIR) / "manual";
    CHECK(stdfs::is_directory(manual));
    const std::string index = Read(manual / "README.md");
    CHECK(!index.empty());
    const std::vector<std::string> chapters = {"01-getting-started.md", "02-projects-and-assets.md", "03-scripting.md",
                                               "04-input.md",           "05-2d.md",                  "06-packaging.md",
                                               "07-extending.md",       "08-api-reference.md",  "09-gameplay.md",       "10-inventory.md",      "11-interaction.md"};
    const std::regex link(R"(\]\(([^)\s]+)\))");
    usize checked = 0;
    for (const std::string& name : chapters) {
        CHECK(stdfs::exists(manual / name));
        CHECK(index.find("(" + name + ")") != std::string::npos); // the contents lists it
    }
    // Every relative link in the index and the chapters points at a file that exists.
    std::vector<std::string> files = {"README.md"};
    files.insert(files.end(), chapters.begin(), chapters.end());
    for (const std::string& name : files) {
        const std::string text = Read(manual / name);
        CHECK(text.size() > 200);
        for (std::sregex_iterator it(text.begin(), text.end(), link), end; it != end; ++it) {
            std::string target = (*it)[1];
            if (target.rfind("http", 0) == 0 || target.rfind('#', 0) == 0) continue;
            const usize hash = target.find('#');
            if (hash != std::string::npos) target.resize(hash);
            const bool ok = stdfs::exists(manual / target);
            if (!ok) std::printf("    %s links to %s, which doesn't exist\n", name.c_str(), target.c_str());
            CHECK(ok);
            ++checked;
        }
    }
    CHECK(checked >= 12);
    // The documents index lists the manual.
    CHECK(Read(stdfs::path(AETHER_REPO_DOCS_DIR) / "README.md").find("manual/README.md") != std::string::npos);
}

AETHER_TEST(Manual_NamesRealTemplatesAndTools) {
    const stdfs::path manual = stdfs::path(AETHER_REPO_DOCS_DIR) / "manual";
    const std::string start = Read(manual / "01-getting-started.md");
    for (const char* t : {"Blank", "First Person", "Third Person", "Top Down", "Vehicle", "2D Platformer"}) {
        CHECK(start.find(t) != std::string::npos);
    }
    for (const char* tool : {"aether_cook", "aether_player", "aether_docgen"}) CHECK(start.find(tool) != std::string::npos);
    const std::string api = Read(manual / "08-api-reference.md");
    CHECK(api.find("aether_docgen --out api") != std::string::npos && api.find("WriteApiDocs") != std::string::npos);
    const std::string packaging = Read(manual / "06-packaging.md");
    for (const char* flag : {"--patch-of", "--dlc", "--key", "--config shipping"}) CHECK(packaging.find(flag) != std::string::npos);
}
