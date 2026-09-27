#include "aether/script/debugger.h"

#include "aether/script/luau_host.h"

#include <lua.h>
#include <lualib.h>

#include <algorithm>
#include <cstdio>

namespace aether::script {

namespace {

constexpr usize kMaxValueText = 80;

std::string ChunkOf(const lua_Debug& ar) {
    const char* source = ar.source != nullptr ? ar.source : "";
    return source[0] == '=' || source[0] == '@' ? std::string(source + 1) : std::string(source);
}

// A short printable form of the value on top of the stack, without running
// script code (a table's __tostring could loop or error inside the hook).
std::string ValueText(lua_State* L, int index) {
    switch (lua_type(L, index)) {
    case LUA_TNIL: return "nil";
    case LUA_TBOOLEAN: return lua_toboolean(L, index) != 0 ? "true" : "false";
    case LUA_TNUMBER: {
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "%.14g", lua_tonumber(L, index));
        return buffer;
    }
    case LUA_TSTRING: {
        size_t length = 0;
        const char* text = lua_tolstring(L, index, &length);
        std::string quoted = "\"";
        quoted.append(text, std::min<size_t>(length, kMaxValueText));
        if (length > kMaxValueText) {
            quoted += "...";
        }
        return quoted + "\"";
    }
    case LUA_TVECTOR: {
        const float* v = lua_tovector(L, index);
        char buffer[96];
        std::snprintf(buffer, sizeof(buffer), "(%g, %g, %g)", v[0], v[1], v[2]);
        return buffer;
    }
    case LUA_TUSERDATA: {
        // Engine userdata (entities, components) have C __tostring methods.
        size_t length = 0;
        const char* text = luaL_tolstring(L, index, &length);
        std::string result(text, length);
        lua_pop(L, 1);
        return result;
    }
    case LUA_TTABLE: {
        const int count = lua_objlen(L, index);
        return count > 0 ? "table [" + std::to_string(count) + "]" : "table";
    }
    default: return luaL_typename(L, index);
    }
}

} // namespace

ScriptDebugger::ScriptDebugger(LuauHost& host) : host_(host) {
    AETHER_ASSERT(host_.Debugger() == nullptr);
    host_.SetDebugger(this);
    lua_State* L = host_.State();
    lua_callbacks(L)->debugbreak = &ScriptDebugger::OnBreak;
    lua_callbacks(L)->debugstep = &ScriptDebugger::OnStep;
    lua_singlestep(L, 1);
    host_.SetChunkLoadedCallback([this](const std::string& chunk) { ApplyBreakpoints(chunk); });
}

ScriptDebugger::~ScriptDebugger() {
    ClearAllBreakpoints();
    lua_State* L = host_.State();
    lua_callbacks(L)->debugbreak = nullptr;
    lua_callbacks(L)->debugstep = nullptr;
    lua_singlestep(L, 0);
    host_.SetChunkLoadedCallback(nullptr);
    host_.SetDebugger(nullptr);
}

ScriptDebugger* ScriptDebugger::Of(lua_State* L) {
    auto* host = static_cast<LuauHost*>(lua_callbacks(L)->userdata);
    return host != nullptr ? static_cast<ScriptDebugger*>(host->Debugger()) : nullptr;
}

int ScriptDebugger::ApplyBreakpoint(const std::string& chunk, int line, bool enabled) {
    const int ref = host_.ChunkFunction(chunk);
    if (ref == LUA_NOREF) {
        return 0;
    }
    lua_State* L = host_.State();
    lua_getref(L, ref);
    const int actual = lua_breakpoint(L, -1, line, enabled ? 1 : 0);
    lua_pop(L, 1);
    return actual;
}

void ScriptDebugger::ApplyBreakpoints(const std::string& chunk) {
    auto it = breakpoints_.find(chunk);
    if (it == breakpoints_.end()) {
        return;
    }
    for (Breakpoint& bp : it->second) {
        bp.actual = ApplyBreakpoint(chunk, bp.requested, true);
    }
}

int ScriptDebugger::SetBreakpoint(const std::string& chunk, int line) {
    std::vector<Breakpoint>& list = breakpoints_[chunk];
    const int actual = ApplyBreakpoint(chunk, line, true);
    if (actual == -1) {
        if (list.empty()) {
            breakpoints_.erase(chunk);
        }
        return -1;
    }
    for (const Breakpoint& bp : list) {
        if (bp.requested == line || (actual != 0 && bp.actual == actual)) {
            return actual != 0 ? actual : line; // already set
        }
    }
    list.push_back({line, actual});
    return actual != 0 ? actual : line;
}

bool ScriptDebugger::ClearBreakpoint(const std::string& chunk, int line) {
    auto it = breakpoints_.find(chunk);
    if (it == breakpoints_.end()) {
        return false;
    }
    std::vector<Breakpoint>& list = it->second;
    auto bp = std::find_if(list.begin(), list.end(),
                           [&](const Breakpoint& b) { return b.requested == line || b.actual == line; });
    if (bp == list.end()) {
        return false;
    }
    const int actual = bp->actual;
    list.erase(bp);
    // Another breakpoint may have landed on the same line; keep it armed.
    const bool shared = std::any_of(list.begin(), list.end(), [&](const Breakpoint& b) { return b.actual == actual; });
    if (actual > 0 && !shared) {
        ApplyBreakpoint(chunk, actual, false);
    }
    if (list.empty()) {
        breakpoints_.erase(it);
    }
    return true;
}

void ScriptDebugger::ClearBreakpoints(const std::string& chunk) {
    auto it = breakpoints_.find(chunk);
    if (it == breakpoints_.end()) {
        return;
    }
    for (const Breakpoint& bp : it->second) {
        if (bp.actual > 0) {
            ApplyBreakpoint(chunk, bp.actual, false);
        }
    }
    breakpoints_.erase(it);
}

void ScriptDebugger::ClearAllBreakpoints() {
    while (!breakpoints_.empty()) {
        ClearBreakpoints(breakpoints_.begin()->first);
    }
}

bool ScriptDebugger::IsLoaded(const std::string& chunk) const { return host_.ChunkFunction(chunk) != LUA_NOREF; }

std::vector<int> ScriptDebugger::Breakpoints(const std::string& chunk) const {
    std::vector<int> lines;
    if (auto it = breakpoints_.find(chunk); it != breakpoints_.end()) {
        for (const Breakpoint& bp : it->second) {
            lines.push_back(bp.actual > 0 ? bp.actual : bp.requested);
        }
    }
    std::sort(lines.begin(), lines.end());
    lines.erase(std::unique(lines.begin(), lines.end()), lines.end());
    return lines;
}

void ScriptDebugger::RequestPause() { step_ = StepMode::Pause; }

ScriptDebugger::Location ScriptDebugger::Here(lua_State* L) const {
    Location here;
    lua_Debug ar{};
    if (lua_getinfo(L, 0, "lp", &ar) != 0) {
        here.proto = ar.protoid;
        here.line = ar.currentline;
    }
    here.depth = lua_stackdepth(L);
    return here;
}

void ScriptDebugger::OnBreak(lua_State* L, lua_Debug*) {
    ScriptDebugger* self = Of(L);
    if (self == nullptr || self->stopped_) {
        return;
    }
    const Location here = self->Here(L);
    if (self->suppressing_ && here == self->suppress_) {
        return; // another instruction of the line we just stopped on
    }
    self->Stop(L, StopReason::Breakpoint, here);
}

void ScriptDebugger::OnStep(lua_State* L, lua_Debug*) {
    ScriptDebugger* self = Of(L);
    if (self == nullptr || self->stopped_) {
        return;
    }
    if (self->step_ == StepMode::None && !self->suppressing_) {
        return; // the common case while attached: nothing to do
    }
    const Location here = self->Here(L);
    if (self->suppressing_ && !(here == self->suppress_)) {
        self->suppressing_ = false;
    }
    const Location& from = self->step_from_;
    bool stop = false;
    StopReason reason = StopReason::Step;
    switch (self->step_) {
    case StepMode::None: break;
    case StepMode::Pause:
        stop = true;
        reason = StopReason::Pause;
        break;
    case StepMode::Into: stop = !(here == from); break;
    case StepMode::Over:
        stop = here.depth < from.depth || (here.depth == from.depth && !(here == from));
        break;
    case StepMode::Out: stop = here.depth < from.depth; break;
    }
    if (stop) {
        self->Stop(L, reason, here);
    }
}

void ScriptDebugger::Stop(lua_State* L, StopReason reason, const Location& here) {
    ++stops_;
    const DebugStop stop = Describe(L, reason);
    DebugAction action = DebugAction::Continue;
    if (handler_) {
        stopped_ = true;
        action = handler_(stop);
        stopped_ = false;
    }
    suppress_ = here;
    suppressing_ = true;
    step_from_ = here;
    switch (action) {
    case DebugAction::Continue: step_ = StepMode::None; break;
    case DebugAction::StepInto: step_ = StepMode::Into; break;
    case DebugAction::StepOver: step_ = StepMode::Over; break;
    case DebugAction::StepOut: step_ = StepMode::Out; break;
    }
}

DebugStop ScriptDebugger::Describe(lua_State* L, StopReason reason) const {
    DebugStop stop;
    stop.reason = reason;
    const int depth = lua_stackdepth(L);
    for (int level = 0; level < depth; ++level) {
        lua_Debug ar{};
        if (lua_getinfo(L, level, "sln", &ar) == 0) {
            break;
        }
        DebugFrame frame;
        frame.function = ar.name != nullptr ? ar.name : "";
        frame.is_native = ar.what != nullptr && std::string(ar.what) == "C";
        frame.chunk = frame.is_native ? "" : ChunkOf(ar);
        frame.line = frame.is_native ? -1 : ar.currentline;
        if (!frame.is_native) {
            for (int n = 1;; ++n) {
                const char* name = lua_getlocal(L, level, n);
                if (name == nullptr) {
                    break;
                }
                if (name[0] != '(') { // "(for state)" and other temporaries
                    frame.locals.push_back({name, ValueText(L, -1), luaL_typename(L, -1)});
                }
                lua_pop(L, 1);
            }
        }
        stop.frames.push_back(std::move(frame));
    }
    if (!stop.frames.empty()) {
        stop.chunk = stop.frames.front().chunk;
        stop.line = stop.frames.front().line;
    }
    return stop;
}

} // namespace aether::script
