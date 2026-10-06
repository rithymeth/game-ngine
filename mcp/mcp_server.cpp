#include "mcp_server.h"

#include <istream>
#include <new>
#include <ostream>

namespace aether::mcp {

namespace {

constexpr const char* kLatestProtocol = "2025-06-18";

constexpr int kParseError = -32700;
constexpr int kInvalidRequest = -32600;
constexpr int kMethodNotFound = -32601;
constexpr int kInvalidParams = -32602;
constexpr int kInternalError = -32603;

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

bool McpServer::AddTool(Tool tool) {
    if (tool.name.empty() || !tool.handler) return false;
    for (Tool& existing : tools_) {
        if (existing.name == tool.name) {
            existing = std::move(tool);
            return true;
        }
    }
    tools_.push_back(std::move(tool));
    return true;
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
    // The schema's "required" arguments must be there, before the handler runs.
    if (tool->input_schema.is_object()) {
        const auto required = tool->input_schema.find("required");
        if (required != tool->input_schema.end() && required->is_array()) {
            for (const Json& key : *required) {
                if (key.is_string() && !args.contains(key.get<std::string>())) {
                    throw RpcError{kInvalidParams, "Missing required argument: " + key.get<std::string>()};
                }
            }
        }
    }
    try {
        Json result = tool->handler(args);
        std::string text = result.is_string() ? result.get<std::string>() : result.dump(2);
        if (text.size() > kMaxResultBytes) {
            return TextResult("The result is too large (" + std::to_string(text.size()) + " bytes, limit " +
                                  std::to_string(kMaxResultBytes) + "); ask for less",
                              true);
        }
        return TextResult(text, false);
    } catch (const ToolError& e) {
        return TextResult(e.what(), true);
    } catch (const Json::exception& e) {
        return TextResult(std::string("Bad arguments: ") + e.what(), true);
    } catch (const std::bad_alloc&) {
        throw;
    } catch (const std::exception& e) {
        return TextResult(std::string("The tool failed: ") + e.what(), true);
    }
}

std::optional<Json> McpServer::HandleMessage(const Json& message) {
    const auto version = message.is_object() ? message.find("jsonrpc") : Json::const_iterator();
    const bool is_v2 = message.is_object() && version != message.end() && version->is_string() && version->get<std::string>() == "2.0";
    if (!is_v2 || !message.contains("method") || !message["method"].is_string()) {
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
    if (!is_notification && !id.is_string() && !id.is_number() && !id.is_null()) {
        return ErrorResponse(nullptr, kInvalidRequest, "\"id\" must be a string, a number or null");
    }
    const Json params = message.contains("params") ? message["params"] : Json::object();

    if (is_notification) {
        return std::nullopt; // notifications/initialized, notifications/cancelled, ...
    }

    try {
        if (method == "initialize") {
            // Echo the client's version if it's one we know; otherwise offer ours.
            std::string requested;
            if (params.is_object()) {
                const auto it = params.find("protocolVersion");
                if (it != params.end() && it->is_string()) requested = it->get<std::string>();
            }
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
            return OkResponse(id, {{"tools", tools}, {"schemaVersion", kToolSchemaVersion}});
        }
        if (method == "tools/call") {
            return OkResponse(id, HandleToolCall(params));
        }
    } catch (const RpcError& e) {
        return ErrorResponse(id, e.code, e.message);
    } catch (const Json::exception& e) {
        return ErrorResponse(id, kInvalidParams, std::string("Bad request: ") + e.what());
    } catch (const std::bad_alloc&) {
        throw;
    } catch (const std::exception& e) {
        return ErrorResponse(id, kInternalError, std::string("Internal error: ") + e.what());
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
    bool too_long = false;
    char c;
    const auto finish = [&] {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (too_long) {
            out << ErrorResponse(nullptr, kInvalidRequest, "The message is larger than " + std::to_string(kMaxMessageBytes) + " bytes").dump() << '\n' << std::flush;
        } else if (line.find_first_not_of(" \t") != std::string::npos) {
            if (std::optional<Json> response = HandleText(line)) {
                out << response->dump() << '\n' << std::flush;
            }
        }
        line.clear();
        too_long = false;
    };
    // A line is read a character at a time so a huge one is skipped, not held in memory.
    while (in.get(c)) {
        if (c == '\n') {
            finish();
        } else if (!too_long) {
            if (line.size() >= kMaxMessageBytes) {
                too_long = true;
                line.clear();
            } else {
                line.push_back(c);
            }
        }
    }
    if (too_long || !line.empty()) finish(); // the last line, with no newline
}

} // namespace aether::mcp
