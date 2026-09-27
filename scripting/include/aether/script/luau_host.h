#pragma once

#include "aether/core/base.h"
#include "aether/ecs/entity.h"

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

struct lua_State;

namespace aether {
class World;
class GuidIndex;
} // namespace aether

namespace aether::script {

// An entity passed to or from a script (needs a bound world; see BindWorld).
struct EntityRef {
    Entity entity;
    bool operator==(const EntityRef& o) const { return entity == o.entity; }
};

// A value crossing between C++ and Luau: nil, boolean, number, string or
// entity. (Other Luau values come back as nil.)
using ScriptValue = std::variant<std::monostate, bool, f64, std::string, EntityRef>;

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
        // Also compiles with full debug info (local names), for the debugger.
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

    // Gives scripts access to a world (Phase 11 step 2, §11.1). Scripts then
    // have a global `world` (Spawn, Destroy, Find, EntitiesWith) and entity
    // values with :Get/:Add/:Has/:Remove/:IsValid/:Guid; components expose
    // their reflected fields (read and write) and functions (as methods).
    // Entities and components are handles, re-checked on every use: using
    // one whose entity was destroyed, whose component was removed, or from
    // before the world was re-bound raises a script error, never a crash.
    // Pass nullptr to unbind.
    void BindWorld(World* world, GuidIndex* guids);
    World* BoundWorld() const { return world_; }

    // Where print() goes (default: the engine log, category "Script").
    void SetPrintHandler(std::function<void(const std::string&)> handler) { print_ = std::move(handler); }

    usize MemoryUsed() const { return memory_used_; }
    usize CompiledCount() const { return bytecode_.size(); }
    // The raw VM, for the binding layer.
    lua_State* State() { return state_; }

    // Lower-level pieces for the binding layer (ScriptSystem):
    // Compiles (cached) and pushes the chunk as a function. False with `error`.
    bool LoadChunk(std::string_view source, const std::string& chunk_name, std::string* error);
    // Calls the function below `args` arguments on the stack, with the
    // instruction budget reset, leaving `results` values (LUA_MULTRET = -1
    // for all). On failure the stack is restored and garbage collected.
    bool ProtectedCall(int args, int results, std::string* error);

    // Called with each newly loaded chunk's function on top of the stack
    // (the debugger applies breakpoints here), and the latest function
    // loaded for a chunk name, as a registry ref (LUA_NOREF if none).
    void SetChunkLoadedCallback(std::function<void(const std::string& chunk)> callback) {
        on_chunk_loaded_ = std::move(callback);
    }
    int ChunkFunction(const std::string& chunk) const;

    // The ScriptDebugger attached to this VM, if any (it installs Luau's
    // debug callbacks, which find it through here).
    void SetDebugger(void* debugger) { debugger_ = debugger; }
    void* Debugger() const { return debugger_; }

private:
    static void* Allocate(void* ud, void* ptr, size_t old_size, size_t new_size);
    static void Interrupt(lua_State* state, int gc);
    static int Print(lua_State* state);
    friend struct BindingAccess;

    ScriptResult CallTop(int args, const std::string& what); // runs the function on the stack

    void InstallWorldBindings();

    Options options_;
    lua_State* state_ = nullptr;
    World* world_ = nullptr;
    GuidIndex* guids_ = nullptr;
    u64 binding_generation_ = 1;
    usize memory_used_ = 0;
    u64 ticks_ = 0;
    bool budget_exceeded_ = false;
    std::function<void(const std::string&)> print_;
    std::unordered_map<u64, std::string> bytecode_;
    std::unordered_map<std::string, int> chunk_functions_; // chunk name -> registry ref
    std::function<void(const std::string& chunk)> on_chunk_loaded_;
    void* debugger_ = nullptr;
};

} // namespace aether::script
