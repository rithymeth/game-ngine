#pragma once

// A small Model Context Protocol (MCP) server core: JSON-RPC 2.0 over
// newline-delimited messages (the MCP "stdio" transport), exposing "tools".
//
// Transport-independent: HandleMessage() turns one request into one response,
// so it is unit-tested without any pipes; RunStdio() is the loop around it.
// Implements the lifecycle (initialize / notifications/initialized / ping)
// and tools/list + tools/call, which is all an editor-control server needs.
//
// stdout belongs to the protocol: nothing else may print there while
// RunStdio is running (the engine logger writes to stdout below Error level,
// so the server executable raises its minimum level).

#include "permissions.h"

#include <nlohmann/json.hpp>
#include <chrono>

#include <cstddef>
#include <functional>
#include <iosfwd>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace aether::mcp {

using Json = nlohmann::json;

// The shape of the tool listing (a tool's name, description and input schema). It is reported in the tools/list
// result as "schemaVersion", so a client can tell if it changes. Adding a field doesn't change it.
inline constexpr int kToolSchemaVersion = 1;
// Limits: a request line longer than this is refused (and skipped), and so is a tool result larger than this.
inline constexpr std::size_t kMaxMessageBytes = 4 * 1024 * 1024;
inline constexpr std::size_t kMaxResultBytes = 8 * 1024 * 1024;

// Thrown by a tool handler to report a failure to the model (an MCP tool
// result with isError = true) rather than a protocol error.
class ToolError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct Tool {
    std::string name;
    std::string description;
    Json input_schema = {{"type", "object"}, {"properties", Json::object()}};
    // Returns the result, sent back as the tool's text content (JSON text).
    std::function<Json(const Json& arguments)> handler;
    // Loses work that undo cannot bring back (deleting, overwriting a file, discarding a scene). The built-in
    // permission rule asks for confirmation before running one.
    bool destructive = false;
};

class McpServer;

// The tools, registered outside the editor group, whose work undo cannot bring back (deleting a project asset, a save slot, writing a prefab over
// its source). The built-in permission rule asks before these run. The editor group flags its own.
inline constexpr const char* kStandardDestructiveTools[] = {"asset_delete", "save_delete", "prefab_apply"};

class McpServer {
public:
    McpServer(std::string name, std::string version);

    // Adds a tool, replacing one of the same name. A tool with no name or no handler is refused (false).
    bool AddTool(Tool tool);
    const std::vector<Tool>& Tools() const { return tools_; }
    // Flags an existing tool as destructive (Tool::destructive). False if there is no such tool.
    bool MarkDestructive(const std::string& name);
    // Flags kStandardDestructiveTools. Returns the names that no registered tool has (a tool renamed or left unregistered would otherwise lose its
    // protection without a sound); empty when all were found.
    std::vector<std::string> MarkStandardDestructiveTools();

    // Handles one JSON-RPC message. Returns the response, or nullopt for a
    // notification (which gets none).
    std::optional<Json> HandleMessage(const Json& message);

    // Same, from text: a parse failure becomes a JSON-RPC -32700 error.
    std::optional<Json> HandleText(const std::string& line);

    // Reads one message per line from `in` and writes one response per line
    // to `out` until `in` ends. Blank lines are ignored.
    void RunStdio(std::istream& in, std::ostream& out);

    // What an agent may do (permissions.h). Without a policy every tool runs, as before; with one, a tool the
    // policy answers Confirm for runs only when the call is repeated with the "confirm_token" it was given.
    void SetPolicy(PermissionPolicy policy) { policy_ = std::move(policy); }
    bool HasPolicy() const { return policy_.has_value(); }

    // One record per tool call: {"time_ms", "tool", "args_hash", "outcome"} with outcome ok, error, denied,
    // confirmation_required or confirmation_invalid. Arguments are hashed, never logged, so a record carries no
    // content. The sink is called on the server's thread.
    using AuditSink = std::function<void(const Json& record)>;
    void SetAuditSink(AuditSink sink) { audit_ = std::move(sink); }

    // For tests: the clock that confirmation tokens expire against.
    using Clock = std::function<std::chrono::steady_clock::time_point()>;
    void SetClock(Clock clock) { clock_ = std::move(clock); }
    // How long a confirmation token stays valid, and how many may be outstanding.
    static constexpr std::chrono::seconds kConfirmationLifetime{300};
    static constexpr std::size_t kMaxPendingConfirmations = 32;

private:
    Json HandleToolCall(const Json& params);

    std::string name_;
    std::string version_;
    std::vector<Tool> tools_;
    std::optional<PermissionPolicy> policy_;
    AuditSink audit_;
    Clock clock_;
    struct PendingConfirmation {
        std::string tool;
        std::string args_key;
        std::chrono::steady_clock::time_point expires;
    };
    std::map<std::string, PendingConfirmation> pending_;
    void Audit(const std::string& tool, const std::string& args_key, const char* outcome) const;
    std::chrono::steady_clock::time_point Now() const;
};

} // namespace aether::mcp
