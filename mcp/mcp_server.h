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

#include <nlohmann/json.hpp>

#include <functional>
#include <iosfwd>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace aether::mcp {

using Json = nlohmann::json;

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
};

class McpServer {
public:
    McpServer(std::string name, std::string version);

    void AddTool(Tool tool);
    const std::vector<Tool>& Tools() const { return tools_; }

    // Handles one JSON-RPC message. Returns the response, or nullopt for a
    // notification (which gets none).
    std::optional<Json> HandleMessage(const Json& message);

    // Same, from text: a parse failure becomes a JSON-RPC -32700 error.
    std::optional<Json> HandleText(const std::string& line);

    // Reads one message per line from `in` and writes one response per line
    // to `out` until `in` ends. Blank lines are ignored.
    void RunStdio(std::istream& in, std::ostream& out);

private:
    Json HandleToolCall(const Json& params);

    std::string name_;
    std::string version_;
    std::vector<Tool> tools_;
};

} // namespace aether::mcp
