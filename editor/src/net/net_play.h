#pragma once

#include "aether/net/interpolation.h"
#include "aether/net/prediction.h"
#include "aether/net/rpc.h"

#include <memory>
#include <string>
#include <vector>

namespace aether::editor {

enum class NetPlayMode : u8 { ListenServer, DedicatedServer };
const char* NetPlayModeName(NetPlayMode mode);

struct NetPlaySettings {
    NetPlayMode mode = NetPlayMode::ListenServer;
    u32 clients = 1;              // client worlds besides the server
    net::LinkConditions conditions; // applied to every datagram; can change while playing
    u64 seed = 1;
    bool replicate_scene = true;  // scene entities with a Transform get a NetIdentity
    bool spawn_players = true;    // a predicted "player" pawn per player
    Vec3 spawn_origin{0, 0, 0};
    f32 spawn_spacing = 2.0f;     // along x, per player
    f64 interpolation_delay = 0.1;
};

// Networked Play-in-Editor (Phase 22 step 6, docs/design/PHASE_SPECS.md
// §22.6): the edited world is copied into a server world, and N client
// worlds connect to it over a simulated network (net::LoopbackNetwork)
// with adjustable latency, jitter, loss and duplication - all in one
// process and deterministic for a seed. The server replicates, routes
// RPCs and runs predicted movement; each client replicates, predicts its
// pawn and interpolates the rest. A listen server is also a player (its
// pawn is moved on the server directly); a dedicated one isn't.
//
// For convenience in the editor, a client's newly spawned entity is given
// copies of the server entity's other components (meshes, lights), as if
// the client had loaded the same level and prefabs; a shipped game spawns
// those through ReplicationClient::OnSpawn.
class NetPlaySession {
public:
    NetPlaySession();
    ~NetPlaySession();

    bool Start(const World& edited, const NetPlaySettings& settings, std::string* error = nullptr);
    void Stop();
    bool Running() const { return server_ != nullptr; }
    // One frame for every machine: the network, then the server, then each client.
    void Tick(f64 dt);
    f64 Now() const;

    // Player input. Clients predict; the listen server's player moves on the server.
    bool MoveClient(usize client, f32 x, f32 z, bool jump, f32 dt);
    bool MoveListenPlayer(f32 x, f32 z, bool jump, f32 dt);

    const NetPlaySettings& Settings() const { return settings_; }
    void SetConditions(const net::LinkConditions& conditions);

    World& ServerWorld();
    usize ClientCount() const { return clients_.size(); }
    World& ClientWorld(usize client);
    Entity ServerPawn(usize client) const;  // the server's entity for that client's player
    Entity ClientPawn(usize client) const;  // that client's own pawn (null until replicated)
    Entity ListenPawn() const { return listen_pawn_; }

    struct ClientView {
        bool connected = false;
        f64 rtt = 0.0;
        f32 loss = 0.0f;
        u64 bytes_sent = 0, bytes_received = 0;
        usize entities = 0;
        u32 corrections = 0;
    };
    ClientView Client(usize client) const;

    net::ReplicationServer& Replication();
    const net::NetProfile& Profile() const;
    void ResetProfile();
    u64 ServerBytesSent() const;

private:
    struct Server;
    struct Peer;
    void OnServerEvent(const net::NetEvent& event);
    Entity SpawnPawn(net::PeerId owner, usize index);

    NetPlaySettings settings_;
    std::unique_ptr<net::LoopbackNetwork> net_;
    std::unique_ptr<Server> server_;
    std::vector<std::unique_ptr<Peer>> clients_;
    std::vector<std::pair<net::PeerId, Entity>> pawns_; // server side, by peer
    Entity listen_pawn_;
    usize players_spawned_ = 0;
};

} // namespace aether::editor
