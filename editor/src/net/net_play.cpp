#include "net/net_play.h"

#include "aether/scene/serialization.h"

#include <algorithm>

namespace aether::editor {

using namespace aether::net;

namespace {
constexpr u16 kGamePort = 7777;

std::vector<Entity> AllEntities(const World& world) {
    std::vector<Entity> entities;
    world.ForEachArchetype([&](const Archetype& archetype) {
        for (usize c = 0; c < archetype.ChunkCount(); ++c) {
            const Entity* chunk = archetype.EntityArray(c);
            entities.insert(entities.end(), chunk, chunk + archetype.ChunkEntityCount(c));
        }
    });
    return entities;
}
} // namespace

const char* NetPlayModeName(NetPlayMode mode) {
    return mode == NetPlayMode::ListenServer ? "Listen Server" : "Dedicated Server";
}

struct NetPlaySession::Server {
    World world;
    std::unique_ptr<DatagramSocket> socket;
    std::unique_ptr<NetHost> host;
    std::unique_ptr<ReplicationServer> replication;
    std::unique_ptr<RpcRouter> rpc;
    std::unique_ptr<MovementServer> movement;
};

struct NetPlaySession::Peer {
    World world;
    std::unique_ptr<DatagramSocket> socket;
    std::unique_ptr<NetHost> host;
    PeerId server = kNoPeer;
    std::unique_ptr<ReplicationClient> replication;
    std::unique_ptr<RpcRouter> rpc;
    std::unique_ptr<MovementClient> movement;
    std::unique_ptr<SnapshotInterpolation> interpolation;
};

NetPlaySession::NetPlaySession() = default;
NetPlaySession::~NetPlaySession() { Stop(); }

bool NetPlaySession::Start(const World& edited, const NetPlaySettings& settings, std::string* error) {
    Stop();
    settings_ = settings;
    net_ = std::make_unique<LoopbackNetwork>(settings.seed);
    net_->conditions = settings.conditions;
    server_ = std::make_unique<Server>();
    if (!LoadSceneFromMemory(server_->world, SaveSceneToMemory(edited), "<net play>")) {
        if (error) *error = "the edited world couldn't be copied";
        Stop();
        return false;
    }
    if (settings.replicate_scene)
        for (Entity e : AllEntities(server_->world))
            if (server_->world.HasComponent<Transform>(e) && !server_->world.HasComponent<NetIdentity>(e)) {
                NetIdentity ni;
                SetNetArchetype(ni, "scene");
                server_->world.AddComponent(e, ni);
            }
    server_->socket = net_->Open(kGamePort);
    HostConfig config;
    config.max_peers = std::max<u32>(settings.clients, 1);
    server_->host = std::make_unique<NetHost>(*server_->socket, config);
    server_->host->Listen();
    server_->replication = std::make_unique<ReplicationServer>(server_->world, *server_->host);
    server_->rpc = std::make_unique<RpcRouter>(server_->world, *server_->host, *server_->replication);
    server_->movement = std::make_unique<MovementServer>(server_->world, *server_->host, *server_->replication);
    server_->replication->MutableProfile().Reset(0.0);
    if (settings.mode == NetPlayMode::ListenServer && settings.spawn_players) listen_pawn_ = SpawnPawn(kNoPeer, players_spawned_++);

    for (u32 i = 0; i < settings.clients; ++i) {
        auto c = std::make_unique<Peer>();
        c->socket = net_->Open();
        c->host = std::make_unique<NetHost>(*c->socket);
        c->server = c->host->Connect(Address::Loopback(kGamePort), net_->Now());
        c->replication = std::make_unique<ReplicationClient>(c->world, *c->host, c->server);
        Server* server = server_.get();
        World* client_world = &c->world;
        c->replication->OnSpawn("", [server, client_world](World&, Entity e, const NetIdentity& ni) {
            // As if the client had loaded the same level: the server entity's other components.
            const Entity source = server->replication->FindEntity(ni.net_id);
            if (source.IsNull()) return;
            std::vector<u8> bytes;
            for (ComponentId id = 0; id < RegisteredComponentCount(); ++id) {
                const ComponentInfo& info = GetComponentInfo(id);
                if (id == GetComponentId<NetIdentity>() || !server->world.HasComponentRaw(source, id) ||
                    client_world->HasComponentRaw(e, id) || (!info.reflected && !info.trivially_copyable))
                    continue;
                bytes.clear();
                info.serialize(server->world.GetComponentRaw(source, id), bytes);
                client_world->AddComponentRaw(e, id);
                info.deserialize(client_world->GetComponentRaw(e, id), bytes.data(), bytes.size());
            }
        });
        c->rpc = std::make_unique<RpcRouter>(c->world, *c->host, *c->replication, c->server);
        c->movement = std::make_unique<MovementClient>(c->world, *c->host, *c->replication, c->server);
        c->interpolation = std::make_unique<SnapshotInterpolation>(c->world, *c->replication, settings.interpolation_delay);
        clients_.push_back(std::move(c));
    }
    return true;
}

void NetPlaySession::Stop() {
    clients_.clear(); // clients first: their sockets belong to the network
    server_.reset();
    net_.reset();
    pawns_.clear();
    listen_pawn_ = Entity{};
    players_spawned_ = 0;
}

f64 NetPlaySession::Now() const { return net_ ? net_->Now() : 0.0; }

void NetPlaySession::SetConditions(const LinkConditions& conditions) {
    settings_.conditions = conditions;
    if (net_) net_->conditions = conditions;
}

Entity NetPlaySession::SpawnPawn(PeerId owner, usize index) {
    NetIdentity ni;
    SetNetArchetype(ni, "player");
    ni.owner = owner;
    const Vec3 at = settings_.spawn_origin + Vec3(settings_.spawn_spacing * static_cast<f32>(index), 0, 0);
    NetMovement movement;
    movement.ground_height = at.y;
    return server_->world.CreateEntity(ni, Transform{at, Quaternion{}}, movement);
}

void NetPlaySession::OnServerEvent(const NetEvent& event) {
    if (!settings_.spawn_players) return;
    if (event.type == NetEventType::Connected) {
        pawns_.push_back({event.peer, SpawnPawn(event.peer, players_spawned_++)});
    } else if (event.type == NetEventType::Disconnected) {
        auto it = std::find_if(pawns_.begin(), pawns_.end(), [&](const auto& p) { return p.first == event.peer; });
        if (it != pawns_.end()) {
            if (server_->world.IsAlive(it->second)) server_->world.DestroyEntity(it->second);
            pawns_.erase(it);
        }
    }
}

void NetPlaySession::Tick(f64 dt) {
    if (!Running()) return;
    net_->Advance(dt);
    const f64 now = net_->Now();
    Server& s = *server_;
    s.host->Update(now);
    for (const NetEvent& e : s.host->TakeEvents()) {
        OnServerEvent(e);
        if (!s.replication->HandleEvent(e) && !s.movement->HandleEvent(e)) s.rpc->HandleEvent(e);
    }
    s.movement->Update(now);
    s.replication->Update(now);
    for (auto& c : clients_) {
        c->host->Update(now);
        for (const NetEvent& e : c->host->TakeEvents())
            if (!c->replication->HandleEvent(e) && !c->movement->HandleEvent(e)) c->rpc->HandleEvent(e);
        c->interpolation->Update(now);
    }
}

bool NetPlaySession::MoveClient(usize client, f32 x, f32 z, bool jump, f32 dt) {
    if (client >= clients_.size()) return false;
    const Entity pawn = ClientPawn(client);
    return !pawn.IsNull() && clients_[client]->movement->Predict(pawn, x, z, jump, dt);
}

bool NetPlaySession::MoveListenPlayer(f32 x, f32 z, bool jump, f32 dt) {
    if (!Running() || listen_pawn_.IsNull() || !server_->world.IsAlive(listen_pawn_)) return false;
    MovementInput input;
    input.dt = dt, input.move_x = x, input.move_z = z, input.jump = jump;
    StepMovement(*server_->world.GetComponent<Transform>(listen_pawn_), *server_->world.GetComponent<NetMovement>(listen_pawn_),
                 SanitizeInput(input));
    return true;
}

World& NetPlaySession::ServerWorld() { return server_->world; }
World& NetPlaySession::ClientWorld(usize client) { return clients_[client]->world; }

Entity NetPlaySession::ServerPawn(usize client) const {
    if (client >= clients_.size()) return Entity{};
    const std::optional<PeerInfo> info = clients_[client]->host->Peer(clients_[client]->server);
    if (!info || info->state != PeerState::Connected) return Entity{};
    for (const auto& [peer, pawn] : pawns_)
        if (peer == info->remote_id) return pawn;
    return Entity{};
}

Entity NetPlaySession::ClientPawn(usize client) const {
    if (client >= clients_.size()) return Entity{};
    const Peer& c = *clients_[client];
    for (const auto& [net_id, e] : c.replication->Entities()) {
        const NetIdentity* ni = c.world.IsAlive(e) ? c.world.GetComponent<NetIdentity>(e) : nullptr;
        if (ni && ni->locally_owned && std::string(ni->archetype) == "player") return e;
    }
    return Entity{};
}

NetPlaySession::ClientView NetPlaySession::Client(usize client) const {
    ClientView v;
    if (client >= clients_.size()) return v;
    const auto& c = *clients_[client];
    const std::optional<PeerInfo> info = c.host->Peer(c.server);
    v.connected = info && info->state == PeerState::Connected;
    if (info && info->connection) {
        const ConnectionStats& st = info->connection->Stats();
        v.rtt = st.rtt, v.loss = st.packet_loss, v.bytes_sent = st.bytes_sent, v.bytes_received = st.bytes_received;
    }
    v.entities = c.replication->EntityCount();
    v.corrections = c.movement->GetStats().corrections;
    return v;
}

ReplicationServer& NetPlaySession::Replication() { return *server_->replication; }
const NetProfile& NetPlaySession::Profile() const { return server_->replication->Profile(); }
void NetPlaySession::ResetProfile() {
    if (Running()) server_->replication->MutableProfile().Reset(Now());
}

u64 NetPlaySession::ServerBytesSent() const {
    u64 total = 0;
    if (!Running()) return 0;
    for (PeerId p : server_->host->Peers())
        if (const std::optional<PeerInfo> info = server_->host->Peer(p); info && info->connection)
            total += info->connection->Stats().bytes_sent;
    return total;
}

} // namespace aether::editor
