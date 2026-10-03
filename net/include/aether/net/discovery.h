#pragma once

#include "aether/net/transport.h"

#include <string>
#include <vector>

namespace aether::net {

// LAN discovery (Phase 22 step 6): a host answers "who is hosting?" queries
// with a description of its session, and a browser broadcasts a query and
// collects the answers. They use their own Transport (their own socket on
// the well-known discovery port), separate from the game's.
//
//   Query:    [kDiscoveryQuery][u32 magic][string game][u32 nonce]
//   Response: [kDiscoveryResponse][u32 magic][u32 nonce][string game][string name][string map]
//             [u32 game version][u16 game port][u8 players][u8 max players]
//
// A browser learns where a session is from the datagram's source (the
// host's address) plus `game_port`: the discovery socket and the game
// socket are different ports on the same machine.

inline constexpr u8 kDiscoveryQuery = 0xD1;
inline constexpr u8 kDiscoveryResponse = 0xD2;
inline constexpr u16 kDefaultDiscoveryPort = 47777;
inline constexpr usize kMaxDiscoveryString = 64;

struct SessionInfo {
    std::string name;
    std::string map;
    u32 game_version = 1;
    u16 game_port = 0;
    u8 players = 0;
    u8 max_players = 0;
};

class LanHost {
public:
    LanHost(Transport& discovery, std::string game) : transport_(discovery), game_(std::move(game)) {}

    // Starts (or updates) advertising; call again when the player count changes.
    void SetSession(const SessionInfo& info) { info_ = info; advertising_ = true; }
    void Stop() { advertising_ = false; }

    // Answers the queries that arrived. `now` (seconds) paces the reply rate.
    void Update(f64 now);

    usize max_replies_per_second = 50; // a flood of queries can't turn this host into a reflector
    u64 Answered() const { return answered_; }
    u64 Ignored() const { return ignored_; }

private:
    Transport& transport_;
    std::string game_;
    SessionInfo info_;
    bool advertising_ = false;
    f64 window_start_ = 0.0;
    usize window_count_ = 0;
    u64 answered_ = 0, ignored_ = 0;
};

struct DiscoveredSession {
    NetAddress host = 0; // the host's discovery address on the browser's transport
    SessionInfo info;
    f64 round_trip = 0.0; // seconds, from the query that was answered
    f64 last_seen = 0.0;
};

class LanBrowser {
public:
    LanBrowser(Transport& discovery, std::string game) : transport_(discovery), game_(std::move(game)) {}

    // Broadcasts a query to the hosts listening on `port`.
    bool Search(f64 now, u16 port = kDefaultDiscoveryPort);
    // Queries one known host (a saved server, or a refresh) instead of broadcasting.
    bool SearchHost(NetAddress host, f64 now);
    // Collects answers.
    void Update(f64 now);
    // Forgets sessions not heard from for `max_age` seconds.
    void Prune(f64 now, f64 max_age);

    const std::vector<DiscoveredSession>& Sessions() const { return sessions_; }
    u64 Malformed() const { return malformed_; }

private:
    std::vector<u8> NextQuery(f64 now);
    Transport& transport_;
    std::string game_;
    u32 nonce_ = 0;
    f64 sent_at_ = 0.0;
    std::vector<DiscoveredSession> sessions_;
    u64 malformed_ = 0;
};

} // namespace aether::net
