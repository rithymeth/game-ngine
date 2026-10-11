#include "mcp_server.h"

#include <cstdint>
#include <iomanip>
#include <istream>
#include <random>
#include <sstream>
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


// FNV-1a 64: a stable fingerprint of the arguments for the audit log (not a security hash).
std::string ArgsHash(const std::string& text) {
    std::uint64_t hash = 1469598103934665603ull;
    for (unsigned char c : text) {
        hash ^= c;
        hash *= 1099511628211ull;
    }
    std::ostringstream out;
    out << std::hex << std::setw(16) << std::setfill('0') << hash;
    return out.str();
}

std::string NewConfirmationToken() {
    static std::mt19937_64 rng{std::random_device{}()};
    std::ostringstream out;
    out << std::hex << std::setw(16) << std::setfill('0') << rng();
    return out.str();
}

// A protocol-level failure inside a handler (as opposed to a ToolError).
struct RpcError {
    int code;
    std::string message;
};

Json TextResult(const std::string& text, bool is_error) {
    return {{"content", Json::array({{{"type", "text"}, {"text", text}}})}, {"isError", is_error}};
}

bool MatchesSchemaType(const Json& value, const Json& type) {
    if (!type.is_string()) return true;
    const std::string name = type.get<std::string>();
    if (name == "object") return value.is_object();
    if (name == "array") return value.is_array();
    if (name == "string") return value.is_string();
    if (name == "boolean") return value.is_boolean();
    if (name == "integer") return value.is_number_integer();
    if (name == "number") return value.is_number();
    if (name == "null") return value.is_null();
    return true; // Unknown schema extensions are left to the handler.
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

std::chrono::steady_clock::time_point McpServer::Now() const {
    return clock_ ? clock_() : std::chrono::steady_clock::now();
}

void McpServer::Audit(const std::string& tool, const std::string& args_key, const char* outcome) const {
    if (!audit_) return;
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    audit_({{"time_ms", ms}, {"tool", tool}, {"args_hash", ArgsHash(args_key)}, {"outcome", outcome}});
}

bool McpServer::MarkDestructive(const std::string& name) {
    for (Tool& tool : tools_) {
        if (tool.name == name) {
            tool.destructive = true;
            return true;
        }
    }
    return false;
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
    // A confirmation token is not an argument of the tool; take it out before anything else reads the arguments.
    std::string confirm_token;
    if (const auto it = args.find("confirm_token"); it != args.end()) {
        if (it->is_string()) confirm_token = it->get<std::string>();
        args.erase(it);
    }
    const std::string args_key = args.dump();
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
        const auto properties = tool->input_schema.find("properties");
        if (properties != tool->input_schema.end() && properties->is_object()) {
            for (auto it = properties->begin(); it != properties->end(); ++it) {
                if (!args.contains(it.key()) || !it.value().is_object()) continue;
                const auto type = it.value().find("type");
                if (type != it.value().end() && !MatchesSchemaType(args[it.key()], *type)) {
                    const std::string expected = type->is_string() ? type->get<std::string>() : "the declared type";
                    throw RpcError{kInvalidParams, "Argument '" + it.key() + "' must be " + expected};
                }
            }
        }
    }
    if (policy_) {
        const Permission permission = policy_->For(tool->name, tool->destructive);
        if (permission == Permission::Deny) {
            Audit(name, args_key, "denied");
            return TextResult("The tool '" + name + "' is denied by this project's agent permissions", true);
        }
        if (permission == Permission::Confirm) {
            const auto now = Now();
            for (auto it = pending_.begin(); it != pending_.end();) {
                it = it->second.expires <= now ? pending_.erase(it) : std::next(it);
            }
            const auto found = confirm_token.empty() ? pending_.end() : pending_.find(confirm_token);
            if (!confirm_token.empty() &&
                (found == pending_.end() || found->second.tool != name || found->second.args_key != args_key)) {
                Audit(name, args_key, "confirmation_invalid");
                return TextResult("The confirmation token is not valid for this call (it may have expired, been used already, or the arguments changed). "
                                  "Call '" + name + "' without a token to get a new one",
                                  true);
            }
            if (confirm_token.empty()) {
                if (pending_.size() >= kMaxPendingConfirmations) {
                    Audit(name, args_key, "confirmation_invalid");
                    return TextResult("Too many confirmations are waiting; answer or wait for them to expire", true);
                }
                const std::string token = NewConfirmationToken();
                pending_[token] = {name, args_key, now + kConfirmationLifetime};
                Audit(name, args_key, "confirmation_required");
                const Json answer = {{"status", "confirmation_required"},
                                     {"tool", name},
                                     {"token", token},
                                     {"expires_in_seconds", static_cast<int>(kConfirmationLifetime.count())},
                                     {"message", "This tool needs the person's approval. After they approve, call '" + name +
                                                     "' again with the same arguments plus \"confirm_token\": \"" + token + "\"."}};
                return TextResult(answer.dump(2), false);
            }
            pending_.erase(found); // single use
        }
    }
    try {
        Json result = tool->handler(args);
        std::string text = result.is_string() ? result.get<std::string>() : result.dump(2);
        if (text.size() > kMaxResultBytes) {
            Audit(name, args_key, "error");
            return TextResult("The result is too large (" + std::to_string(text.size()) + " bytes, limit " +
                                  std::to_string(kMaxResultBytes) + "); ask for less",
                              true);
        }
        Audit(name, args_key, "ok");
        return TextResult(text, false);
    } catch (const ToolError& e) {
        Audit(name, args_key, "error");
        return TextResult(e.what(), true);
    } catch (const Json::exception& e) {
        Audit(name, args_key, "error");
        return TextResult(std::string("Bad arguments: ") + e.what(), true);
    } catch (const std::bad_alloc&) {
        throw;
    } catch (const std::exception& e) {
        Audit(name, args_key, "error");
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
