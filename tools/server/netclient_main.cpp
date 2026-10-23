// aether_netclient: a headless test client for aether_server (M8.9). Joins
// over UDP with a name, password and build id, plays for --duration seconds
// receiving replicated state, Pings its pawn by server RPC and waits for the
// replicated acknowledgement, then prints one JSON line:
//   AETHER_NETCLIENT {"joined":true,"entities_seen":..,"rpc_ack":true,"bytes_received":..,...}
// Exit codes: 0 passed, 1 failed (not joined, no state, no RPC ack), 2 bad arguments.

#include "aether/server/dedicated_server.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

using namespace aether;
using namespace aether::server;

namespace {
void Usage() {
    std::fprintf(stderr,
                 "usage: aether_netclient [--server <ip:port>] [--name <text>] [--password <text>] [--build-id <n>]\n"
                 "                        [--duration <seconds>] [--join-timeout <seconds>] [--no-rpc]\n"
                 "AETHER_SERVER_PASSWORD is used when --password isn't given.\n");
}
} // namespace

int main(int argc, char** argv) {
    NetClientConfig config;
    if (const char* pw = std::getenv("AETHER_SERVER_PASSWORD")) config.password = pw;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const bool has = i + 1 < argc;
        auto number = [&](f64& out) {
            char* end = nullptr;
            out = std::strtod(argv[++i], &end);
            return *end == '\0' && out >= 0;
        };
        f64 v = 0;
        if (a == "--server" && has) {
            const auto addr = net::Address::Parse(argv[++i]);
            if (!addr) return std::fprintf(stderr, "aether_netclient: --server must be a.b.c.d:port\n"), 2;
            config.server = *addr;
        } else if (a == "--name" && has) {
            config.name = argv[++i];
        } else if (a == "--password" && has) {
            config.password = argv[++i];
        } else if (a == "--build-id" && has) {
            if (!number(v) || v > 4294967295.0) return Usage(), 2;
            config.build_id = static_cast<u32>(v);
        } else if (a == "--duration" && has) {
            if (!number(v)) return Usage(), 2;
            config.duration = v;
        } else if (a == "--join-timeout" && has) {
            if (!number(v)) return Usage(), 2;
            config.join_timeout = v;
        } else if (a == "--no-rpc") {
            config.send_rpc = false;
        } else {
            Usage();
            return 2;
        }
    }
    using Clock = std::chrono::steady_clock;
    const auto start = Clock::now();
    const auto now = [&] { return std::chrono::duration<f64>(Clock::now() - start).count(); };
    NetClient client;
    std::string error;
    if (!client.Start(config, now(), &error)) {
        std::fprintf(stderr, "aether_netclient: %s\n", error.c_str());
        return 1;
    }
    while (!client.Done()) {
        client.Tick(now());
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    const NetClientResult& r = client.Result();
    std::printf("AETHER_NETCLIENT %s\n", r.ToJson().c_str());
    return r.Passed(config.send_rpc) ? 0 : 1;
}
