#include "aether/script/dap.h"
#include "aether/script/luau_host.h"
#include "test_framework.h"

#include <deque>

using namespace aether;
using namespace aether::script;
using nlohmann::json;

namespace {

const char* kCounter = R"(local function add(a, b)
    local sum = a + b
    return sum
end

function Run(n)
    local total = 0
    for i = 1, n do
        total = add(total, i)
    end
    return total
end
)";

LuauHost::Options DebugOptions() {
    LuauHost::Options options;
    options.allow_debug = true;
    return options;
}

// A scripted DAP client: requests queued for while the game is stopped, and
// everything the session sent.
struct Client {
    int seq = 1;
    std::deque<json> queued;
    std::vector<json> sent;
    bool gone = false;

    json Request(const std::string& command, json args = json::object()) {
        return {{"seq", seq++}, {"type", "request"}, {"command", command}, {"arguments", std::move(args)}};
    }
    void Queue(const std::string& command, json args = json::object()) { queued.push_back(Request(command, args)); }

    DapSession::Send Sender() {
        return [this](const json& message) { sent.push_back(message); };
    }
    DapSession::Receive Receiver() {
        return [this]() -> std::optional<json> {
            if (queued.empty()) {
                gone = true;
                return std::nullopt; // as if the client disconnected
            }
            json next = queued.front();
            queued.pop_front();
            return next;
        };
    }

    std::vector<json> Events(const std::string& name) const {
        std::vector<json> out;
        for (const json& m : sent) {
            if (m.value("type", "") == "event" && m.value("event", "") == name) out.push_back(m);
        }
        return out;
    }
    const json* Response(const std::string& command, usize nth = 0) const {
        for (const json& m : sent) {
            if (m.value("type", "") == "response" && m.value("command", "") == command && nth-- == 0) return &m;
        }
        return nullptr;
    }
};

} // namespace

AETHER_TEST(Dap_FramingSplitsJoinsAndSkipsBadMessages) {
    DapFraming framing;
    const std::string a = DapFraming::Frame({{"seq", 1}, {"command", "initialize"}});
    const std::string b = DapFraming::Frame({{"seq", 2}, {"text", "héllo"}});
    AETHER_CHECK(a.rfind("Content-Length: ", 0) == 0);

    // Arrives in pieces, two messages in one read.
    const std::string both = a + b;
    framing.Append(both.substr(0, 10));
    AETHER_CHECK(!framing.Next());
    framing.Append(both.substr(10, a.size() - 12));
    AETHER_CHECK(!framing.Next()); // body incomplete
    framing.Append(both.substr(a.size() - 2));
    std::optional<json> first = framing.Next();
    std::optional<json> second = framing.Next();
    AETHER_CHECK(first && (*first)["seq"] == 1);
    AETHER_CHECK(second && (*second)["text"] == "héllo"); // Content-Length counts bytes
    AETHER_CHECK(!framing.Next());

    // Invalid JSON is skipped; the next message still comes through.
    framing.Append("content-length: 5\r\n\r\n{nope" + DapFraming::Frame({{"seq", 3}}));
    std::optional<json> third = framing.Next();
    AETHER_CHECK(third && (*third)["seq"] == 3 && framing.Malformed() == 1);

    // A header without Content-Length can't be recovered from.
    framing.Append("X-Other: 1\r\n\r\n{}");
    AETHER_CHECK(!framing.Next() && framing.Malformed() == 2);
}

AETHER_TEST(Dap_SessionBreaksInspectsAndSteps) {
    LuauHost host(DebugOptions());
    ScriptDebugger debugger(host);
    Client client;
    DapSession session(debugger, client.Sender(), client.Receiver());

    session.Handle(client.Request("initialize", {{"adapterID", "aether"}, {"linesStartAt1", true}}));
    AETHER_CHECK(session.Initialized());
    const json* init = client.Response("initialize");
    AETHER_CHECK(init && (*init)["success"] == true && (*init)["body"]["supportsConfigurationDoneRequest"] == true);
    AETHER_CHECK(client.Events("initialized").size() == 1);

    // Before the script loads: accepted but not verified yet.
    const std::string path = "C:/Game/Content/Scripts/counter.luau";
    const json source = {{"path", path}};
    session.Handle(client.Request("setBreakpoints", {{"source", source}, {"breakpoints", {{{"line", 3}}}}}));
    const json* early = client.Response("setBreakpoints");
    AETHER_CHECK(early && (*early)["body"]["breakpoints"][0]["verified"] == false);
    session.Handle(client.Request("configurationDone"));

    AETHER_CHECK(host.Run(kCounter, "counter.luau").ok);
    // Re-sent now that it's loaded (as clients do when a source changes):
    // verified, on line 3; a blank line moves; past the end is refused.
    session.Handle(client.Request("setBreakpoints",
                                  {{"source", source}, {"breakpoints", {{{"line", 3}}, {{"line", 500}}}}}));
    const json& bps = (*client.Response("setBreakpoints", 1))["body"]["breakpoints"];
    AETHER_CHECK(bps.size() == 2 && bps[0]["verified"] == true && bps[0]["line"] == 3);
    AETHER_CHECK(bps[0]["source"]["path"] == path);
    AETHER_CHECK(bps[1]["verified"] == false && !bps[1].contains("line"));
    AETHER_CHECK(debugger.Breakpoints("counter.luau") == std::vector<int>{3});

    session.Handle(client.Request("threads"));
    AETHER_CHECK((*client.Response("threads"))["body"]["threads"][0]["name"] == "Scripts");
    session.Handle(client.Request("stackTrace")); // not stopped
    AETHER_CHECK((*client.Response("stackTrace"))["success"] == false);

    // What the client does at the first stop, then at the step's stop.
    client.Queue("stackTrace", {{"threadId", 1}});
    client.Queue("scopes", {{"frameId", 1}});
    client.Queue("variables", {{"variablesReference", 1}});
    client.Queue("evaluate", {{"expression", "sum"}, {"frameId", 1}});
    client.Queue("evaluate", {{"expression", "i"}, {"frameId", 2}});
    client.Queue("evaluate", {{"expression", "sum + 1"}, {"frameId", 1}});
    client.Queue("stepOut", {{"threadId", 1}});
    client.Queue("setBreakpoints", {{"source", source}, {"breakpoints", json::array()}});
    client.Queue("continue", {{"threadId", 1}});

    ScriptResult r = host.Call("Run", {2.0});
    AETHER_CHECK(r.ok && std::get<f64>(r.values[0]) == 3.0);
    AETHER_CHECK(!client.gone && client.queued.empty() && !session.Stopped());

    const std::vector<json> stopped = client.Events("stopped");
    AETHER_CHECK(stopped.size() == 2);
    AETHER_CHECK(stopped[0]["body"]["reason"] == "breakpoint" && stopped[0]["body"]["threadId"] == 1);
    AETHER_CHECK(stopped[1]["body"]["reason"] == "step");

    const json& frames = (*client.Response("stackTrace", 1))["body"]["stackFrames"];
    AETHER_CHECK(frames.size() == 2);
    AETHER_CHECK(frames[0]["name"] == "add" && frames[0]["line"] == 3 && frames[0]["source"]["path"] == path);
    AETHER_CHECK(frames[1]["name"] == "Run" && frames[1]["line"] == 9);
    AETHER_CHECK((*client.Response("scopes"))["body"]["scopes"][0]["variablesReference"] == 1);
    const json& vars = (*client.Response("variables"))["body"]["variables"];
    bool saw_sum = false;
    for (const json& v : vars) {
        if (v["name"] == "sum") saw_sum = v["value"] == "1" && v["type"] == "number";
    }
    AETHER_CHECK(saw_sum);
    AETHER_CHECK((*client.Response("evaluate", 0))["body"]["result"] == "1");
    AETHER_CHECK((*client.Response("evaluate", 1))["body"]["result"] == "1");
    AETHER_CHECK((*client.Response("evaluate", 2))["success"] == false);
    AETHER_CHECK((*client.Response("stepOut"))["success"] == true);
    // Clearing from inside a stop takes effect: the second call of add ran through.
    AETHER_CHECK(debugger.Breakpoints("counter.luau").empty());
    AETHER_CHECK(debugger.StopCount() == 2);

    // Each response answers its request.
    for (const json& m : client.sent) {
        if (m["type"] == "response") AETHER_CHECK(m["request_seq"].get<int>() > 0);
    }
}

AETHER_TEST(Dap_PauseDisconnectAndErrors) {
    LuauHost host(DebugOptions());
    ScriptDebugger debugger(host);
    Client client;
    DapSession session(debugger, client.Sender(), client.Receiver());
    AETHER_CHECK(host.Run(kCounter, "counter.luau").ok);

    // Not initialized yet: stops don't block.
    debugger.SetBreakpoint("counter.luau", 3);
    AETHER_CHECK(host.Call("Run", {1.0}).ok && client.Events("stopped").empty());
    debugger.ClearAllBreakpoints();

    session.Handle(client.Request("initialize"));
    session.Handle(client.Request("frobnicate"));
    const json* unknown = client.Response("frobnicate");
    AETHER_CHECK(unknown && (*unknown)["success"] == false);
    session.Handle(client.Request("next"));
    AETHER_CHECK((*client.Response("next"))["success"] == false);

    // Pause while running stops at the next line a script runs.
    session.Handle(client.Request("pause", {{"threadId", 1}}));
    client.Queue("continue");
    AETHER_CHECK(host.Call("Run", {1.0}).ok);
    AETHER_CHECK(client.Events("stopped").size() == 1 && client.Events("stopped")[0]["body"]["reason"] == "pause");

    // The client vanishing mid-stop lets the game run on, breakpoints gone.
    session.Handle(client.Request("setBreakpoints", {{"source", {{"name", "counter.luau"}}},
                                                     {"breakpoints", {{{"line", 3}}}}}));
    AETHER_CHECK(host.Call("Run", {3.0}).ok);
    AETHER_CHECK(client.gone && session.Disconnected());
    AETHER_CHECK(client.Events("stopped").size() == 2);
    AETHER_CHECK(debugger.Breakpoints("counter.luau").empty());

    // A disconnect request does the same.
    Client second;
    LuauHost host2(DebugOptions());
    ScriptDebugger debugger2(host2);
    DapSession session2(debugger2, second.Sender(), second.Receiver());
    AETHER_CHECK(host2.Run(kCounter, "counter.luau").ok);
    session2.Handle(second.Request("initialize"));
    session2.Handle(second.Request("setBreakpoints", {{"source", {{"name", "counter.luau"}}},
                                                      {"breakpoints", {{{"line", 3}}}}}));
    second.Queue("disconnect");
    AETHER_CHECK(host2.Call("Run", {3.0}).ok);
    AETHER_CHECK(!second.gone && session2.Disconnected() && debugger2.StopCount() == 1);
    AETHER_CHECK((*second.Response("disconnect"))["success"] == true);
}
