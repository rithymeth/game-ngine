#pragma once

#include "aether/core/cvar.h"
#include "aether/core/log.h"

#include <deque>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace aether {

struct ConsoleLogQueue;

struct ConsoleLine {
    LogLevel level = LogLevel::Info;
    std::string text;
};

// The developer console (Phase 23 step 1, docs/design/PHASE_SPECS.md §23.1):
// runs lines against the CVar registry -
//   `name`         prints the variable (value, default, range, help);
//   `name value`   sets it (ReadOnly refused; Cheat only with allow_cheats);
//   `command args` runs a registered command;
// with `;` between statements, `//` and `#` starting comments, and quotes
// for arguments with spaces. Built in: help [prefix], find text, cvarlist
// [prefix], set name value, reset name, toggle name, echo text, exec file,
// writeconfig file, history, clear. Keeps an output buffer, a history and
// completion; the editor's console panel and the in-game overlay draw it.
class Console {
public:
    explicit Console(CVarRegistry& registry = CVarRegistry::Get());
    ~Console();
    Console(const Console&) = delete;
    Console& operator=(const Console&) = delete;

    // False when any statement failed (unknown name, bad value, refused).
    bool Execute(std::string_view line, bool add_to_history = true);
    void Print(std::string text, LogLevel level = LogLevel::Info);

    const std::deque<ConsoleLine>& Output() const { return output_; }
    void Clear() { output_.clear(); }
    usize max_output = 2000;

    const std::vector<std::string>& History() const { return history_; }
    usize max_history = 64;

    // Names (variables, commands, built-ins) starting with `prefix`, case-insensitively, sorted.
    std::vector<std::string> Complete(std::string_view prefix) const;
    // Tab: completes the line's first word to the candidates' longest common
    // prefix (and a space when there is one candidate). Unchanged when none.
    std::string CompleteLine(std::string_view line) const;

    // Runs a config file, line by line. False when it can't be read.
    bool ExecFile(const std::string& path);
    // Writes `name value` for every Archive variable not at its default.
    bool WriteConfig(const std::string& path) const;
    // "+name value ... +command args ..." from a command line: each `+` starts a statement.
    usize ApplyCommandLine(int argc, const char* const* argv);

    // Copies log lines (from any thread) into the output while on; they
    // arrive on the next Pump (Print and Execute pump too).
    void CaptureLog(bool capture);
    void Pump();
    bool CapturingLog() const { return sink_ != 0; }

    bool allow_cheats = false;

    static std::vector<std::string> Tokenize(std::string_view statement);
    static std::vector<std::string> SplitStatements(std::string_view line);

private:
    bool RunStatement(const std::vector<std::string>& args);
    bool Builtin(const std::vector<std::string>& args, bool& ok);
    bool SetVar(CVar& var, const std::string& value);

    CVarRegistry& registry_;
    std::deque<ConsoleLine> output_;
    std::vector<std::string> history_;
    int sink_ = 0;
    std::shared_ptr<ConsoleLogQueue> log_queue_;
    int exec_depth_ = 0;
};

} // namespace aether
