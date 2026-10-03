#include "net/net_play.h"

#include "aether/scene/components.h"
#include "aether/scene/serialization.h"

#include <cmath>

namespace aether::editor {

using namespace aether::net;

NetPlaySession::NetPlaySession(std::span<const u8> server_scene, const NetPlayConfig& config)
    : network_(config.seed), config_(config) {
    RegisterReplicatedComponent<Transform>();
    RegisterReplicatedComponent<NetIdentity>();
    network_.conditions = config.conditions;
    server_link_ = &network_.CreateEndpoint();
    server_ = std::make_unique<NetEndpoint>(*server_link_);
    server_->Listen();
    replication_ = std::make_unique<ReplicationServer>(server_world_, *server_);
    replication_->ReplicateOnlyOnSpawn<Transform>();
    if (!server_scene.empty()) LoadSceneFromMemory(server_world_, server_scene, "<net play>");
    for (usize i = 0; i < config.clients; ++i) AddClient();
}

NetPlaySession::~NetPlaySession() = default;

usize NetPlaySession::AddClient() {
    auto c = std::make_unique<ClientState>();
    c->link = &network_.CreateEndpoint();
    c->endpoint = std::make_unique<NetEndpoint>(*c->link);
    c->world = std::make_unique<World>();
    c->replication = std::make_unique<ReplicationClient>(*c->world);
    c->interpolator = std::make_unique<SnapshotInterpolator>();
    c->endpoint->Connect(server_link_->LocalAddress());
    clients_.push_back(std::move(c));
    return clients_.size() - 1;
}

void NetPlaySession::DisconnectClient(usize index) {
    if (index >= clients_.size()) return;
    ClientState& c = *clients_[index];
    c.endpoint->Disconnect(server_link_->LocalAddress());
    c.replication->Clear();
    c.interpolator->Clear();
}

void NetPlaySession::ReconnectClient(usize index) {
    if (index >= clients_.size()) return;
    ClientState& c = *clients_[index];
    if (!c.endpoint->IsConnected(server_link_->LocalAddress())) c.endpoint->Connect(server_link_->LocalAddress());
}

bool NetPlaySession::SetOwner(Entity entity, usize client_index) {
    if (client_index >= clients_.size() || !server_world_.IsAlive(entity)) return false;
    NetIdentity* id = server_world_.GetComponent<NetIdentity>(entity);
    if (id == nullptr) return false;
    id->owner = clients_[client_index]->link->LocalAddress();
    return true;
}

void NetPlaySession::StepClient(usize index) {
    ClientState& c = *clients_[index];
    c.endpoint->Update(time_);
    NetEvent e;
    while (c.endpoint->Poll(e)) {
        if (ReplicationClient::IsReplicationMessage(e)) {
            c.replication->Apply(e.data);
        } else if (e.type == NetEventType::Message && !e.data.empty() && e.data[0] == kSnapshotMessage) {
            c.interpolator->OnSnapshotMessage(e.data, time_);
        } else if (on_client_message) {
            on_client_message(index, e);
        }
    }
    c.interpolator->Apply(*c.world, *c.replication, time_, c.link->LocalAddress());
}

void NetPlaySession::Step(f64 dt) {
    time_ += dt;
    network_.Advance(dt);
    server_->Update(time_);
    NetEvent e;
    while (server_->Poll(e)) {
        if (on_server_message) on_server_message(e.peer, e);
    }

    if (config_.replication_rate > 0.0) {
        replication_clock_ += dt;
        const f64 period = 1.0 / config_.replication_rate;
        if (replication_clock_ >= period) {
            replication_clock_ = std::fmod(replication_clock_, period);
            replication_->Replicate();
        }
    }
    if (config_.snapshot_rate > 0.0) {
        snapshot_clock_ += dt;
        const f64 period = 1.0 / config_.snapshot_rate;
        if (snapshot_clock_ >= period) {
            snapshot_clock_ = std::fmod(snapshot_clock_, period);
            const auto messages = BuildSnapshotMessages(time_, CaptureSnapshots(server_world_), server_->Config().max_message_size);
            for (const auto& m : messages) {
                for (NetAddress peer : server_->ConnectedPeers()) server_->Send(peer, Channel::Unreliable, m);
            }
        }
    }
    for (usize i = 0; i < clients_.size(); ++i) StepClient(i);
}

NetPlayClientInfo NetPlaySession::Info(usize index) const {
    NetPlayClientInfo info;
    if (index >= clients_.size()) return info;
    const ClientState& c = *clients_[index];
    info.address = c.link->LocalAddress();
    info.connected = c.endpoint->IsConnected(server_link_->LocalAddress()) && server_->IsConnected(info.address);
    info.entities = c.replication->EntityCount();
    PeerStats server_side, client_side;
    if (server_->Stats(info.address, server_side)) {
        info.rtt = server_side.rtt;
        info.packets_lost = server_side.packets_lost;
        info.bytes_to_client = server_side.bytes_sent;
        info.bytes_from_client = server_side.bytes_received;
    }
    (void)client_side;
    return info;
}

u64 NetPlaySession::ServerBytesSent() const {
    u64 total = 0;
    for (const auto& c : clients_) {
        PeerStats s;
        if (server_->Stats(c->link->LocalAddress(), s)) total += s.bytes_sent;
    }
    return total;
}

} // namespace aether::editor
