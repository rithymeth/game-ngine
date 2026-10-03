#pragma once

#include "aether/net/discovery.h"
#include "aether/net/endpoint.h"

#include <deque>
#include <map>
#include <string>
#include <vector>

namespace aether::net {

// Game sessions (Phase 22 step 8): hosting and joining, the lobby, and
// starting a game, on top of NetEndpoint. The host admits players (checking
// the game version, password, name and room), keeps the player list and the
// lobby settings, and tells everyone about every change; clients see the
// same lobby and learn when the game starts. Both sides take the endpoint's
// events through HandleEvent, which consumes session messages (first byte
// kSessionMessage) and lets everything else, including Connected and
// Disconnected, pass back to the game.
//
//   Join -> JoinRequest -> JoinAccept (player id) | JoinReject (why)
//   then Lobby updates (reliable, whole state) -> StartGame (map, seed)
//
// A connection that doesn't send its JoinRequest within join_timeout is
// dropped, and nothing but a JoinRequest is accepted from it. Only the host
// can change the map, start the game or kick; the same messages from a client
// are ignored.

inline constexpr u8 kSessionMessage = 0xA5;
inline constexpr usize kMaxPlayerName = 24;
inline constexpr usize kMaxSessionPlayers = 32;

enum class SessionPhase : u8 { Lobby, InGame };

enum class JoinResult : u8 { Accepted, Full, VersionMismatch, BadPassword, BadName, InProgress };

struct SessionPlayer {
    u16 id = 0;
    std::string name;
    bool ready = false;
    bool is_host = false; // the host's own player in a listen server
    bool operator==(const SessionPlayer& o) const { return id == o.id && name == o.name && ready == o.ready && is_host == o.is_host; }
};

struct LobbyState {
    SessionPhase phase = SessionPhase::Lobby;
    std::string map;
    std::vector<SessionPlayer> players;
    const SessionPlayer* Find(u16 id) const {
        for (const SessionPlayer& p : players) {
            if (p.id == id) return &p;
        }
        return nullptr;
    }
    bool operator==(const LobbyState& o) const { return phase == o.phase && map == o.map && players == o.players; }
};

struct SessionConfig {
    std::string name = "Game";
    std::string map = "default";
    u32 game_version = 1;
    usize max_players = 8;
    std::string password;            // empty = open
    bool allow_join_in_progress = false;
    f64 join_timeout = 5.0;          // seconds a new connection has to send its JoinRequest
    std::string host_player_name;    // non-empty: the host plays too (a listen server), as player 1
};

// --- host -------------------------------------------------------------------

class SessionHost {
public:
    SessionHost(NetEndpoint& endpoint, const SessionConfig& config);

    // Feeds an endpoint event. True if it was a session message (consumed);
    // Connected/Disconnected events are acted on and still returned to the game (false).
    bool HandleEvent(const NetEvent& event, f64 now);
    // Drops connections that never joined.
    void Update(f64 now);

    const LobbyState& Lobby() const { return lobby_; }
    const SessionConfig& Config() const { return config_; }
    bool SetMap(const std::string& map);
    // Starts the game if every player is ready (or `force`). False if it isn't the lobby, there are no
    // players, or someone isn't ready.
    bool StartGame(bool force = false);
    u32 Seed() const { return seed_; }
    bool Kick(u16 player_id);
    bool SetHostReady(bool ready);
    // The peer address behind a player (0 for the host's own player or an unknown id).
    NetAddress AddressOf(u16 player_id) const;
    u16 PlayerOf(NetAddress peer) const;

    // What a LanHost should advertise for this session.
    SessionInfo Advertisement(u16 game_port) const;

    u64 Rejected() const { return rejected_; }
    u64 Malformed() const { return malformed_; }

private:
    struct Pending {
        f64 since = 0.0;
    };
    void HandleJoin(NetAddress peer, const std::vector<u8>& data, f64 now);
    void Broadcast(std::vector<u8> message);
    void BroadcastLobby();
    void Reject(NetAddress peer, JoinResult why);
    std::string UniqueName(const std::string& wanted) const;

    NetEndpoint& endpoint_;
    SessionConfig config_;
    LobbyState lobby_;
    u32 seed_ = 0;
    u16 next_id_ = 1;
    f64 now_ = 0.0;
    std::map<NetAddress, Pending> pending_;
    std::map<NetAddress, f64> closing_; // rejected or kicked peers, and when to cut them off if they have not left
    std::map<NetAddress, u16> players_; // peer -> player id
    u64 rejected_ = 0, malformed_ = 0;
};

// --- client -----------------------------------------------------------------

struct SessionEvent {
    enum class Type : u8 { Joined, Rejected, LobbyChanged, GameStarting, Kicked, Disconnected };
    Type type = Type::LobbyChanged;
    u16 player_id = 0;                      // Joined
    JoinResult reason = JoinResult::Accepted; // Rejected
    std::string map;                        // GameStarting
    u32 seed = 0;                           // GameStarting
};

class SessionClient {
public:
    explicit SessionClient(NetEndpoint& endpoint) : endpoint_(endpoint) {}

    // Connects to `server` and asks to join; the outcome arrives as a Joined or Rejected event.
    bool Join(NetAddress server, const std::string& player_name, u32 game_version, const std::string& password = "");
    void Leave();

    bool HandleEvent(const NetEvent& event);
    bool Poll(SessionEvent& out);

    bool SetReady(bool ready);

    bool Joined() const { return joined_; }
    u16 LocalPlayer() const { return player_id_; }
    const LobbyState& Lobby() const { return lobby_; }
    NetAddress Server() const { return server_; }
    u64 Malformed() const { return malformed_; }

private:
    void Push(SessionEvent e) { events_.push_back(std::move(e)); }
    NetEndpoint& endpoint_;
    NetAddress server_ = 0;
    std::string name_, password_;
    u32 version_ = 0;
    bool joined_ = false;
    bool request_sent_ = false;
    u16 player_id_ = 0;
    LobbyState lobby_;
    std::deque<SessionEvent> events_;
    u64 malformed_ = 0;
};

} // namespace aether::net
