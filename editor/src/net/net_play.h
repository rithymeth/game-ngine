#pragma once

#include "aether/net/interpolation.h"
#include "aether/net/replication.h"

#include <functional>
#include <memory>
#include <span>
#include <vector>

namespace aether::editor {

// Multi-client Play-in-Editor (Phase 22 step 7): a server world and N client
// worlds, each its own World, joined by a simulated network. The server world
// is loaded from a snapshot of the edited scene (SaveSceneToMemory); clients
// start empty and fill up through replication, exactly as they would over a
// real connection. Entities with a NetIdentity replicate; their Transforms
// travel as interpolated snapshots (the Transform is sent whole only at spawn).
//
// The network is a LoopbackNetwork, so latency, jitter, loss and duplication
// are adjustable live (Conditions()) and every run is reproducible by seed.
// A session is stepped by the editor each frame with the frame time.

struct NetPlayConfig {
    usize clients = 1;
    f64 replication_rate = 30.0; // replication passes per second
    f64 snapshot_rate = 20.0;    // transform snapshots per second
    net::LinkConditions conditions;
    u64 seed = 1;
};

struct NetPlayClientInfo {
    bool connected = false;
    f64 rtt = 0.0;               // seconds, as the server measures it
    u64 packets_lost = 0;        // server -> client packets never acknowledged
    u64 bytes_to_client = 0;
    u64 bytes_from_client = 0;
    usize entities = 0;          // replicated entities in the client's world
    net::NetAddress address = 0; // the client's address (what NetIdentity::owner holds)
};

class NetPlaySession {
public:
    NetPlaySession(std::span<const u8> server_scene, const NetPlayConfig& config = {});
    ~NetPlaySession();
    NetPlaySession(const NetPlaySession&) = delete;
    NetPlaySession& operator=(const NetPlaySession&) = delete;

    // Advances the network, the endpoints, replication and interpolation by `dt` seconds.
    void Step(f64 dt);

    World& Server() { return server_world_; }
    usize ClientCount() const { return clients_.size(); }
    World& Client(usize index) { return *clients_[index]->world; }
    net::ReplicationServer& Replication() { return *replication_; }
    net::NetEndpoint& ServerEndpoint() { return *server_; }
    net::NetEndpoint& ClientEndpoint(usize index) { return *clients_[index]->endpoint; }
    const net::SnapshotInterpolator& Interpolator(usize index) const { return *clients_[index]->interpolator; }
    net::LinkConditions& Conditions() { return network_.conditions; }
    f64 Time() const { return time_; }

    NetPlayClientInfo Info(usize index) const;
    // Another client joins (and receives the whole world); returns its index.
    usize AddClient();
    void DisconnectClient(usize index);
    void ReconnectClient(usize index);
    // Makes a client the owner of a server entity (it must have a NetIdentity).
    bool SetOwner(Entity server_entity, usize client_index);

    // Messages that aren't replication or snapshots (RPCs, game messages), as they arrive.
    std::function<void(net::NetAddress from, const net::NetEvent&)> on_server_message;
    std::function<void(usize client, const net::NetEvent&)> on_client_message;

    // Totals the Network panel graphs.
    u64 ServerBytesSent() const;

private:
    struct ClientState {
        net::LoopbackTransport* link = nullptr;
        std::unique_ptr<net::NetEndpoint> endpoint;
        std::unique_ptr<World> world;
        std::unique_ptr<net::ReplicationClient> replication;
        std::unique_ptr<net::SnapshotInterpolator> interpolator;
    };
    void StepClient(usize index);

    net::LoopbackNetwork network_;
    NetPlayConfig config_;
    net::LoopbackTransport* server_link_ = nullptr;
    std::unique_ptr<net::NetEndpoint> server_;
    World server_world_;
    std::unique_ptr<net::ReplicationServer> replication_;
    std::vector<std::unique_ptr<ClientState>> clients_;
    f64 time_ = 0.0;
    f64 replication_clock_ = 0.0;
    f64 snapshot_clock_ = 0.0;
};

} // namespace aether::editor
