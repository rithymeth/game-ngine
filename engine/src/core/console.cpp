#include "aether/core/console.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <memory>
#include <sstream>

namespace aether {

namespace {

const char* const kBuiltins[] = {"help", "find", "cvarlist", "set", "reset", "toggle", "echo", "exec", "writeconfig", "history", "clear"};

std::string Lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

bool StartsWithNoCase(std::string_view s, std::string_view prefix) {
    if (prefix.size() > s.size()) return false;
    for (usize i = 0; i < prefix.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(s[i])) != std::tolower(static_cast<unsigned char>(prefix[i]))) return false;
    return true;
}

std::string Join(const std::vector<std::string>& args, usize from) {
    std::string out;
    for (usize i = from; i < args.size(); ++i) out += (i > from ? " " : "") + args[i];
    return out;
}

std::string Quote(const std::string& s) {
    if (!s.empty() && s.find_first_of(" \t;\"/#") == std::string::npos) return s;
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    return out + "\"";
}

std::string Describe(const CVar& v) {
    std::string flags;
    if (v.HasFlag(CVar_ReadOnly)) flags += " read-only";
    if (v.HasFlag(CVar_Cheat)) flags += " cheat";
    if (v.HasFlag(CVar_Archive)) flags += " saved";
    if (v.HasFlag(CVar_RequiresRestart)) flags += " restart";
    std::string range;
    if (v.HasRange()) range = " " + FormatCVarFloat(v.Min()) + ".." + FormatCVarFloat(v.Max());
    return v.Name() + " = " + Quote(v.ValueString()) + "  (" + CVarTypeName(v.Type()) + range + ", default " +
           Quote(v.DefaultString()) + (flags.empty() ? "" : "," + flags) + ")" + (v.Help().empty() ? "" : "  " + v.Help());
}

} // namespace

// Log lines can arrive from any thread; they wait here until the console's thread takes them (Pump).
struct ConsoleLogQueue {
    std::mutex mutex;
    std::vector<ConsoleLine> lines;
};

Console::Console(CVarRegistry& registry) : registry_(registry) {}

Console::~Console() { CaptureLog(false); }

void Console::Print(std::string text, LogLevel level) {
    Pump();
    output_.push_back({level, std::move(text)});
    while (output_.size() > max_output) output_.pop_front();
}

void Console::Pump() {
    const std::shared_ptr<ConsoleLogQueue> queue = log_queue_;
    if (!queue) return;
    std::vector<ConsoleLine> lines;
    {
        std::lock_guard<std::mutex> lock(queue->mutex);
        lines.swap(queue->lines);
    }
    for (ConsoleLine& l : lines) output_.push_back(std::move(l));
    while (output_.size() > max_output) output_.pop_front();
}

void Console::CaptureLog(bool capture) {
    if (capture == (sink_ != 0)) return;
    if (capture) {
        auto queue = std::make_shared<ConsoleLogQueue>();
        log_queue_ = queue;
        sink_ = Logger::Instance().AddSink([queue](const LogLine& line) {
            std::lock_guard<std::mutex> lock(queue->mutex);
            queue->lines.push_back({line.level, line.category + ": " + line.message});
        });
    } else {
        Logger::Instance().RemoveSink(sink_);
        sink_ = 0;
        Pump();
        log_queue_.reset();
    }
}

std::vector<std::string> Console::SplitStatements(std::string_view line) {
    std::vector<std::string> out;
    std::string current;
    bool quoted = false;
    for (usize i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (quoted && c == '\\' && i + 1 < line.size()) {
            current += c, current += line[++i];
            continue;
        }
        if (c == '"') quoted = !quoted;
        if (!quoted && (c == '#' || (c == '/' && i + 1 < line.size() && line[i + 1] == '/'))) break; // a comment
        if (!quoted && c == ';') {
            out.push_back(std::move(current));
            current.clear();
            continue;
        }
        current += c;
    }
    out.push_back(std::move(current));
    std::erase_if(out, [](const std::string& s) { return s.find_first_not_of(" \t\r\n") == std::string::npos; });
    return out;
}

std::vector<std::string> Console::Tokenize(std::string_view statement) {
    std::vector<std::string> out;
    usize i = 0;
    while (i < statement.size()) {
        while (i < statement.size() && std::isspace(static_cast<unsigned char>(statement[i]))) ++i;
        if (i >= statement.size()) break;
        std::string token;
        if (statement[i] == '"') {
            for (++i; i < statement.size() && statement[i] != '"'; ++i) {
                if (statement[i] == '\\' && i + 1 < statement.size()) ++i;
                token += statement[i];
            }
            ++i; // the closing quote (or the end)
        } else {
            while (i < statement.size() && !std::isspace(static_cast<unsigned char>(statement[i]))) token += statement[i++];
        }
        out.push_back(std::move(token));
    }
    return out;
}

bool Console::Execute(std::string_view line, bool add_to_history) {
    Pump();
    std::string text(line);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.pop_back();
    if (text.find_first_not_of(" \t") == std::string::npos) return true;
    if (add_to_history) {
        std::erase(history_, text);
        history_.push_back(text);
        while (history_.size() > max_history) history_.erase(history_.begin());
        Print("> " + text);
    }
    bool ok = true;
    for (const std::string& statement : SplitStatements(text)) {
        const std::vector<std::string> args = Tokenize(statement);
        if (!args.empty()) ok = RunStatement(args) && ok;
    }
    return ok;
}

bool Console::SetVar(CVar& var, const std::string& value) {
    if (var.HasFlag(CVar_ReadOnly)) {
        Print(var.Name() + " is read-only", LogLevel::Error);
        return false;
    }
    if (var.HasFlag(CVar_Cheat) && !allow_cheats) {
        Print(var.Name() + " is a cheat; cheats are off", LogLevel::Error);
        return false;
    }
    std::string error;
    if (!var.SetFromString(value, &error)) {
        Print(var.Name() + ": " + error, LogLevel::Error);
        return false;
    }
    Print(var.Name() + " = " + Quote(var.ValueString()) +
          (var.HasFlag(CVar_RequiresRestart) ? "  (takes effect after a restart)" : ""));
    return true;
}

bool Console::Builtin(const std::vector<std::string>& args, bool& ok) {
    const std::string name = Lower(args[0]);
    const auto need = [&](usize n, const char* usage) {
        if (args.size() >= n) return true;
        Print(std::string("usage: ") + usage, LogLevel::Error);
        ok = false;
        return false;
    };
    const auto var_arg = [&](const char* usage) -> CVar* {
        if (!need(2, usage)) return nullptr;
        CVar* v = registry_.Find(args[1]);
        if (!v) {
            Print("no variable named '" + args[1] + "'", LogLevel::Error);
            ok = false;
        }
        return v;
    };
    if (name == "help") {
        const std::string prefix = args.size() > 1 ? args[1] : std::string();
        if (prefix.empty()) {
            Print("Type a variable's name to see it, 'name value' to set it, or a command. Built in: help [prefix], "
                  "find text, cvarlist [prefix], set, reset, toggle, echo, exec, writeconfig, history, clear.");
            Print(std::to_string(registry_.All().size()) + " variables and " + std::to_string(registry_.Commands().size()) +
                  " commands; 'help r.' lists those starting with r.");
            return true;
        }
        for (const CVar* v : registry_.All())
            if (StartsWithNoCase(v->Name(), prefix)) Print(Describe(*v));
        for (const ConsoleCommand* c : registry_.Commands())
            if (StartsWithNoCase(c->name, prefix)) Print(c->name + "  (command)  " + c->help);
        return true;
    }
    if (name == "find") {
        if (!need(2, "find text")) return true;
        const std::string text = Lower(Join(args, 1));
        usize found = 0;
        for (const CVar* v : registry_.All())
            if (Lower(v->Name()).find(text) != std::string::npos || Lower(v->Help()).find(text) != std::string::npos)
                Print(Describe(*v)), ++found;
        for (const ConsoleCommand* c : registry_.Commands())
            if (Lower(c->name).find(text) != std::string::npos || Lower(c->help).find(text) != std::string::npos)
                Print(c->name + "  (command)  " + c->help), ++found;
        if (found == 0) Print("nothing matches '" + text + "'");
        return true;
    }
    if (name == "cvarlist") {
        const std::string prefix = args.size() > 1 ? args[1] : std::string();
        for (const CVar* v : registry_.All())
            if (StartsWithNoCase(v->Name(), prefix)) Print(v->Name() + " = " + Quote(v->ValueString()));
        return true;
    }
    if (name == "set") {
        if (CVar* v = var_arg("set name value")) {
            if (need(3, "set name value")) ok = SetVar(*v, Join(args, 2));
        }
        return true;
    }
    if (name == "reset") {
        if (CVar* v = var_arg("reset name")) ok = SetVar(*v, v->DefaultString());
        return true;
    }
    if (name == "toggle") {
        if (CVar* v = var_arg("toggle name")) {
            if (v->Type() == CVarType::String || v->Type() == CVarType::Float) {
                Print(v->Name() + " isn't a bool or an int", LogLevel::Error);
                ok = false;
            } else {
                ok = SetVar(*v, v->GetInt() != 0 ? "0" : "1");
            }
        }
        return true;
    }
    if (name == "echo") {
        Print(Join(args, 1));
        return true;
    }
    if (name == "exec") {
        if (need(2, "exec file") && !ExecFile(args[1])) ok = false;
        return true;
    }
    if (name == "writeconfig") {
        if (need(2, "writeconfig file")) {
            ok = WriteConfig(args[1]);
            Print(ok ? "wrote " + args[1] : "couldn't write " + args[1], ok ? LogLevel::Info : LogLevel::Error);
        }
        return true;
    }
    if (name == "history") {
        for (usize i = 0; i < history_.size(); ++i) Print(std::to_string(i + 1) + "  " + history_[i]);
        return true;
    }
    if (name == "clear") {
        output_.clear();
        return true;
    }
    return false;
}

bool Console::RunStatement(const std::vector<std::string>& args) {
    bool ok = true;
    if (Builtin(args, ok)) return ok;
    if (CVar* var = registry_.Find(args[0])) {
        if (args.size() == 1) {
            Print(Describe(*var));
            return true;
        }
        return SetVar(*var, var->Type() == CVarType::String ? Join(args, 1) : args[1]);
    }
    if (const ConsoleCommand* cmd = registry_.FindCommand(args[0])) {
        if ((cmd->flags & CVar_Cheat) && !allow_cheats) {
            Print(cmd->name + " is a cheat; cheats are off", LogLevel::Error);
            return false;
        }
        const ConsoleCommandFn fn = cmd->fn; // the command may re-register itself
        fn(std::vector<std::string>(args.begin() + 1, args.end()), *this);
        return true;
    }
    std::string message = "unknown command or variable '" + args[0] + "'";
    const std::vector<std::string> close = Complete(args[0].substr(0, std::min<usize>(args[0].size(), 3)));
    if (!close.empty() && close.size() <= 5) {
        message += "; did you mean";
        for (usize i = 0; i < close.size(); ++i) message += (i ? ", " : " ") + close[i];
        message += "?";
    }
    Print(message, LogLevel::Error);
    return false;
}

std::vector<std::string> Console::Complete(std::string_view prefix) const {
    std::vector<std::string> out;
    for (const CVar* v : registry_.All())
        if (StartsWithNoCase(v->Name(), prefix)) out.push_back(v->Name());
    for (const ConsoleCommand* c : registry_.Commands())
        if (StartsWithNoCase(c->name, prefix)) out.push_back(c->name);
    for (const char* b : kBuiltins)
        if (StartsWithNoCase(b, prefix)) out.push_back(b);
    std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) { return Lower(a) < Lower(b); });
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::string Console::CompleteLine(std::string_view line) const {
    // The word being typed: the last statement's first word, with nothing after it yet.
    const usize start = line.find_last_of(';') == std::string_view::npos ? 0 : line.find_last_of(';') + 1;
    usize word = start;
    while (word < line.size() && std::isspace(static_cast<unsigned char>(line[word]))) ++word;
    const std::string_view typed = line.substr(word);
    if (typed.empty() || typed.find_first_of(" \t") != std::string_view::npos) return std::string(line);
    const std::vector<std::string> candidates = Complete(typed);
    if (candidates.empty()) return std::string(line);
    std::string common = candidates[0];
    for (const std::string& c : candidates) {
        usize n = 0;
        while (n < common.size() && n < c.size() &&
               std::tolower(static_cast<unsigned char>(common[n])) == std::tolower(static_cast<unsigned char>(c[n])))
            ++n;
        common.resize(n);
    }
    if (common.size() < typed.size()) return std::string(line);
    return std::string(line.substr(0, word)) + common + (candidates.size() == 1 ? " " : "");
}

bool Console::ExecFile(const std::string& path) {
    if (exec_depth_ >= 8) {
        Print("exec: too deeply nested at " + path, LogLevel::Error);
        return false;
    }
    std::ifstream in(path);
    if (!in) {
        Print("exec: couldn't read " + path, LogLevel::Error);
        return false;
    }
    Print("exec " + path);
    ++exec_depth_;
    std::string line;
    while (std::getline(in, line)) Execute(line, false);
    --exec_depth_;
    return true;
}

bool Console::WriteConfig(const std::string& path) const {
    std::ofstream out(path);
    if (!out) return false;
    out << "// Aether console variables (written by writeconfig; values that differ from their defaults)\n";
    for (const CVar* v : registry_.All())
        if (v->HasFlag(CVar_Archive) && !v->HasFlag(CVar_ReadOnly) && !v->IsDefault())
            out << v->Name() << ' ' << Quote(v->ValueString()) << '\n';
    return static_cast<bool>(out);
}

usize Console::ApplyCommandLine(int argc, const char* const* argv) {
    std::vector<std::string> statements;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i] ? argv[i] : "";
        if (!arg.empty() && arg[0] == '+') statements.push_back(Quote(arg.substr(1)));
        else if (!statements.empty()) statements.back() += " " + Quote(arg);
    }
    usize ran = 0;
    for (const std::string& s : statements)
        if (Execute(s, false)) ++ran;
    return ran;
}

} // namespace aether
