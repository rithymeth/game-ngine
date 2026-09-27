#pragma once

#include "aether/core/base.h"

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

struct lua_State;

namespace aether::script {

// A value crossing between C++ and Luau: nil, boolean, number or string.
// (Engine types, entities and components come with the reflection bindings.)
using ScriptValue = std::variant<std::monostate, bool, f64, std::string>;

struct ScriptResult {
    bool ok = false;
    std::string error; // "chunk:line: message" for script errors
    std::vector<ScriptValue> values; // what the chunk or function returned
};

// One Luau VM (Phase 11 step 1, docs/design/PHASE_SPECS.md §11.1-11.4).
//
// Sandbox (§11.4): the standard libraries math, string, table, bit32, utf8,
// buffer, coroutine and vector; `os` only as os.clock; no `debug` unless
// Options::allow_debug (editor builds, for the debugger); no loadstring.
// Limits: each top-level run or call gets an instruction budget (enforced
// through Luau's interrupt callback), and the VM's memory is capped by its
// allocator. Both raise ordinary script errors, so a runaway script fails
// cleanly instead of hanging or exhausting memory.
class LuauHost {
public:
    struct Options {
        usize memory_limit = 64u * 1024u * 1024u; // bytes; 0 = no limit
        // Interrupt checks (function calls and loop iterations, roughly) per
        // top-level run or call; 0 = no limit.
        u64 instruction_budget = 10'000'000;
        bool allow_debug = false;
    };

    LuauHost();
    explicit LuauHost(Options options);
    ~LuauHost();
    LuauHost(const LuauHost&) = delete;
    LuauHost& operator=(const LuauHost&) = delete;

    // Compiles (bytecode cached by source hash) and runs a chunk. Globals it
    // defines stay in the VM. `chunk_name` labels errors.
    ScriptResult Run(std::string_view source, const std::string& chunk_name = "chunk");
    ScriptResult RunFile(const std::filesystem::path& file);

    // Calls a global function.
    ScriptResult Call(const std::string& function, const std::vector<ScriptValue>& args = {});

    ScriptValue GetGlobal(const std::string& name);
    void SetGlobal(const std::string& name, const ScriptValue& value);

    // Where print() goes (default: the engine log, category "Script").
    void SetPrintHandler(std::function<void(const std::string&)> handler) { print_ = std::move(handler); }

    usize MemoryUsed() const { return memory_used_; }
    usize CompiledCount() const { return bytecode_.size(); }
    // The raw VM, for the binding layer.
    lua_State* State() { return state_; }

private:
    static void* Allocate(void* ud, void* ptr, size_t old_size, size_t new_size);
    static void Interrupt(lua_State* state, int gc);
    static int Print(lua_State* state);

    ScriptResult CallTop(int args, const std::string& what); // runs the function on the stack

    Options options_;
    lua_State* state_ = nullptr;
    usize memory_used_ = 0;
    u64 ticks_ = 0;
    bool budget_exceeded_ = false;
    std::function<void(const std::string&)> print_;
    std::unordered_map<u64, std::string> bytecode_;
};

} // namespace aether::script
