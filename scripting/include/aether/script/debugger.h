#pragma once

#include "aether/core/base.h"

#include <functional>
#include <map>
#include <string>
#include <vector>

struct lua_State;
struct lua_Debug;

namespace aether::script {

class LuauHost;

// A variable as the debugger shows it: its name, a short printable value
// ("12", "\"hello\"", "table", "Entity(3v1)") and its Luau type name.
struct DebugVariable {
    std::string name;
    std::string value;
    std::string type;
};

struct DebugFrame {
    std::string function; // "" for a chunk's top level
    std::string chunk;    // the chunk name scripts were loaded with
    int line = 0;         // -1 for C functions
    bool is_native = false; // a C function (no locals, no line)
    std::vector<DebugVariable> locals; // Luau functions only; temporaries left out
};

enum class StopReason {
    Breakpoint,
    Step,  // a StepInto/StepOver/StepOut finished
    Pause, // RequestPause
};

// Where execution stopped: the innermost frame first.
struct DebugStop {
    StopReason reason = StopReason::Breakpoint;
    std::string chunk;
    int line = 0;
    std::vector<DebugFrame> frames;
};

// What to do after a stop.
enum class DebugAction {
    Continue,
    StepInto, // stop at the next line, entering calls
    StepOver, // stop at the next line of this function (or its caller, after a return)
    StepOut,  // stop in the caller, after this function returns
};

// Called at each stop, on the thread running the script (the script is
// paused until it returns). The editor runs a nested UI loop in here; the
// DAP server blocks on its client. It must not run scripts on the same VM.
using DebugHandler = std::function<DebugAction(const DebugStop& stop)>;

// The script debugger core (Phase 11 step 6, docs/design/PHASE_SPECS.md
// §11.5): breakpoints and stepping on one LuauHost, built on Luau's
// breakpoint instructions and single-step callback.
//
// - Breakpoints are per chunk name and line. They can be set before the
//   chunk is loaded, and are re-applied whenever it's (re)loaded, so they
//   survive hot reload. A line with no code moves to the next line that has
//   some (the line actually used is returned).
// - While attached, the VM runs in single-step mode (every instruction calls
//   back; slower, which is fine for a debugging session); stepping needs it.
// - The host should be created with Options::allow_debug, so local
//   variables have names.
class ScriptDebugger {
public:
    explicit ScriptDebugger(LuauHost& host);
    ~ScriptDebugger();
    ScriptDebugger(const ScriptDebugger&) = delete;
    ScriptDebugger& operator=(const ScriptDebugger&) = delete;

    void SetHandler(DebugHandler handler) { handler_ = std::move(handler); }

    // Returns the line the breakpoint landed on, or `line` itself while the
    // chunk isn't loaded yet (resolved when it is), or -1 if the chunk is
    // loaded and has no code at or after `line`.
    int SetBreakpoint(const std::string& chunk, int line);
    // By the line asked for or the line it landed on.
    bool ClearBreakpoint(const std::string& chunk, int line);
    void ClearBreakpoints(const std::string& chunk);
    void ClearAllBreakpoints();
    // Whether a chunk of this name has been loaded (so its breakpoints are
    // resolved to real lines).
    bool IsLoaded(const std::string& chunk) const;
    // Lines with a breakpoint in `chunk` (as landed, when known), sorted.
    std::vector<int> Breakpoints(const std::string& chunk) const;

    // Stops at the next line any script runs (the editor's Pause button).
    void RequestPause();

    usize StopCount() const { return stops_; }

private:
    struct Breakpoint {
        int requested = 0;
        int actual = 0; // 0 while the chunk isn't loaded
    };
    enum class StepMode { None, Into, Over, Out, Pause };
    struct Location {
        int depth = -1;
        int proto = 0;
        int line = 0;
        bool operator==(const Location& o) const {
            return depth == o.depth && proto == o.proto && line == o.line;
        }
    };

    static void OnBreak(lua_State* state, lua_Debug* ar);
    static void OnStep(lua_State* state, lua_Debug* ar);
    static ScriptDebugger* Of(lua_State* state);

    void ApplyBreakpoints(const std::string& chunk);
    int ApplyBreakpoint(const std::string& chunk, int line, bool enabled);
    Location Here(lua_State* state) const;
    void Stop(lua_State* state, StopReason reason, const Location& here);
    DebugStop Describe(lua_State* state, StopReason reason) const;

    LuauHost& host_;
    DebugHandler handler_;
    std::map<std::string, std::vector<Breakpoint>> breakpoints_;
    StepMode step_ = StepMode::None;
    Location step_from_;
    // After a stop, breakpoints on the same line don't stop again until
    // execution has left it (a line is several instructions, each with its
    // own break instruction).
    Location suppress_;
    bool suppressing_ = false;
    bool stopped_ = false; // inside the handler
    usize stops_ = 0;
};

} // namespace aether::script
