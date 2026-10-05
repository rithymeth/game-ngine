#include "mcp_server.h"

#include <istream>
#include <ostream>

namespace aether::mcp {

namespace {

constexpr const char* kLatestProtocol = "2025-06-18";

constexpr int kParseError = -32700;
constexpr int kInvalidRequest = -32600;
constexpr int kMethodNotFound = -32601;
constexpr int kInvalidParams = -32602;

Json ErrorResponse(const Json& id, int code, const std::string& message) {
    return {{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", message}}}};
}

Json OkResponse(const Json& id, Json result) {
    return {{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}};
}

// A protocol-level failure inside a handler (as opposed to a ToolError).
struct RpcError {
    int code;
    std::string message;
};

Json TextResult(const std::string& text, bool is_error) {
    return {{"content", Json::array({{{"type", "text"}, {"text", text}}})}, {"isError", is_error}};
}

} // namespace

McpServer::McpServer(std::string name, std::string version) : name_(std::move(name)), version_(std::move(version)) {}

void McpServer::AddTool(Tool tool) {
    for (Tool& existing : tools_) {
        if (existing.name == tool.name) {
            existing = std::move(tool);
            return;
        }
    }
    tools_.push_back(std::move(tool));
}

Json McpServer::HandleToolCall(const Json& params) {
    if (!params.is_object() || !params.contains("name") || !params["name"].is_string()) {
        throw RpcError{kInvalidParams, "tools/call needs a string \"name\""};
    }
    const std::string name = params["name"];
    const Tool* tool = nullptr;
    for (const Tool& t : tools_) {
        if (t.name == name) {
            tool = &t;
        }
    }
    if (tool == nullptr) {
        throw RpcError{kInvalidParams, "Unknown tool: " + name};
    }
    Json args = params.value("arguments", Json::object());
    if (args.is_null()) {
        args = Json::object();
    }
    if (!args.is_object()) {
        throw RpcError{kInvalidParams, "\"arguments\" must be an object"};
    }
    try {
        Json result = tool->handler(args);
        return TextResult(result.is_string() ? result.get<std::string>() : result.dump(2), false);
    } catch (const ToolError& e) {
        return TextResult(e.what(), true);
    } catch (const Json::exception& e) {
        return TextResult(std::string("Bad arguments: ") + e.what(), true);
    }
}

std::optional<Json> McpServer::HandleMessage(const Json& message) {
    if (!message.is_object() || message.value("jsonrpc", "") != "2.0" || !message.contains("method") ||
        !message["method"].is_string()) {
        // Responses from the client and malformed messages alike: only the
        // latter get an error, and only if they carry an id to answer.
        if (message.is_object() && (message.contains("result") || message.contains("error"))) {
            return std::nullopt;
        }
        return ErrorResponse(message.is_object() && message.contains("id") ? message["id"] : Json(nullptr),
                             kInvalidRequest, "Not a JSON-RPC 2.0 request");
    }

    const std::string method = message["method"];
    const bool is_notification = !message.contains("id");
    const Json id = is_notification ? Json(nullptr) : message["id"];
    const Json params = message.value("params", Json::object());

    if (is_notification) {
        return std::nullopt; // notifications/initialized, notifications/cancelled, ...
    }

    try {
        if (method == "initialize") {
            // Echo the client's version if it's one we know; otherwise offer ours.
            std::string requested = params.is_object() ? params.value("protocolVersion", "") : "";
            static const char* kSupported[] = {"2025-06-18", "2025-03-26", "2024-11-05"};
            std::string version = kLatestProtocol;
            for (const char* s : kSupported) {
                if (requested == s) {
                    version = requested;
                }
            }
            return OkResponse(id, {{"protocolVersion", version},
                                   {"capabilities", {{"tools", {{"listChanged", false}}}}},
                                   {"serverInfo", {{"name", name_}, {"version", version_}}}});
        }
        if (method == "ping") {
            return OkResponse(id, Json::object());
        }
        if (method == "tools/list") {
            Json tools = Json::array();
            for (const Tool& t : tools_) {
                tools.push_back({{"name", t.name}, {"description", t.description}, {"inputSchema", t.input_schema}});
            }
            return OkResponse(id, {{"tools", tools}});
        }
        if (method == "tools/call") {
            return OkResponse(id, HandleToolCall(params));
        }
    } catch (const RpcError& e) {
        return ErrorResponse(id, e.code, e.message);
    }
    return ErrorResponse(id, kMethodNotFound, "Method not found: " + method);
}

std::optional<Json> McpServer::HandleText(const std::string& line) {
    Json message = Json::parse(line, nullptr, /*allow_exceptions=*/false);
    if (message.is_discarded()) {
        return ErrorResponse(nullptr, kParseError, "Parse error");
    }
    return HandleMessage(message);
}

void McpServer::RunStdio(std::istream& in, std::ostream& out) {
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.find_first_not_of(" \t") == std::string::npos) {
            continue;
        }
        if (std::optional<Json> response = HandleText(line)) {
            out << response->dump() << '\n' << std::flush;
        }
    }
}

} // namespace aether::mcp
