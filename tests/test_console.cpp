#include "aether/core/console.h"
#include "test_framework.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <thread>

// Phase 23 step 1: console variables (types, parsing, ranges, defaults,
// callbacks, the registry), the console (statements, built-ins, flags,
// commands, completion, history, config files, the command line, log
// capture), and the logger's sinks and recent lines.

using namespace aether;

#define CHECK(...) AETHER_CHECK((__VA_ARGS__))

namespace {

bool Has(const Console& c, const std::string& text) {
    return std::any_of(c.Output().begin(), c.Output().end(), [&](const ConsoleLine& l) { return l.text.find(text) != std::string::npos; });
}

AutoCVar<int> g_test_cascades("test.shadows.cascades", 4, "Shadow cascades (a test)", CVar_Archive, 1, 4);
AutoCVar<f32> g_test_scale("test.time.scale", 1.0f, "Time scale (a test)");
AutoCVar<std::string> g_test_name("test.player.name", "Ada", "A test string");
int g_test_command_calls = 0;
AutoConsoleCommand g_test_command("test.ping", "Counts its calls (a test)",
                                  [](const std::vector<std::string>& args, Console& c) {
                                      ++g_test_command_calls;
                                      c.Print("pong " + std::to_string(args.size()));
                                  });

} // namespace

AETHER_TEST(CVar_TypesParsingAndRanges) {
    CVarRegistry reg;
    CVar* b = reg.Register("r.Shadows", CVarType::Bool, "1", "Shadows on");
    CVar* i = reg.Register("r.shadows.cascades", CVarType::Int, "4", "Cascades", CVar_Archive, 1, 4);
    CVar* f = reg.Register("t.scale", CVarType::Float, "0.1", "Scale", CVar_None, 0.0, 10.0);
    CVar* s = reg.Register("player.name", CVarType::String, "Ada", "Name");
    CHECK(b && i && f && s);
    CHECK(b->GetBool() && i->GetInt() == 4 && f->GetFloat() == 0.1 && s->GetString() == "Ada");
    CHECK(f->ValueString() == "0.1" && FormatCVarFloat(1.0 / 3.0) == "0.3333333333333333" && FormatCVarFloat(2.0) == "2");
    // Parsing.
    for (const char* t : {"0", "false", "OFF", "no"}) CHECK(b->SetFromString(t) && !b->GetBool());
    for (const char* t : {"1", "TRUE", "on", "yes"}) CHECK(b->SetFromString(t) && b->GetBool());
    std::string error;
    CHECK(!b->SetFromString("maybe", &error) && error.find("bool") != std::string::npos && b->GetBool());
    CHECK(i->SetFromString("2") && i->GetInt() == 2 && i->SetFromString("3.0") && i->GetInt() == 3);
    CHECK(!i->SetFromString("2.5") && !i->SetFromString("x") && !i->SetFromString("") && i->GetInt() == 3);
    CHECK(i->SetFromString("0x2") && i->GetInt() == 2);
    // Ranges clamp.
    CHECK(i->SetFromString("99") && i->GetInt() == 4 && i->SetFromString("-5") && i->GetInt() == 1);
    f->SetFloat(25.0);
    CHECK(f->GetFloat() == 10.0 && !f->SetFromString("nan") && !f->SetFromString("1e999") && f->GetFloat() == 10.0);
    // Cross-type access and defaults.
    CHECK(i->GetFloat() == 1.0 && i->GetBool() && f->GetInt() == 10 && s->GetInt() == 0);
    CHECK(!i->IsDefault() && i->DefaultString() == "4");
    i->Reset();
    CHECK(i->IsDefault() && i->GetInt() == 4);
    s->SetString("Grace Hopper");
    CHECK(s->GetString() == "Grace Hopper" && s->ValueString() == "Grace Hopper");
    // Change notification.
    int calls = 0;
    i64 seen = 0;
    const int id = i->OnChange([&](const CVar& v) { ++calls, seen = v.GetInt(); });
    const u64 gen = i->Generation();
    i->SetInt(2);
    CHECK(calls == 1 && seen == 2 && i->Generation() == gen + 1);
    i->RemoveOnChange(id);
    i->SetInt(3);
    CHECK(calls == 1);
    // The registry: case-insensitive, idempotent, type clashes, names, commands.
    CHECK(reg.Find("R.SHADOWS.CASCADES") == i && reg.Find("nope") == nullptr);
    CHECK(reg.Register("R.Shadows.Cascades", CVarType::Int, "1", "") == i && i->GetInt() == 3); // the value stays
    CHECK(reg.Register("r.shadows.cascades", CVarType::Float, "1", "") == nullptr);
    CHECK(reg.Register("has space", CVarType::Int, "1", "") == nullptr && reg.Register("", CVarType::Int, "1", "") == nullptr);
    CHECK(reg.All().size() == 4 && reg.All()[0]->Name() == "player.name");
    CHECK(reg.RegisterCommand("quit", "Quit", [](const std::vector<std::string>&, Console&) {}));
    CHECK(!reg.RegisterCommand("t.scale", "clash", [](const std::vector<std::string>&, Console&) {}));
    CHECK(reg.Register("QUIT", CVarType::Int, "1", "") == nullptr);
    CHECK(reg.FindCommand("Quit") && reg.Commands().size() == 1);
    CHECK(reg.UnregisterCommand("quit") && reg.Unregister("t.scale") && reg.Find("t.scale") == nullptr);
}

AETHER_TEST(CVar_DeclaredWhereUsed) {
    CHECK(g_test_cascades && g_test_cascades.Get() == 4);
    g_test_cascades.Set(9);
    CHECK(g_test_cascades.Get() == 4); // clamped
    g_test_cascades.Set(2);
    CHECK(CVarRegistry::Get().Find("test.shadows.cascades")->GetInt() == 2);
    CHECK(g_test_scale.Get() == 1.0f && g_test_name.Get() == "Ada");
    g_test_name.Set("Lin");
    CHECK(g_test_name.Raw().ValueString() == "Lin");
    Console console; // the global registry
    CHECK(console.Execute("test.ping a b") && g_test_command_calls == 1 && Has(console, "pong 2"));
    CHECK(console.Execute("reset test.shadows.cascades; reset test.player.name") && g_test_cascades.Get() == 4 && g_test_name.Get() == "Ada");
}

AETHER_TEST(Console_RunsStatements) {
    CVarRegistry reg;
    CVar* cascades = reg.Register("r.shadows.cascades", CVarType::Int, "4", "Shadow cascades", CVar_Archive, 1, 4);
    CVar* god = reg.Register("cheat.god", CVarType::Bool, "0", "Invulnerable", CVar_Cheat);
    CVar* version = reg.Register("app.version", CVarType::String, "1.2", "Build", CVar_ReadOnly);
    CVar* name = reg.Register("player.name", CVarType::String, "Ada", "Name");
    CVar* restart = reg.Register("r.api", CVarType::String, "vulkan", "Graphics API", CVar_RequiresRestart);
    std::vector<std::string> got;
    reg.RegisterCommand("give", "Gives an item", [&](const std::vector<std::string>& args, Console&) { got = args; });
    reg.RegisterCommand("noclip", "Flies", [](const std::vector<std::string>&, Console&) {}, CVar_Cheat);
    Console c(reg);

    CHECK(c.Execute("r.shadows.cascades 2") && cascades->GetInt() == 2 && Has(c, "r.shadows.cascades = 2"));
    CHECK(c.Execute("R.Shadows.Cascades") && Has(c, "default 4") && Has(c, "int 1..4") && Has(c, "saved"));
    // Several statements, quotes, comments.
    CHECK(c.Execute("player.name \"Grace Hopper\"; r.shadows.cascades 3 // set both"));
    CHECK(name->GetString() == "Grace Hopper" && cascades->GetInt() == 3);
    CHECK(c.Execute("player.name Ada Lovelace") && name->GetString() == "Ada Lovelace"); // strings take the rest
    CHECK(c.Execute("give \"health pack\" 3 # a comment") && got == (std::vector<std::string>{"health pack", "3"}));
    CHECK(Console::Tokenize("echo \"a \\\"quoted\\\" word\" b") == (std::vector<std::string>{"echo", "a \"quoted\" word", "b"}));
    CHECK(Console::SplitStatements("a 1; b \"x;y\"; ;  ") == (std::vector<std::string>{"a 1", " b \"x;y\""}));
    // Refusals.
    CHECK(!c.Execute("app.version 2.0") && version->GetString() == "1.2" && Has(c, "read-only"));
    CHECK(!c.Execute("cheat.god 1") && !god->GetBool() && Has(c, "cheats are off"));
    CHECK(!c.Execute("noclip") && Has(c, "noclip is a cheat"));
    c.allow_cheats = true;
    CHECK(c.Execute("cheat.god 1; noclip") && god->GetBool());
    CHECK(!c.Execute("r.shadows.cascades lots") && Has(c, "whole number") && cascades->GetInt() == 3);
    CHECK(!c.Execute("r.shadow") && Has(c, "unknown command or variable 'r.shadow'") && Has(c, "did you mean"));
    CHECK(c.Execute("r.api dx12") && Has(c, "after a restart"));
    CHECK(!c.Execute("r.shadows.cascades 2; nope; player.name x") && cascades->GetInt() == 2 && name->GetString() == "x");
    // Built-ins.
    CHECK(c.Execute("set r.shadows.cascades 1") && cascades->GetInt() == 1);
    CHECK(!c.Execute("set nothing 1") && !c.Execute("set") && Has(c, "usage: set name value"));
    CHECK(c.Execute("reset r.shadows.cascades") && cascades->GetInt() == 4);
    CHECK(c.Execute("toggle cheat.god") && !god->GetBool() && c.Execute("toggle cheat.god") && god->GetBool());
    CHECK(!c.Execute("toggle player.name"));
    CHECK(c.Execute("echo hello   world") && Has(c, "hello world"));
    CHECK(c.Execute("find shadow") && Has(c, "Shadow cascades"));
    CHECK(c.Execute("find zzz") && Has(c, "nothing matches"));
    CHECK(c.Execute("help r.") && Has(c, "r.api = ") && c.Execute("help") && Has(c, "5 variables and 2 commands"));
    CHECK(c.Execute("help give") && Has(c, "give  (command)  Gives an item"));
    c.Clear();
    CHECK(c.Execute("cvarlist player") && Has(c, "player.name = x") && !Has(c, "r.api"));
    CHECK(c.Execute("clear") && c.Output().empty());
    (void)restart;
}

AETHER_TEST(Console_CompletionHistoryConfigsAndCommandLine) {
    CVarRegistry reg;
    CVar* cascades = reg.Register("r.shadows.cascades", CVarType::Int, "4", "", CVar_Archive, 1, 4);
    CVar* quality = reg.Register("r.shadows.quality", CVarType::String, "high", "", CVar_Archive);
    CVar* gamma = reg.Register("r.gamma", CVarType::Float, "2.2", "", CVar_Archive);
    CVar* temp = reg.Register("r.temp", CVarType::Int, "0", ""); // not archived
    reg.RegisterCommand("respawn", "", [](const std::vector<std::string>&, Console&) {});
    Console c(reg);
    // Completion.
    CHECK(c.Complete("r.sh") == (std::vector<std::string>{"r.shadows.cascades", "r.shadows.quality"}));
    CHECK(c.Complete("RE") == (std::vector<std::string>{"reset", "respawn"}));
    CHECK(c.CompleteLine("r.sh") == "r.shadows." && c.CompleteLine("r.shadows.c") == "r.shadows.cascades ");
    CHECK(c.CompleteLine("echo x; r.g") == "echo x; r.gamma " && c.CompleteLine("zz") == "zz");
    CHECK(c.CompleteLine("r.gamma 1") == "r.gamma 1"); // only the name completes
    // History: newest last, no repeats, bounded.
    c.Execute("r.gamma 2");
    c.Execute("r.temp 1");
    c.Execute("r.gamma 2");
    CHECK(c.History() == (std::vector<std::string>{"r.temp 1", "r.gamma 2"}));
    c.max_history = 3;
    for (int n = 0; n < 5; ++n) c.Execute("echo " + std::to_string(n));
    CHECK(c.History().size() == 3 && c.History().back() == "echo 4");
    // Config files: archived changes only, read back.
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "aether_test_console";
    std::filesystem::create_directories(dir);
    const std::string cfg = (dir / "user.cfg").string();
    c.Execute("reset r.gamma; r.shadows.cascades 2; r.shadows.quality \"very high\"; r.temp 7");
    CHECK(c.Execute("writeconfig " + cfg));
    std::ifstream in(cfg);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(text.find("r.shadows.cascades 2") != std::string::npos && text.find("r.shadows.quality \"very high\"") != std::string::npos);
    CHECK(text.find("r.temp") == std::string::npos && text.find("r.gamma") == std::string::npos);
    in.close(); // Windows does not allow deleting a directory with an open file.
    c.Execute("reset r.shadows.cascades; reset r.shadows.quality");
    CHECK(c.ExecFile(cfg) && cascades->GetInt() == 2 && quality->GetString() == "very high");
    CHECK(!c.Execute("exec " + (dir / "missing.cfg").string()));
    {
        std::ofstream loop(dir / "loop.cfg");
        loop << "exec " << (dir / "loop.cfg").string() << "\n";
    }
    CHECK(c.ExecFile((dir / "loop.cfg").string()) && Has(c, "too deeply nested"));
    std::filesystem::remove_all(dir);
    // The command line.
    const char* argv[] = {"game.exe", "-windowed", "+r.gamma", "1.8", "+r.shadows.quality", "low", "medium", "+echo", "hi"};
    CHECK(c.ApplyCommandLine(9, argv) == 3);
    CHECK(gamma->GetFloat() == 1.8 && quality->GetString() == "low medium" && Has(c, "hi"));
    (void)temp;
}

AETHER_TEST(Console_CapturesTheLogFromAnyThread) {
    Logger& log = Logger::Instance();
    log.SetStdout(false);
    std::vector<LogLine> seen;
    const int sink = log.AddSink([&](const LogLine& l) { seen.push_back(l); });
    AETHER_LOG_WARN("Test", "sink %d", 1);
    CHECK(seen.size() == 1 && seen[0].level == LogLevel::Warn && seen[0].category == "Test" && seen[0].message == "sink 1");
    CHECK(!log.Recent().empty() && log.Recent().back().message == "sink 1");
    log.RemoveSink(sink);
    AETHER_LOG_INFO("Test", "unheard");
    CHECK(seen.size() == 1);

    CVarRegistry reg;
    Console c(reg);
    c.CaptureLog(true);
    CHECK(c.CapturingLog());
    std::thread worker([] {
        for (int i = 0; i < 50; ++i) AETHER_LOG_INFO("Worker", "line %d", i);
    });
    worker.join();
    AETHER_LOG_ERROR("Main", "boom");
    c.Pump();
    usize lines = 0;
    for (const ConsoleLine& l : c.Output()) lines += l.text.rfind("Worker: line ", 0) == 0;
    CHECK(lines == 50 && Has(c, "Main: boom") && c.Output().back().level == LogLevel::Error);
    c.CaptureLog(false);
    AETHER_LOG_INFO("Test", "after");
    c.Pump();
    CHECK(!Has(c, "after"));
    c.max_output = 10;
    c.Print("x");
    CHECK(c.Output().size() == 10);
    log.ClearRecent();
    CHECK(log.Recent().empty());
    log.SetStdout(true);
}
