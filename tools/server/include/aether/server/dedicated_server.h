#pragma once

// M8.9: the dedicated server - a headless SessionHost + ReplicationServer +
// RpcRouter over a real UDP socket, ticking a World at a fixed rate - and the
// headless test client that joins it. The aether_server and aether_netclient
// executables are thin wrappers; tests drive both in one process.

#include "aether/ecs/world.h"
#include "aether/net/host.h"
#include "aether/net/replication.h"
#include "aether/net/rpc.h"
#include "aether/net/session.h"
#include "aether/net/socket.h"

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace aether::server {

// Every key is optional; see docs/manual/15-dedicated-server.md.
struct ServerConfig {
    std::string name = "Aether dedicated server";
    u16 port = 7777;                 // UDP; 0 picks a free port (tests)
    u32 max_players = 8;
    std::string password;            // empty: none. AETHER_SERVER_PASSWORD overrides the file.
    u32 build_id = 1;                // clients must send the same build to join
    std::string token_secret;        // join-token issuer secret; empty: no tokens. AETHER_SERVER_TOKEN_SECRET overrides.
    bool require_token = true;       // with a token_secret: joins without a valid token are refused
    std::string scene;               // empty: the package's startup scene
    std::vector<std::string> paks;   // empty: Paks/ beside the executable
    f64 tick_rate = 30.0;            // fixed simulation + network ticks per second
    std::string log_file;            // empty: stdout/stderr only
    std::string log_level = "info";  // trace, info, warn, error
    bool replicate_scene = true;     // scene entities with a Transform get a NetIdentity ("scene")
    f64 max_runtime = 0.0;           // seconds; 0: until signalled
    bool quit_when_empty = false;    // exit once every player that joined has left
    f64 status_interval = 10.0;      // seconds between status log lines; 0: never
    bool lan_advertise = true;       // answer LAN discovery (aether_netclient --discover) on lan_port
    u16 lan_port = net::kDefaultDiscoveryPort; // 0: any free port (tests)
    std::string profile;             // a network profile capture (JSON lines) is written here; empty: none
    f64 profile_interval = 1.0;      // seconds between capture records
};

// Parses a server.json body over `config` (unknown keys and wrong types are errors).
bool ParseServerConfigJson(const std::string& text, ServerConfig& config, std::string* error);
// Applies command-line arguments (argv[1..]) over `config`. A --config <file>
// is loaded first wherever it appears, so the command line always wins.
// `help` is set for -h/--help. Errors name the bad argument.
bool ParseServerArgs(const std::vector<std::string>& args, ServerConfig& config, std::string* error, bool* help = nullptr);
const char* ServerUsage();

// The pawn each joined player gets: an RPC target clients can poke (Ping),
// whose replicated counter is the server's acknowledgement.
struct ServerPlayer {
    i32 pings = 0;
    i32 last_value = 0;
    void Ping(i32 value) { ++pings, last_value = value; }
};
constexpr const char* kPlayerArchetype = "aether.player";
constexpr const char* kSceneArchetype = "scene";
// Registers ServerPlayer's component id (call before creating worlds that replicate).
void RegisterServerComponents();

struct ServerStats {
    u32 players = 0;
    u32 peak_players = 0;
    u64 joins = 0;
    u64 leaves = 0;
    u64 refused = 0;
    u64 pings = 0;
    u64 ticks = 0;
    u64 bytes_sent = 0;
    u64 bytes_received = 0;
    usize replicated_entities = 0;
    net::HostSecurityStats security; // drops: rate limited, bad cookies, replays, bad MACs
    u32 lan_answered = 0, lan_rate_limited = 0, lan_oversized = 0; // LAN discovery queries
};

class DedicatedServer {
public:
    DedicatedServer() = default;
    ~DedicatedServer();
    DedicatedServer(const DedicatedServer&) = delete;
    DedicatedServer& operator=(const DedicatedServer&) = delete;

    // Binds the UDP port and starts the session over `world` (which must
    // outlive the server and not be replaced while it runs).
    bool Start(const ServerConfig& config, World& world, std::string* error);
    void Stop();
    bool Running() const { return host_ != nullptr; }
    u16 Port() const { return socket_.LocalAddress().port; }

    // One network tick at time `now` (seconds, monotonic).
    void Tick(f64 now);
    // quit_when_empty: true once someone joined and now nobody is left.
    bool EmptyAfterPlay() const { return stats_.joins > 0 && stats_.players == 0; }
    ServerStats Stats() const;
    // The LAN discovery port answered on (config lan_advertise); 0 when not advertising (off, or the
    // port was taken by another server on this host - a warning, not an error).
    u16 LanPort() const { return beacon_ ? lan_socket_.LocalAddress().port : 0; }
    const net::SessionInfo* Advert() const { return beacon_ ? &beacon_->info : nullptr; }

    // Network profile capture (--profile): one JSON object per call - per peer RTT, loss, bytes and
    // packets each way, bytes per channel; snapshots and their bytes since the last record; security
    // drop counters; LAN discovery counters. ProfileSummaryJson() aggregates every record so far.
    std::string ProfileRecord(f64 now);
    std::string ProfileSummaryJson() const;

private:
    void OnSessionEvent(const net::SessionEvent& event);
    struct ProfileTotals {
        u64 records = 0, snapshots = 0, snapshot_bytes = 0, bytes_sent = 0, bytes_received = 0;
        u64 peer_samples = 0;
        f64 rtt_sum = 0, rtt_max = 0, loss_max = 0, max_snapshot_avg = 0;
        u32 peak_peers = 0;
    };
    net::UdpSocket lan_socket_;
    std::unique_ptr<net::LanBeacon> beacon_;
    ProfileTotals profile_;
    u64 profile_snapshot_bytes_ = 0;
    u32 profile_snapshots_ = 0;
    std::map<net::PeerId, std::pair<u64, u64>> profile_peer_bytes_; // last seen sent/received
    f64 profile_last_ = 0;
    ServerConfig config_;
    World* world_ = nullptr;
    net::UdpSocket socket_;
    std::unique_ptr<net::NetHost> host_;
    std::unique_ptr<net::SessionHost> session_;
    std::unique_ptr<net::ReplicationServer> replication_;
    std::unique_ptr<net::RpcRouter> rpc_;
    std::vector<std::pair<net::PeerId, Entity>> pawns_;
    ServerStats stats_;
    f64 last_now_ = 0;
};

// --- LAN discovery (aether_netclient --discover) ----------------------------------------------------

struct DiscoverConfig {
    u32 build_id = 1;            // results with another build are listed as incompatible
    u16 lan_port = net::kDefaultDiscoveryPort;
    f64 timeout = 1.0;           // seconds of listening (queries are repeated every 0.25 s)
    std::vector<net::Address> also_query = {net::Address::Loopback(0)}; // besides the broadcast; port 0: lan_port
};
// Broadcasts discovery queries and collects answers for `timeout` seconds, calling `pump` (if any) every
// few milliseconds (tests tick an in-process server there). Sorted by ping.
std::vector<net::LanSession> DiscoverServers(const DiscoverConfig& config, std::string* error,
                                            const std::function<void()>& pump = {});
std::string DiscoveryJson(const std::vector<net::LanSession>& servers);

// --- The headless test client ---------------------------------------------------------------------

struct NetClientConfig {
    net::Address server = net::Address::Loopback(7777);
    std::string name = "netclient";
    std::string password;
    std::string token;        // a join token (aether_server --issue-token)
    u32 build_id = 1;
    f64 duration = 5.0;       // seconds of replicated play after joining
    f64 join_timeout = 5.0;
    bool send_rpc = true;     // Ping our pawn once it arrives, and wait for the ack
};

struct NetClientResult {
    bool joined = false;
    std::string join_result = "Ok";
    std::string player_name;
    f64 join_seconds = -1;
    usize entities_seen = 0;   // most replicated entities at once
    u64 snapshots = 0;
    bool pawn_seen = false;
    u32 rpcs_sent = 0;
    bool rpc_ack = false;
    f64 rpc_ack_seconds = -1;
    u64 bytes_sent = 0, bytes_received = 0;
    bool Passed(bool want_rpc) const { return joined && entities_seen > 0 && (!want_rpc || rpc_ack); }
    std::string ToJson() const;
};

class NetClient {
public:
    NetClient() = default;
    ~NetClient();
    NetClient(const NetClient&) = delete;
    NetClient& operator=(const NetClient&) = delete;
    bool Start(const NetClientConfig& config, f64 now, std::string* error);
    void Tick(f64 now);
    // Joined and played for `duration`, or failed (refused / timed out).
    bool Done() const { return done_; }
    const NetClientResult& Result();
    World& GetWorld() { return world_; }
    // Optional host-level hook, invoked only after an accepted replication snapshot.
    // A game with Jolt may bind a physics reconciliation callback here; the
    // headless networking core deliberately has no physics dependency.
    void OnSnapshotApplied(net::ReplicationClient::SnapshotAppliedFn fn);

private:
    NetClientConfig config_;
    World world_;
    net::UdpSocket socket_;
    std::unique_ptr<net::NetHost> host_;
    std::unique_ptr<net::SessionClient> session_;
    std::unique_ptr<net::ReplicationClient> replication_;
    net::ReplicationClient::SnapshotAppliedFn snapshot_applied_;
    std::unique_ptr<net::RpcRouter> rpc_;
    NetClientResult result_;
    f64 start_ = 0, joined_at_ = -1, rpc_at_ = -1;
    bool done_ = false;
    bool left_ = false;
};

} // namespace aether::server
