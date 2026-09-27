#pragma once

#include "aether/script/debugger.h"

#include <nlohmann/json.hpp>

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace aether::script {

// The Debug Adapter Protocol (Phase 11 step 6, docs/design/PHASE_SPECS.md
// §11.5): lets VS Code and other DAP clients debug the game's scripts
// through the ScriptDebugger. Transport-agnostic: the editor feeds it bytes
// from a socket (or stdio) and sends what it writes.

// DAP's wire format: "Content-Length: N\r\n\r\n" then N bytes of JSON.
class DapFraming {
public:
    // Adds received bytes; complete messages become available from Next.
    void Append(std::string_view bytes) { buffer_.append(bytes); }
    // The next complete message, if any. A message that isn't valid JSON is
    // skipped (and counted); a bad header drops the buffer.
    std::optional<nlohmann::json> Next();
    usize Malformed() const { return malformed_; }

    static std::string Frame(const nlohmann::json& message);

private:
    std::string buffer_;
    usize malformed_ = 0;
};

// One client's debugging session.
//
// - Requests handled: initialize, launch, attach, setBreakpoints,
//   configurationDone, threads, stackTrace, scopes, variables, evaluate (a
//   local's name, in a stopped frame), continue, next, stepIn, stepOut,
//   pause, disconnect. Others get an error response.
// - Scripts are one DAP thread ("Scripts", id 1).
// - Sources map to chunk names through a SourceMap (default: the file
//   name, "C:/Game/Content/Scripts/Mover.luau" -> "Mover.luau").
// - While a script is stopped the session blocks inside the debugger's
//   handler, reading requests with `receive` until one resumes it. When the
//   client disconnects (receive returns nullopt) execution continues.
class DapSession {
public:
    using Send = std::function<void(const nlohmann::json& message)>;
    // Blocks until the next request arrives; nullopt when the client is gone.
    using Receive = std::function<std::optional<nlohmann::json>()>;

    struct SourceMap {
        std::function<std::string(const std::string& path)> to_chunk;
        std::function<std::string(const std::string& chunk)> to_path; // for stack frames
    };

    DapSession(ScriptDebugger& debugger, Send send, Receive receive);
    ~DapSession();
    DapSession(const DapSession&) = delete;
    DapSession& operator=(const DapSession&) = delete;

    void SetSourceMap(SourceMap map);

    // Handles one request while scripts run (the editor calls this for each
    // message it receives between frames).
    void Handle(const nlohmann::json& request);

    bool Initialized() const { return initialized_; }
    bool Disconnected() const { return disconnected_; }
    bool Stopped() const { return stop_ != nullptr; }

private:
    enum class Resume { None, Continue, StepInto, StepOver, StepOut };

    DebugAction OnStop(const DebugStop& stop);
    Resume Dispatch(const nlohmann::json& request);
    void Respond(const nlohmann::json& request, bool success, nlohmann::json body = nlohmann::json::object(),
                 const std::string& message = "");
    void Event(const std::string& event, nlohmann::json body = nlohmann::json::object());

    nlohmann::json SetBreakpoints(const nlohmann::json& args);
    nlohmann::json StackTrace(const nlohmann::json& args) const;
    nlohmann::json Scopes(const nlohmann::json& args) const;
    nlohmann::json Variables(const nlohmann::json& args) const;
    std::optional<nlohmann::json> Evaluate(const nlohmann::json& args) const;
    nlohmann::json SourceOf(const std::string& chunk) const;

    ScriptDebugger& debugger_;
    Send send_;
    Receive receive_;
    SourceMap sources_;
    int seq_ = 1;
    bool initialized_ = false;
    bool disconnected_ = false;
    bool lines_start_at_1_ = true;
    const DebugStop* stop_ = nullptr; // while stopped
    std::map<std::string, std::string> chunk_paths_; // chunk -> the path the client used
};

} // namespace aether::script
