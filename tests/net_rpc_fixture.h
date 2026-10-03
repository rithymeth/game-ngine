#pragma once

// A server and clients with replication and RPC routers on a simulated
// network, shared by the RPC tests (C++ and Blueprints; Luau).

#include "aether/net/rpc.h"
#include "aether/scene/components.h"

#include <memory>
#include <string>

namespace rpc_test {

using namespace aether;

inline net::RpcRouter* g_running = nullptr; // the router whose world is running a call, for Caller()

struct RpcWeapon {
    i32 shots = 0, power = 0, pings = 0;
    std::string effect, note;
    net::PeerId caller = net::kNoPeer;

    void Fire(i32 p) {
        ++shots, power = p;
        if (g_running) caller = g_running->Caller();
    }
    void Ping() { ++pings; }
    void PlayEffect(const std::string& name) { effect = name; }
    void Notify(const std::string& text) { note = text; }
    i32 Shots() const { return shots; }
};

} // namespace rpc_test

AETHER_REFLECT(rpc_test::RpcWeapon, 1,
    AETHER_FIELD(shots),
    AETHER_FIELD(effect),
    AETHER_METHOD(Fire, Fn_Server | Fn_BlueprintCallable, {"power"}),
    AETHER_METHOD(Ping, Fn_Server | Fn_Unreliable),
    AETHER_METHOD(PlayEffect, Fn_Multicast | Fn_BlueprintCallable, {"name"}),
    AETHER_METHOD(Notify, Fn_Client, {"text"}),
    AETHER_METHOD(Shots, Fn_Pure)
)

namespace rpc_test {

struct RpcSim {
    static constexpr f64 kStep = 1.0 / 60.0;
    net::LoopbackNetwork net;
    World world;
    std::unique_ptr<net::DatagramSocket> socket;
    std::unique_ptr<net::NetHost> host;
    std::unique_ptr<net::ReplicationServer> replication;
    std::unique_ptr<net::RpcRouter> rpc;
    std::vector<net::PeerId> peers;
    struct Client {
        World world;
        std::unique_ptr<net::DatagramSocket> socket;
        std::unique_ptr<net::NetHost> host;
        std::unique_ptr<net::ReplicationClient> replication;
        std::unique_ptr<net::RpcRouter> rpc;
    };
    std::vector<std::unique_ptr<Client>> clients;

    RpcSim() {
        (void)GetComponentId<RpcWeapon>();
        socket = net.Open(7777);
        host = std::make_unique<net::NetHost>(*socket);
        host->Listen();
        replication = std::make_unique<net::ReplicationServer>(world, *host);
        rpc = std::make_unique<net::RpcRouter>(world, *host, *replication);
    }
    Client& AddClient() {
        auto c = std::make_unique<Client>();
        c->socket = net.Open();
        c->host = std::make_unique<net::NetHost>(*c->socket);
        const net::PeerId server = c->host->Connect(net::Address::Loopback(7777), net.Now());
        c->replication = std::make_unique<net::ReplicationClient>(c->world, *c->host, server);
        c->replication->OnSpawn("", [](World& w, Entity e, const net::NetIdentity&) { w.AddComponent(e, RpcWeapon{}); });
        c->rpc = std::make_unique<net::RpcRouter>(c->world, *c->host, *c->replication, server);
        clients.push_back(std::move(c));
        return *clients.back();
    }
    void Step(int frames = 1) {
        for (int i = 0; i < frames; ++i) {
            net.Advance(kStep);
            host->Update(net.Now());
            for (const net::NetEvent& e : host->TakeEvents()) {
                if (e.type == net::NetEventType::Connected) peers.push_back(e.peer);
                g_running = rpc.get();
                if (!replication->HandleEvent(e)) rpc->HandleEvent(e);
                g_running = nullptr;
            }
            replication->Update(net.Now());
            for (auto& c : clients) {
                c->host->Update(net.Now());
                for (const net::NetEvent& e : c->host->TakeEvents()) {
                    g_running = c->rpc.get();
                    if (!c->replication->HandleEvent(e)) c->rpc->HandleEvent(e);
                    g_running = nullptr;
                }
            }
        }
    }
    Entity Spawn(const Vec3& at, net::PeerId owner = net::kNoPeer, f32 radius = 0) {
        net::NetIdentity ni;
        net::SetNetArchetype(ni, "pawn");
        ni.owner = owner;
        ni.relevancy_radius = radius;
        return world.CreateEntity(ni, Transform{at, Quaternion{}}, RpcWeapon{});
    }
    u32 NetId(Entity e) { return world.GetComponent<net::NetIdentity>(e)->net_id; }
    RpcWeapon& Weapon(Entity e) { return *world.GetComponent<RpcWeapon>(e); }
    RpcWeapon* Weapon(Client& c, Entity server_entity) {
        const Entity e = c.replication->FindEntity(NetId(server_entity));
        return e.IsNull() ? nullptr : c.world.GetComponent<RpcWeapon>(e);
    }
};

// Calls `name` on the RpcWeapon of `entity` in `world` the way gameplay code would.
inline RemoteCallResult Call(World& world, Entity entity, const char* name, std::vector<reflect::Any> args = {}) {
    const reflect::TypeInfo& type = reflect::Reflect<RpcWeapon>();
    RemoteCallResult result = RemoteCallResult::Refused;
    CallFunction(world, entity, type, *type.FindFunction(name), world.GetComponent<RpcWeapon>(entity), args, nullptr,
                 &result);
    return result;
}

} // namespace rpc_test
