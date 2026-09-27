#include "aether/script/luau_host.h"
#include "test_framework.h"

#include <filesystem>
#include <fstream>

using namespace aether;
using namespace aether::script;

namespace {

f64 Number(const ScriptValue& v) { return std::holds_alternative<f64>(v) ? std::get<f64>(v) : -1.0; }
std::string Text(const ScriptValue& v) { return std::holds_alternative<std::string>(v) ? std::get<std::string>(v) : ""; }

} // namespace

AETHER_TEST(Luau_RunsChunksAndCallsFunctions) {
    LuauHost host;
    ScriptResult r = host.Run("return 1 + 2, 'three', true, nil", "sum");
    AETHER_CHECK(r.ok && r.values.size() == 4 && Number(r.values[0]) == 3.0 && Text(r.values[1]) == "three");
    AETHER_CHECK(std::get<bool>(r.values[2]) && std::holds_alternative<std::monostate>(r.values[3]));

    // Globals persist between runs; C++ calls script functions.
    AETHER_CHECK(host.Run(R"(
        function Damage(health: number, amount: number): number
            return math.max(0, health - amount)
        end
        greeting = "hi"
    )", "combat").ok);
    ScriptResult hit = host.Call("Damage", {100.0, 30.0});
    AETHER_CHECK(hit.ok && Number(hit.values[0]) == 70.0);
    AETHER_CHECK(Number(host.Call("Damage", {10.0, 30.0}).values[0]) == 0.0);
    AETHER_CHECK(Text(host.GetGlobal("greeting")) == "hi");
    host.SetGlobal("speed", 4.5);
    AETHER_CHECK(Number(host.Run("return speed * 2").values[0]) == 9.0);
    AETHER_CHECK(!host.Call("Nope").ok);

    // Compiled bytecode is cached by source.
    const usize compiled = host.CompiledCount();
    host.Run("return 1 + 2, 'three', true, nil", "again");
    AETHER_CHECK(host.CompiledCount() == compiled);

    // print goes to the handler, tab-separated like Lua's.
    std::vector<std::string> printed;
    host.SetPrintHandler([&](const std::string& line) { printed.push_back(line); });
    host.Run("print('score', 42, true)");
    AETHER_CHECK(printed.size() == 1 && printed[0] == "score\t42\ttrue");
}

AETHER_TEST(Luau_ErrorsAreReportedWithLocations) {
    LuauHost host;
    ScriptResult syntax = host.Run("local x = = 1", "broken");
    AETHER_CHECK(!syntax.ok && syntax.error.find("broken:1") != std::string::npos);
    AETHER_CHECK(host.CompiledCount() == 0); // failed compiles aren't cached

    ScriptResult runtime = host.Run("local t = nil\nreturn t.field", "nil_index");
    AETHER_CHECK(!runtime.ok && runtime.error.find("nil_index:2") != std::string::npos);
    ScriptResult thrown = host.Run("error('out of ammo')", "gun");
    AETHER_CHECK(!thrown.ok && thrown.error.find("out of ammo") != std::string::npos);
    // The VM is fine afterwards.
    AETHER_CHECK(host.Run("return 5").ok);

    const std::filesystem::path file = std::filesystem::temp_directory_path() / "aether_test_script.luau";
    std::ofstream(file) << "return 'from a file'";
    ScriptResult from_file = host.RunFile(file);
    AETHER_CHECK(from_file.ok && Text(from_file.values[0]) == "from a file");
    std::filesystem::remove(file);
    AETHER_CHECK(!host.RunFile(file).ok);
}

AETHER_TEST(Luau_SandboxAndLimits) {
    LuauHost host;
    // Removed or trimmed libraries.
    AETHER_CHECK(Text(host.Run("return type(io)").values[0]) == "nil");
    AETHER_CHECK(Text(host.Run("return type(debug)").values[0]) == "nil");
    AETHER_CHECK(Text(host.Run("return type(loadstring)").values[0]) == "nil");
    AETHER_CHECK(Text(host.Run("return type(os.clock)").values[0]) == "function");
    AETHER_CHECK(Text(host.Run("return type(os.execute)").values[0]) == "nil");
    AETHER_CHECK(Text(host.Run("return type(os.exit)").values[0]) == "nil");
    // Allowed ones.
    AETHER_CHECK(Number(host.Run("return bit32.band(6, 3) + #string.rep('a', 3) + math.floor(2.7)").values[0]) == 7.0);
    AETHER_CHECK(Number(host.Run("local t = {3, 1, 2}; table.sort(t); return t[1]").values[0]) == 1.0);

    LuauHost editor(LuauHost::Options{64u * 1024u * 1024u, 10'000'000, /*allow_debug=*/true});
    AETHER_CHECK(Text(editor.Run("return type(debug)").values[0]) == "table");

    // An endless loop fails cleanly instead of hanging.
    LuauHost tight(LuauHost::Options{64u * 1024u * 1024u, 100'000, false});
    ScriptResult spin = tight.Run("while true do end", "spin");
    AETHER_CHECK(!spin.ok && spin.error.find("instruction budget") != std::string::npos);
    AETHER_CHECK(tight.Run("local s = 0; for i = 1, 1000 do s += i end; return s").ok); // the budget resets per run

    // So does runaway allocation.
    LuauHost small(LuauHost::Options{2u * 1024u * 1024u, 0, false});
    const usize baseline = small.MemoryUsed();
    AETHER_CHECK(baseline > 0 && baseline < 2u * 1024u * 1024u);
    ScriptResult hog = small.Run("local t = {} for i = 1, 10000000 do t[i] = string.rep('x', 100) .. i end", "hog");
    AETHER_CHECK(!hog.ok && hog.error.find("memory") != std::string::npos);
    AETHER_CHECK(small.Run("return 1").ok);
}
