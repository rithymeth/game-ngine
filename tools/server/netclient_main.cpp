// aether_netclient: a headless test client for aether_server (M8.9). Joins
// over UDP with a name, password and build id, plays for --duration seconds
// receiving replicated state, Pings its pawn by server RPC and waits for the
// replicated acknowledgement, then prints one JSON line:
//   AETHER_NETCLIENT {"joined":true,"entities_seen":..,"rpc_ack":true,"bytes_received":..,...}
// --discover lists the servers answering LAN discovery instead (one JSON line,
//   AETHER_DISCOVER {"count":..,"servers":[{"address":..,"name":..,"players":..,...}]}),
// and with --join-first joins the first compatible one found.
// Exit codes: 0 passed, 1 failed (not joined, no state, no RPC ack), 2 bad arguments.

#include "aether/server/dedicated_server.h"

#include <algorithm>
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
                 "usage: aether_netclient [--server <ip:port>] [--name <text>] [--password <text>] [--token <join token>]\n"
                 "                        [--build-id <n>] [--duration <seconds>] [--join-timeout <seconds>] [--no-rpc]\n"
                 "       aether_netclient --discover [--timeout <seconds>] [--lan-port <n>] [--query <ip:port>]... [--build-id <n>]\n"
                 "                        [--join-first [join options]]\n"
                 "AETHER_SERVER_PASSWORD / AETHER_JOIN_TOKEN are used when --password / --token aren't given.\n");
}
} // namespace

int main(int argc, char** argv) {
    NetClientConfig config;
    DiscoverConfig discover;
    bool discovering = false, join_first = false, server_given = false;
    if (const char* pw = std::getenv("AETHER_SERVER_PASSWORD")) config.password = pw;
    if (const char* tk = std::getenv("AETHER_JOIN_TOKEN")) config.token = tk;
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
            server_given = true;
        } else if (a == "--discover") {
            discovering = true;
        } else if (a == "--join-first") {
            join_first = true;
        } else if (a == "--timeout" && has) {
            if (!number(v) || v <= 0) return Usage(), 2;
            discover.timeout = v;
        } else if (a == "--lan-port" && has) {
            if (!number(v) || v > 65535) return Usage(), 2;
            discover.lan_port = static_cast<u16>(v);
        } else if (a == "--query" && has) {
            const auto addr = net::Address::Parse(argv[++i]);
            if (!addr) return std::fprintf(stderr, "aether_netclient: --query must be a.b.c.d:port\n"), 2;
            discover.also_query.push_back(*addr);
        } else if (a == "--name" && has) {
            config.name = argv[++i];
        } else if (a == "--password" && has) {
            config.password = argv[++i];
        } else if (a == "--token" && has) {
            config.token = argv[++i];
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
    if (join_first && (!discovering || server_given)) return std::fprintf(stderr, "aether_netclient: --join-first needs --discover and no --server\n"), 2;
    if (discovering) {
        discover.build_id = config.build_id;
        std::string discover_error;
        const std::vector<net::LanSession> found = DiscoverServers(discover, &discover_error);
        if (!discover_error.empty()) return std::fprintf(stderr, "aether_netclient: %s\n", discover_error.c_str()), 1;
        std::printf("AETHER_DISCOVER %s\n", DiscoveryJson(found).c_str());
        std::fflush(stdout);
        if (!join_first) return found.empty() ? 1 : 0;
        const auto it = std::find_if(found.begin(), found.end(), [](const net::LanSession& s) { return s.compatible; });
        if (it == found.end()) return std::fprintf(stderr, "aether_netclient: no compatible server found\n"), 1;
        config.server = it->address;
        std::fprintf(stderr, "aether_netclient: joining '%s' at %s\n", it->info.name.c_str(), it->address.ToString().c_str());
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
