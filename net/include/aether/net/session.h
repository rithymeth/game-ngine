#pragma once

#include "aether/net/host.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace aether::net {

// What a game session tells the world about itself.
struct SessionInfo {
    std::string name;     // "Ada's game"
    std::string map;
    std::string mode;
    u16 port = 0;         // where the game's NetHost listens
    u32 players = 0, max_players = 0; // max 0: the host's own limit
    u32 build = 0;        // clients must match it to join
    bool password = false; // a password is needed (it is never advertised)
    std::vector<std::pair<std::string, std::string>> properties;

    const std::string* Property(const std::string& key) const;
};

std::vector<u8> EncodeSessionInfo(const SessionInfo& info);
bool DecodeSessionInfo(std::span<const u8> data, SessionInfo& out);

constexpr u16 kDefaultDiscoveryPort = 7778;

// Answers LAN discovery queries (Phase 22 step 5, docs/design/PHASE_SPECS.md
// §22.5): listen on the discovery port, and every query with our protocol
// id gets the session's info back, with the query's nonce so the browser
// can time it.
class LanBeacon {
public:
    LanBeacon(DatagramSocket& socket, u32 protocol_id = HostConfig{}.protocol_id) : socket_(socket), protocol_(protocol_id) {}
    SessionInfo info;
    bool enabled = true; // when false, queries are read and ignored
    void Update();
    u32 Answered() const { return answered_; }

private:
    DatagramSocket& socket_;
    u32 protocol_;
    u32 answered_ = 0;
};

struct LanSession {
    Address address;      // the game's: the beacon's host and info.port
    SessionInfo info;
    f64 ping = 0.0;       // seconds from query to answer
    f64 last_seen = 0.0;
    bool compatible = true; // same build
};

// Finds sessions on the local network: Search broadcasts a query to the
// discovery port; answers arriving in Update become results, one per game
// address, sorted by ping. A session not heard from for `expiry` seconds
// is dropped.
class LanBrowser {
public:
    LanBrowser(DatagramSocket& socket, u32 build, u16 discovery_port = kDefaultDiscoveryPort,
               u32 protocol_id = HostConfig{}.protocol_id);
    void Search(f64 now);
    void Update(f64 now);
    std::vector<LanSession> Results() const;
    f64 expiry = 5.0;
    f64 auto_search = 0.0; // search again this often (0: only when asked)

private:
    DatagramSocket& socket_;
    u32 build_;
    u16 port_;
    u32 protocol_;
    u64 nonce_seed_;
    f64 last_search_ = -1e300;
    std::vector<std::pair<u64, f64>> queries_; // nonce, when sent
    std::vector<LanSession> sessions_;
};

// --- Hosting and joining -------------------------------------------------------------------------

enum class JoinResult : u8 { Ok, WrongPassword, BuildMismatch, Full, NoRequest, Kicked, ConnectFailed, Lost };
const char* JoinResultName(JoinResult result);

struct SessionConfig {
    u8 channel = 0;            // a ReliableOrdered channel
    f64 join_timeout = 3.0;    // a connected peer must ask to join within this
    f64 linger = 1.0;          // a refused peer is dropped once the refusal is acked, or after this
};

struct SessionPlayer {
    PeerId peer = kNoPeer;
    std::string name;
};

enum class SessionEventType : u8 { PlayerJoined, PlayerLeft, JoinRefused };
struct SessionEvent {
    SessionEventType type;
    PeerId peer = kNoPeer;
    std::string name;
    JoinResult reason = JoinResult::Ok;
};

// The host's side of a session: a connected peer becomes a player only once
// it has asked to join with the right build and password while there is
// room; otherwise it is told why and dropped. Player names are kept unique
// ("Ada", "Ada (2)"). Keeps info.players current, so a beacon can show it.
class SessionHost {
public:
    SessionHost(NetHost& host, SessionInfo info, std::string password = {}, SessionConfig config = {});

    bool HandleEvent(const NetEvent& event, f64 now);
    void Update(f64 now);
    std::vector<SessionEvent> TakeEvents();

    const std::vector<SessionPlayer>& Players() const { return players_; }
    const SessionPlayer* Player(PeerId peer) const;
    void Kick(PeerId peer, f64 now);
    const SessionInfo& Info() const { return info_; }
    SessionInfo& MutableInfo() { return info_; }

private:
    void Refuse(PeerId peer, JoinResult reason, f64 now);
    NetHost& host_;
    SessionInfo info_;
    std::string password_;
    SessionConfig config_;
    std::vector<std::pair<PeerId, f64>> pending_;  // connected, not yet asked
    std::vector<std::pair<PeerId, f64>> leaving_;  // refused: dropped when the refusal is through
    std::vector<SessionPlayer> players_;
    std::vector<SessionEvent> events_;
};

// The joining side: connects, asks to join, and ends Joined (with the
// session's info and the name the host gave us) or Failed with a reason.
class SessionClient {
public:
    explicit SessionClient(NetHost& host, SessionConfig config = {}) : host_(host), config_(config) {}

    void Join(const Address& server, const std::string& player_name, const std::string& password, u32 build, f64 now);
    void Leave(f64 now);
    bool HandleEvent(const NetEvent& event);

    enum class State : u8 { Idle, Connecting, Joined, Failed };
    State GetState() const { return state_; }
    JoinResult Result() const { return result_; }
    PeerId Server() const { return server_; }
    const SessionInfo& Info() const { return info_; }
    const std::string& PlayerName() const { return name_; }

private:
    NetHost& host_;
    SessionConfig config_;
    State state_ = State::Idle;
    JoinResult result_ = JoinResult::Ok;
    PeerId server_ = kNoPeer;
    std::string name_, password_;
    u32 build_ = 0;
    SessionInfo info_;
};

// --- Lobbies ---------------------------------------------------------------------------------------

struct LobbyEntry {
    std::string id; // the service's name for it (on the LAN: the game address)
    Address address;
    SessionInfo info;
    f64 ping = 0.0;
    bool compatible = true;
};

// Where sessions are advertised and found. The LAN one comes first;
// platform services (Steam, EOS, consoles) implement the same interface
// later, so games and the editor don't care which they talk to.
class LobbyService {
public:
    virtual ~LobbyService() = default;
    virtual const char* Name() const = 0;
    virtual bool Advertise(const SessionInfo& info) = 0; // start, or update what is shown
    virtual void StopAdvertising() = 0;
    virtual void Search(f64 now) = 0;
    virtual std::vector<LobbyEntry> Results() const = 0;
    virtual void Update(f64 now) = 0;
};

// Lobbies on the local network: a LanBeacon while advertising, a LanBrowser for searches.
class LanLobbyService final : public LobbyService {
public:
    // `discovery` is bound to the discovery port to advertise; `search` is any socket.
    LanLobbyService(DatagramSocket* discovery, DatagramSocket& search, u32 build,
                    u16 discovery_port = kDefaultDiscoveryPort, u32 protocol_id = HostConfig{}.protocol_id);
    const char* Name() const override { return "LAN"; }
    bool Advertise(const SessionInfo& info) override;
    void StopAdvertising() override { advertising_ = false; }
    void Search(f64 now) override { browser_.Search(now); }
    std::vector<LobbyEntry> Results() const override;
    void Update(f64 now) override;
    bool Advertising() const { return advertising_; }

private:
    std::unique_ptr<LanBeacon> beacon_;
    LanBrowser browser_;
    bool advertising_ = false;
};

} // namespace aether::net
