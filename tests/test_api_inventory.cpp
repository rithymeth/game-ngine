// M8.1 Luau API inventory: walks every global table and function a game script
// sees (ScriptSystem with the kit APIs, plus the world bindings), and compares
// the sorted list with docs/design/api_inventory_luau.txt. An unreviewed change
// to the script API fails here. To accept a change on purpose, run
//   python3 tools/api_inventory.py --update --tests <build>/tests/aether_tests
// and commit the regenerated file with the change.

#include "test_framework.h"

#if AETHER_TEST_HAS_SCRIPTING

#include "aether/scene/components.h"
#include "aether/script/script_system.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <variant>

using namespace aether;
using namespace aether::script;

namespace {

// Luau's own globals are not engine API; everything else in _G is.
const char* kWalker = R"(
local builtin = {}
for _, n in ipairs({'_G','_VERSION','assert','bit32','buffer','collectgarbage','coroutine','debug','error','gcinfo',
    'getfenv','getmetatable','ipairs','load','loadstring','math','newproxy','next','os','pairs','pcall','print',
    'rawequal','rawget','rawlen','rawset','require','select','setfenv','setmetatable','string','table','tonumber',
    'tostring','type','typeof','unpack','utf8','vector','xpcall','integer'}) do builtin[n] = true end
local out, seen = {}, {}
local function walk(path, t, depth)
    if seen[t] or depth > 6 then return end
    seen[t] = true
    for k, v in pairs(t) do
        if type(k) == 'string' then
            local p = path == '' and k or (path .. '.' .. k)
            local kind = typeof(v)
            table.insert(out, p .. ' ' .. kind)
            if kind == 'table' then walk(p, v, depth + 1) end
        end
    end
    local mt = getmetatable(t)
    if type(mt) == 'table' and type(mt.__index) == 'table' and not seen[mt.__index] then
        walk(path .. '<index>', mt.__index, depth + 1)
    end
end
for k, v in pairs(_G) do
    if type(k) == 'string' and not builtin[k] then
        table.insert(out, k .. ' ' .. typeof(v))
        if type(v) == 'table' then walk(k, v, 1) end
    end
end
-- Which Luau builtins the sandbox leaves visible (removing one is an API change).
for n in pairs(builtin) do table.insert(out, '<builtin>.' .. n .. ' ' .. typeof(_G[n])) end
-- Methods on the handle types scripts get back (userdata metatables).
local function methods(label, value)
    local ok, mt = pcall(getmetatable, value)
    if not ok or type(mt) ~= 'table' then return end
    for k, v in pairs(mt) do
        if type(k) == 'string' then table.insert(out, label .. ':meta.' .. k .. ' ' .. typeof(v)) end
    end
    if type(mt.__index) == 'table' then
        for k, v in pairs(mt.__index) do
            if type(k) == 'string' then table.insert(out, label .. ':' .. k .. ' ' .. typeof(v)) end
        end
    end
end
local e = world:Spawn()
methods('<Entity>', e)
pcall(function() methods('<Transform>', e:Add('Transform')) end)
pcall(function() methods('<Event>', Event.new()) end)
pcall(function() methods('<Timer>', Timer.After(1000, function() end)) end)
pcall(function() methods('<Connection>', Event.new():Connect(function() end)) end)
table.sort(out)
return table.concat(out, '\n')
)";

std::string Config() {
    std::string c = "scripting";
#if AETHER_TEST_HAS_PHYSICS
    c += " physics";
#endif
#if AETHER_TEST_HAS_QUESTS
    c += " quests";
#endif
#if AETHER_TEST_HAS_INTERACTION
    c += " interaction";
#endif
#if AETHER_TEST_HAS_INVENTORY
    c += " inventory";
#endif
    return c;
}

std::set<std::string> Lines(const std::string& text) {
    std::set<std::string> lines;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty() && line[0] != '#') lines.insert(line);
    }
    return lines;
}

} // namespace

AETHER_TEST(ApiInventory_LuauMatchesCheckedInFile) {
    (void)GetComponentId<ScriptComponent>();
    (void)GetComponentId<Transform>();
    World world;
    GuidIndex guids;
    LuauHost host;
    ScriptSystem system(host, world, guids, [](const assets::AssetGuid&, std::string&, std::string&) { return false; }, true);
    host.BindWorld(&world, &guids);
    ScriptResult r = host.Run(kWalker, "api_inventory");
    AETHER_CHECK(r.ok);
    if (!r.ok || r.values.empty() || !std::holds_alternative<std::string>(r.values[0])) {
        std::printf("  walker failed: %s\n", r.error.c_str());
        return;
    }
    const std::string dump = std::get<std::string>(r.values[0]);
    const std::string path = std::string(AETHER_REPO_DOCS_DIR) + "/design/api_inventory_luau.txt";
    const std::string header = "# Luau script API inventory (M8.1). Generated; do not edit by hand.\n"
                               "# Regenerate: python3 tools/api_inventory.py --update --tests <build>/tests/aether_tests\n"
                               "# Build features: " + Config() + "\n";
    const char* update = std::getenv("AETHER_UPDATE_API_INVENTORY");
    if (update && *update) {
        std::ofstream out(path, std::ios::binary);
        out << header << dump << "\n";
        AETHER_CHECK(out.good());
        std::printf("  wrote %s\n", path.c_str());
        return;
    }
    std::ifstream in(path, std::ios::binary);
    std::stringstream buffer;
    buffer << in.rdbuf();
    const std::string expected_text = buffer.str();
    // The inventory is recorded for a build with every optional module; skip lean builds.
    if (expected_text.find("# Build features: " + Config() + "\n") == std::string::npos &&
        expected_text.find("# Build features: " + Config() + "\r\n") == std::string::npos) {
        std::printf("  skipped: this build's features (%s) differ from the recorded inventory\n", Config().c_str());
        return;
    }
    const std::set<std::string> expected = Lines(expected_text);
    const std::set<std::string> actual = Lines(dump);
    int shown = 0;
    for (const auto& line : expected)
        if (!actual.count(line) && shown++ < 40) std::printf("  removed from the script API: %s\n", line.c_str());
    for (const auto& line : actual)
        if (!expected.count(line) && shown++ < 40) std::printf("  added to the script API: %s\n", line.c_str());
    if (expected != actual) {
        std::printf("  The Luau API changed without updating docs/design/api_inventory_luau.txt.\n"
                    "  If the change is intended, run: python3 tools/api_inventory.py --update --tests <build>/tests/aether_tests\n"
                    "  and commit the file (removals of stable names need a deprecation first; see docs/manual/api_stability.md).\n");
    }
    AETHER_CHECK(expected == actual);
}

#endif
