#include "aether/script/luau_host.h"

#include "script_values.h"

#include "aether/core/log.h"
#include "aether/platform/filesystem.h"

#include <lua.h>
#include <luacode.h>
#include <lualib.h>

#include <cstdlib>
#include <cstring>

namespace aether::script {

namespace {

u64 HashSource(std::string_view source) {
    u64 hash = 0xcbf29ce484222325ull;
    for (unsigned char c : source) {
        hash ^= c;
        hash *= 0x100000001b3ull;
    }
    return hash;
}

} // namespace

void PushScriptValue(lua_State* L, const ScriptValue& value) {
    if (const bool* b = std::get_if<bool>(&value)) {
        lua_pushboolean(L, *b ? 1 : 0);
    } else if (const f64* n = std::get_if<f64>(&value)) {
        lua_pushnumber(L, *n);
    } else if (const std::string* s = std::get_if<std::string>(&value)) {
        lua_pushlstring(L, s->data(), s->size());
    } else if (const EntityRef* e = std::get_if<EntityRef>(&value)) {
        PushEntityValue(L, e->entity);
    } else {
        lua_pushnil(L);
    }
}

// Other Luau types (tables, functions, userdata) come back as nil for now.
ScriptValue ReadScriptValue(lua_State* L, int index) {
    switch (lua_type(L, index)) {
    case LUA_TBOOLEAN: return lua_toboolean(L, index) != 0;
    case LUA_TNUMBER: return static_cast<f64>(lua_tonumber(L, index));
    case LUA_TSTRING: {
        size_t length = 0;
        const char* text = lua_tolstring(L, index, &length);
        return std::string(text, length);
    }
    case LUA_TUSERDATA: {
        Entity entity;
        if (ReadEntityValue(L, index, entity)) {
            return EntityRef{entity};
        }
        return std::monostate{};
    }
    default: return std::monostate{};
    }
}

void InstallDebugDrawBindings(lua_State* L); // debug_bindings.cpp

namespace {

LuauHost* HostOf(lua_State* L) { return static_cast<LuauHost*>(lua_callbacks(L)->userdata); }

} // namespace

LuauHost::LuauHost() : LuauHost(Options{}) {}

LuauHost::LuauHost(Options options) : options_(options) {
    state_ = lua_newstate(&LuauHost::Allocate, this);
    AETHER_ASSERT(state_ != nullptr);
    lua_callbacks(state_)->userdata = this;
    lua_callbacks(state_)->interrupt = &LuauHost::Interrupt;
    luaL_openlibs(state_);

    // Sandbox (§11.4): os only as os.clock; no debug (unless allowed); no loadstring.
    lua_getglobal(state_, "os");
    lua_getfield(state_, -1, "clock");
    lua_newtable(state_);
    lua_insert(state_, -2);
    lua_setfield(state_, -2, "clock");
    lua_setglobal(state_, "os");
    lua_pop(state_, 1); // the original os table
    if (!options_.allow_debug) {
        lua_pushnil(state_);
        lua_setglobal(state_, "debug");
    }
    lua_pushnil(state_);
    lua_setglobal(state_, "loadstring");

    lua_pushcfunction(state_, &LuauHost::Print, "print");
    lua_setglobal(state_, "print");

    print_ = [](const std::string& text) { AETHER_LOG_INFO("Script", "%s", text.c_str()); };
    InstallWorldBindings();
    InstallDebugDrawBindings(state_);
}

LuauHost::~LuauHost() {
    if (state_ != nullptr) {
        lua_close(state_);
    }
}

void* LuauHost::Allocate(void* ud, void* ptr, size_t old_size, size_t new_size) {
    LuauHost* host = static_cast<LuauHost*>(ud);
    if (new_size == 0) {
        std::free(ptr);
        host->memory_used_ -= old_size;
        return nullptr;
    }
    // Luau passes a type tag, not a size, as old_size for new blocks.
    const usize previous = ptr != nullptr ? old_size : 0;
    if (host->options_.memory_limit != 0 && new_size > previous && host->state_ != nullptr &&
        host->memory_used_ + (new_size - previous) > host->options_.memory_limit) {
        return nullptr; // Luau raises "not enough memory"
    }
    void* block = std::realloc(ptr, new_size);
    if (block != nullptr) {
        host->memory_used_ = host->memory_used_ - previous + new_size;
    }
    return block;
}

void LuauHost::Interrupt(lua_State* L, int gc) {
    if (gc >= 0) {
        return; // a GC step: must not raise errors
    }
    LuauHost* host = HostOf(L);
    if (host->options_.instruction_budget != 0 && ++host->ticks_ > host->options_.instruction_budget) {
        host->budget_exceeded_ = true;
        luaL_error(L, "script exceeded its instruction budget (%llu); is there an endless loop?",
                   static_cast<unsigned long long>(host->options_.instruction_budget));
    }
}

int LuauHost::Print(lua_State* L) {
    std::string line;
    const int count = lua_gettop(L);
    for (int i = 1; i <= count; ++i) {
        size_t length = 0;
        const char* text = luaL_tolstring(L, i, &length);
        if (i > 1) {
            line.push_back('\t');
        }
        line.append(text, length);
        lua_pop(L, 1);
    }
    if (LuauHost* host = HostOf(L); host->print_) {
        host->print_(line);
    }
    return 0;
}

bool LuauHost::ProtectedCall(int args, int results, std::string* error) {
    const int base = lua_gettop(state_) - args - 1; // below the function
    ticks_ = 0;
    budget_exceeded_ = false;
    if (lua_pcall(state_, args, results, 0) == LUA_OK) {
        return true;
    }
    const char* message = lua_tostring(state_, -1);
    if (error != nullptr) {
        *error = message != nullptr ? message : "unknown script error";
    }
    lua_settop(state_, base);
    // Whatever the failed call allocated is garbage now; collect it, so
    // a script that hit the memory limit doesn't leave the VM full.
    lua_gc(state_, LUA_GCCOLLECT, 0);
    return false;
}

ScriptResult LuauHost::CallTop(int args, const std::string& what) {
    ScriptResult result;
    const int base = lua_gettop(state_) - args - 1;
    if (!ProtectedCall(args, LUA_MULTRET, &result.error)) {
        if (result.error.empty()) {
            result.error = "unknown error in " + what;
        }
        return result;
    }
    for (int i = base + 1; i <= lua_gettop(state_); ++i) {
        result.values.push_back(ReadScriptValue(state_, i));
    }
    lua_settop(state_, base);
    result.ok = true;
    return result;
}

bool LuauHost::LoadChunk(std::string_view source, const std::string& chunk_name, std::string* error) {
    const u64 key = HashSource(source);
    auto cached = bytecode_.find(key);
    if (cached == bytecode_.end()) {
        size_t size = 0;
        lua_CompileOptions compile{};
        compile.optimizationLevel = 1;
        compile.debugLevel = options_.allow_debug ? 2 : 1; // 2 keeps local names
        char* code = luau_compile(source.data(), source.size(), &compile, &size);
        cached = bytecode_.emplace(key, std::string(code, size)).first;
        std::free(code);
    }
    const std::string chunk = "=" + chunk_name;
    if (luau_load(state_, chunk.c_str(), cached->second.data(), cached->second.size(), 0) != 0) {
        const char* message = lua_tostring(state_, -1);
        if (error != nullptr) {
            *error = message != nullptr ? message : "couldn't load " + chunk_name;
        }
        lua_pop(state_, 1);
        bytecode_.erase(cached); // a syntax error: don't keep it
        return false;
    }
    // Remember the latest function for this chunk (for late breakpoints).
    lua_pushvalue(state_, -1);
    const int ref = lua_ref(state_, -1);
    lua_pop(state_, 1);
    if (auto it = chunk_functions_.find(chunk_name); it != chunk_functions_.end()) {
        lua_unref(state_, it->second);
        it->second = ref;
    } else {
        chunk_functions_.emplace(chunk_name, ref);
    }
    if (on_chunk_loaded_) {
        on_chunk_loaded_(chunk_name);
    }
    return true;
}

int LuauHost::ChunkFunction(const std::string& chunk) const {
    auto it = chunk_functions_.find(chunk);
    return it != chunk_functions_.end() ? it->second : LUA_NOREF;
}

ScriptResult LuauHost::Run(std::string_view source, const std::string& chunk_name) {
    ScriptResult result;
    if (!LoadChunk(source, chunk_name, &result.error)) {
        return result;
    }
    return CallTop(0, chunk_name);
}

ScriptResult LuauHost::RunFile(const std::filesystem::path& file) {
    std::vector<u8> bytes;
    if (!fs::ReadFileBytes(file.string(), bytes)) {
        ScriptResult result;
        result.error = "Couldn't read " + file.string();
        return result;
    }
    return Run(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), file.filename().string());
}

ScriptResult LuauHost::Call(const std::string& function, const std::vector<ScriptValue>& args) {
    lua_getglobal(state_, function.c_str());
    if (lua_type(state_, -1) != LUA_TFUNCTION) {
        lua_pop(state_, 1);
        ScriptResult result;
        result.error = "No function named '" + function + "'";
        return result;
    }
    for (const ScriptValue& arg : args) {
        PushScriptValue(state_, arg);
    }
    return CallTop(static_cast<int>(args.size()), function);
}

usize NativeCall::Count() const { return static_cast<usize>(lua_gettop(state_)); }
bool NativeCall::IsString(usize i) const { return lua_type(state_, static_cast<int>(i) + 1) == LUA_TSTRING; }
bool NativeCall::IsNumber(usize i) const { return lua_type(state_, static_cast<int>(i) + 1) == LUA_TNUMBER; }
bool NativeCall::IsBool(usize i) const { return lua_type(state_, static_cast<int>(i) + 1) == LUA_TBOOLEAN; }
bool NativeCall::IsFunction(usize i) const { return lua_type(state_, static_cast<int>(i) + 1) == LUA_TFUNCTION; }

bool NativeCall::IsEntity(usize i) const {
    Entity unused;
    return ReadEntityValue(state_, static_cast<int>(i) + 1, unused);
}

Entity NativeCall::EntityArg(usize i) const {
    Entity entity;
    return ReadEntityValue(state_, static_cast<int>(i) + 1, entity) ? entity : Entity{};
}

std::string NativeCall::String(usize i, const std::string& fallback) const {
    if (!IsString(i)) return fallback;
    size_t length = 0;
    const char* text = lua_tolstring(state_, static_cast<int>(i) + 1, &length);
    return std::string(text, length);
}

f64 NativeCall::Number(usize i, f64 fallback) const {
    return IsNumber(i) ? lua_tonumber(state_, static_cast<int>(i) + 1) : fallback;
}

bool NativeCall::Bool(usize i, bool fallback) const {
    return IsBool(i) ? lua_toboolean(state_, static_cast<int>(i) + 1) != 0 : fallback;
}

int NativeCall::Function(usize i) const {
    return IsFunction(i) ? lua_ref(state_, static_cast<int>(i) + 1) : -1;
}

void NativeCall::Return(const ScriptValue& value) {
    PushScriptValue(state_, value);
    ++returns_;
}

int NativeTrampoline(lua_State* L) {
    NativeFunction* function = static_cast<NativeFunction*>(lua_touserdata(L, lua_upvalueindex(1)));
    std::string error;
    int returns = 0;
    {
        NativeCall call(L);
        (*function)(call);
        error = call.error_;
        returns = call.returns_;
    }
    if (!error.empty()) luaL_error(L, "%s", error.c_str());
    return returns;
}

void LuauHost::RegisterNative(const std::string& table, const std::string& name, NativeFunction function) {
    natives_.push_back(std::make_unique<NativeFunction>(std::move(function)));
    lua_pushlightuserdata(state_, natives_.back().get());
    lua_pushcclosure(state_, &NativeTrampoline, name.c_str(), 1);
    if (table.empty()) {
        lua_setglobal(state_, name.c_str());
        return;
    }
    lua_getglobal(state_, table.c_str());
    if (!lua_istable(state_, -1)) {
        lua_pop(state_, 1);
        lua_newtable(state_);
        lua_pushvalue(state_, -1);
        lua_setglobal(state_, table.c_str());
    }
    lua_insert(state_, -2); // table, function
    lua_setfield(state_, -2, name.c_str());
    lua_pop(state_, 1);
}

ScriptResult LuauHost::CallRef(int ref, const std::vector<ScriptValue>& args) {
    ScriptResult result;
    lua_getref(state_, ref);
    if (!lua_isfunction(state_, -1)) {
        lua_pop(state_, 1);
        result.error = "not a function";
        return result;
    }
    for (const ScriptValue& a : args) PushScriptValue(state_, a);
    return CallTop(static_cast<int>(args.size()), "callback");
}

void LuauHost::Release(int ref) {
    if (ref >= 0) lua_unref(state_, ref);
}

ScriptValue LuauHost::GetGlobal(const std::string& name) {
    lua_getglobal(state_, name.c_str());
    ScriptValue value = ReadScriptValue(state_, -1);
    lua_pop(state_, 1);
    return value;
}

void LuauHost::SetGlobal(const std::string& name, const ScriptValue& value) {
    PushScriptValue(state_, value);
    lua_setglobal(state_, name.c_str());
}

} // namespace aether::script
