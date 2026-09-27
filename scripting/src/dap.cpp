#include "aether/script/dap.h"

#include <algorithm>
#include <cctype>
#include <filesystem>

namespace aether::script {

using nlohmann::json;

namespace {

constexpr int kThreadId = 1;

std::string LowerAscii(std::string text) {
    for (char& c : text) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return text;
}

const char* ReasonName(StopReason reason) {
    switch (reason) {
    case StopReason::Breakpoint: return "breakpoint";
    case StopReason::Step: return "step";
    case StopReason::Pause: return "pause";
    }
    return "breakpoint";
}

} // namespace

// ---------------------------------------------------------------------------
// Framing
// ---------------------------------------------------------------------------

std::optional<json> DapFraming::Next() {
    for (;;) {
        const usize header_end = buffer_.find("\r\n\r\n");
        if (header_end == std::string::npos) {
            return std::nullopt;
        }
        // Headers are "Name: value" lines; only Content-Length matters.
        long long length = -1;
        usize line_start = 0;
        while (line_start < header_end) {
            usize line_end = buffer_.find("\r\n", line_start);
            if (line_end == std::string::npos || line_end > header_end) {
                line_end = header_end;
            }
            const std::string line = buffer_.substr(line_start, line_end - line_start);
            const usize colon = line.find(':');
            if (colon != std::string::npos && LowerAscii(line.substr(0, colon)) == "content-length") {
                try {
                    length = std::stoll(line.substr(colon + 1));
                } catch (...) {
                    length = -1;
                }
            }
            line_start = line_end + 2;
        }
        if (length < 0) {
            ++malformed_;
            buffer_.clear(); // no way to find the next message's start
            return std::nullopt;
        }
        const usize body_start = header_end + 4;
        if (buffer_.size() - body_start < static_cast<usize>(length)) {
            return std::nullopt; // wait for the rest
        }
        json message = json::parse(buffer_.begin() + static_cast<std::ptrdiff_t>(body_start),
                                   buffer_.begin() + static_cast<std::ptrdiff_t>(body_start + static_cast<usize>(length)),
                                   nullptr, false);
        buffer_.erase(0, body_start + static_cast<usize>(length));
        if (message.is_discarded() || !message.is_object()) {
            ++malformed_;
            continue;
        }
        return message;
    }
}

std::string DapFraming::Frame(const json& message) {
    const std::string body = message.dump();
    return "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
}

// ---------------------------------------------------------------------------
// Session
// ---------------------------------------------------------------------------

DapSession::DapSession(ScriptDebugger& debugger, Send send, Receive receive)
    : debugger_(debugger), send_(std::move(send)), receive_(std::move(receive)) {
    SetSourceMap({});
    debugger_.SetHandler([this](const DebugStop& stop) { return OnStop(stop); });
}

DapSession::~DapSession() { debugger_.SetHandler(nullptr); }

void DapSession::SetSourceMap(SourceMap map) {
    sources_ = std::move(map);
    if (!sources_.to_chunk) {
        sources_.to_chunk = [](const std::string& path) {
            return std::filesystem::path(path).filename().string();
        };
    }
}

void DapSession::Respond(const json& request, bool success, json body, const std::string& message) {
    json response = {{"seq", seq_++},
                     {"type", "response"},
                     {"request_seq", request.value("seq", 0)},
                     {"success", success},
                     {"command", request.value("command", "")},
                     {"body", std::move(body)}};
    if (!message.empty()) {
        response["message"] = message;
    }
    send_(response);
}

void DapSession::Event(const std::string& event, json body) {
    send_({{"seq", seq_++}, {"type", "event"}, {"event", event}, {"body", std::move(body)}});
}

void DapSession::Handle(const json& request) {
    (void)Dispatch(request); // resuming while nothing is stopped is a no-op
}

DebugAction DapSession::OnStop(const DebugStop& stop) {
    if (disconnected_ || !initialized_) {
        return DebugAction::Continue;
    }
    stop_ = &stop;
    Event("stopped", {{"reason", ReasonName(stop.reason)}, {"threadId", kThreadId}, {"allThreadsStopped", true}});
    Resume resume = Resume::None;
    while (resume == Resume::None) {
        std::optional<json> request = receive_ ? receive_() : std::nullopt;
        if (!request) {
            disconnected_ = true; // the client went away: let the game run
            debugger_.ClearAllBreakpoints();
            resume = Resume::Continue;
            break;
        }
        resume = Dispatch(*request);
    }
    stop_ = nullptr;
    switch (resume) {
    case Resume::StepInto: return DebugAction::StepInto;
    case Resume::StepOver: return DebugAction::StepOver;
    case Resume::StepOut: return DebugAction::StepOut;
    default: return DebugAction::Continue;
    }
}

DapSession::Resume DapSession::Dispatch(const json& request) {
    if (request.value("type", "") != "request") {
        return Resume::None; // responses to reverse requests, events: ignored
    }
    const std::string command = request.value("command", "");
    const json args = request.contains("arguments") && request["arguments"].is_object() ? request["arguments"]
                                                                                        : json::object();
    auto need_stop = [&]() {
        if (stop_ == nullptr) {
            Respond(request, false, json::object(), "not stopped");
            return false;
        }
        return true;
    };

    if (command == "initialize") {
        lines_start_at_1_ = args.value("linesStartAt1", true);
        initialized_ = true;
        Respond(request, true,
                {{"supportsConfigurationDoneRequest", true},
                 {"supportsEvaluateForHovers", true},
                 {"supportsTerminateRequest", false},
                 {"supportsStepBack", false}});
        Event("initialized");
    } else if (command == "launch" || command == "attach" || command == "configurationDone") {
        Respond(request, true);
    } else if (command == "setBreakpoints") {
        Respond(request, true, SetBreakpoints(args));
    } else if (command == "threads") {
        Respond(request, true, {{"threads", json::array({{{"id", kThreadId}, {"name", "Scripts"}}})}});
    } else if (command == "stackTrace") {
        if (need_stop()) Respond(request, true, StackTrace(args));
    } else if (command == "scopes") {
        if (need_stop()) Respond(request, true, Scopes(args));
    } else if (command == "variables") {
        if (need_stop()) Respond(request, true, Variables(args));
    } else if (command == "evaluate") {
        if (need_stop()) {
            if (std::optional<json> result = Evaluate(args)) {
                Respond(request, true, *result);
            } else {
                Respond(request, false, json::object(), "only local variables of a stopped frame can be evaluated");
            }
        }
    } else if (command == "continue") {
        Respond(request, true, {{"allThreadsContinued", true}});
        return stop_ != nullptr ? Resume::Continue : Resume::None;
    } else if (command == "next" || command == "stepIn" || command == "stepOut") {
        if (!need_stop()) {
            return Resume::None;
        }
        Respond(request, true);
        return command == "next" ? Resume::StepOver : command == "stepIn" ? Resume::StepInto : Resume::StepOut;
    } else if (command == "pause") {
        if (stop_ == nullptr) {
            debugger_.RequestPause(); // stops at the next line a script runs
        }
        Respond(request, true);
    } else if (command == "disconnect") {
        debugger_.ClearAllBreakpoints();
        disconnected_ = true;
        Respond(request, true);
        return stop_ != nullptr ? Resume::Continue : Resume::None;
    } else {
        Respond(request, false, json::object(), "unsupported request '" + command + "'");
    }
    return Resume::None;
}

json DapSession::SetBreakpoints(const json& args) {
    const json source = args.value("source", json::object());
    const std::string path = source.value("path", source.value("name", ""));
    const std::string chunk = sources_.to_chunk(path);
    chunk_paths_[chunk] = path;

    std::vector<int> lines;
    if (args.contains("breakpoints") && args["breakpoints"].is_array()) {
        for (const json& bp : args["breakpoints"]) {
            lines.push_back(bp.value("line", 0));
        }
    } else if (args.contains("lines") && args["lines"].is_array()) { // the older form
        for (const json& line : args["lines"]) {
            lines.push_back(line.get<int>());
        }
    }

    const int offset = lines_start_at_1_ ? 0 : 1;
    debugger_.ClearBreakpoints(chunk);
    const bool loaded = debugger_.IsLoaded(chunk);
    json result = json::array();
    for (const int line : lines) {
        const int actual = debugger_.SetBreakpoint(chunk, line + offset);
        json bp = {{"verified", loaded && actual > 0}, {"source", SourceOf(chunk)}};
        if (actual > 0) {
            bp["line"] = actual - offset;
        }
        if (!loaded) {
            bp["message"] = "The script isn't loaded yet; the breakpoint applies when it is.";
        } else if (actual <= 0) {
            bp["message"] = "No code at or after this line.";
        }
        result.push_back(std::move(bp));
    }
    return {{"breakpoints", std::move(result)}};
}

json DapSession::SourceOf(const std::string& chunk) const {
    json source = {{"name", chunk}};
    if (auto it = chunk_paths_.find(chunk); it != chunk_paths_.end() && !it->second.empty()) {
        source["path"] = it->second;
    } else if (sources_.to_path) {
        const std::string path = sources_.to_path(chunk);
        if (!path.empty()) {
            source["path"] = path;
        }
    }
    return source;
}

json DapSession::StackTrace(const json& args) const {
    const usize start = static_cast<usize>(std::max(0, args.value("startFrame", 0)));
    const usize levels = static_cast<usize>(std::max(0, args.value("levels", 0))); // 0 = all
    const int offset = lines_start_at_1_ ? 0 : 1;
    json frames = json::array();
    for (usize i = start; i < stop_->frames.size() && (levels == 0 || frames.size() < levels); ++i) {
        const DebugFrame& frame = stop_->frames[i];
        json entry = {{"id", static_cast<int>(i) + 1},
                      {"name", frame.function.empty() ? (frame.is_native ? "[C]" : "main chunk") : frame.function},
                      {"line", frame.is_native ? 0 : frame.line - offset},
                      {"column", 0}};
        if (!frame.is_native) {
            entry["source"] = SourceOf(frame.chunk);
        } else {
            entry["presentationHint"] = "subtle";
        }
        frames.push_back(std::move(entry));
    }
    return {{"stackFrames", std::move(frames)}, {"totalFrames", stop_->frames.size()}};
}

json DapSession::Scopes(const json& args) const {
    const int frame = args.value("frameId", 0);
    json scopes = json::array();
    if (frame >= 1 && static_cast<usize>(frame) <= stop_->frames.size()) {
        scopes.push_back({{"name", "Locals"},
                          {"presentationHint", "locals"},
                          {"variablesReference", frame}, // one scope per frame: same id
                          {"expensive", false}});
    }
    return {{"scopes", std::move(scopes)}};
}

json DapSession::Variables(const json& args) const {
    const int reference = args.value("variablesReference", 0);
    json variables = json::array();
    if (reference >= 1 && static_cast<usize>(reference) <= stop_->frames.size()) {
        for (const DebugVariable& v : stop_->frames[static_cast<usize>(reference) - 1].locals) {
            variables.push_back({{"name", v.name}, {"value", v.value}, {"type", v.type}, {"variablesReference", 0}});
        }
    }
    return {{"variables", std::move(variables)}};
}

std::optional<json> DapSession::Evaluate(const json& args) const {
    const std::string expression = args.value("expression", "");
    const int frame = args.value("frameId", 1);
    if (frame < 1 || static_cast<usize>(frame) > stop_->frames.size()) {
        return std::nullopt;
    }
    const std::vector<DebugVariable>& locals = stop_->frames[static_cast<usize>(frame) - 1].locals;
    // The innermost declaration wins when a name is shadowed (listed last).
    for (auto it = locals.rbegin(); it != locals.rend(); ++it) {
        if (it->name == expression) {
            return json{{"result", it->value}, {"type", it->type}, {"variablesReference", 0}};
        }
    }
    return std::nullopt;
}

} // namespace aether::script
