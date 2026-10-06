#include "aether/net/replication.h"
#include "aether/scene/components.h"
#include "test_framework.h"

#include <algorithm>
#include <cmath>
#include <string>

// Phase 22 step 2: replication - spawns, field updates and despawns
// reaching clients, deltas against acknowledged snapshots, recovering from
// loss, relevancy and ownership, and the bandwidth budget with priorities.

using namespace aether;
using namespace aether::net;

#define CHECK(...) AETHER_CHECK((__VA_ARGS__))

namespace repl_test {
struct NetHealth {
    i32 hp = 100;
    f32 regen = 1.0f; // not replicated
    std::string name = "unit";
};
} // namespace repl_test

AETHER_REFLECT(repl_test::NetHealth, 1,
    AETHER_FIELD(hp, Field_Replicated),
    AETHER_FIELD(regen),
    AETHER_FIELD(name, Field_Replicated)
)

namespace {

using repl_test::NetHealth;
constexpr f64 kStep = 1.0 / 60.0;

struct ReplSim {
    LoopbackNetwork net;
    World server_world;
    std::unique_ptr<DatagramSocket> server_socket;
    std::unique_ptr<NetHost> server;
    std::unique_ptr<ReplicationServer> rs;
    std::vector<PeerId> server_peers; // in connection order
    struct Client {
        World world;
        std::unique_ptr<DatagramSocket> socket;
        std::unique_ptr<NetHost> host;
        std::unique_ptr<ReplicationClient> rc;
    };
    std::vector<std::unique_ptr<Client>> clients;

    explicit ReplSim(ReplicationConfig config = {}, u64 seed = 1) : net(seed) {
        server_socket = net.Open(7777);
        server = std::make_unique<NetHost>(*server_socket);
        server->Listen();
        rs = std::make_unique<ReplicationServer>(server_world, *server, config);
        cfg = config;
    }
    ReplicationConfig cfg;
    Client& AddClient() {
        auto c = std::make_unique<Client>();
        c->socket = net.Open();
        c->host = std::make_unique<NetHost>(*c->socket);
        const PeerId server_id = c->host->Connect(Address::Loopback(7777), net.Now());
        c->rc = std::make_unique<ReplicationClient>(c->world, *c->host, server_id, cfg);
        clients.push_back(std::move(c));
        return *clients.back();
    }
    void Step(int frames = 1) {
        for (int i = 0; i < frames; ++i) {
            net.Advance(kStep);
            server->Update(net.Now());
            for (const NetEvent& e : server->TakeEvents()) {
                if (e.type == NetEventType::Connected) server_peers.push_back(e.peer);
                rs->HandleEvent(e);
            }
            rs->Update(net.Now());
            for (auto& c : clients) {
                c->host->Update(net.Now());
                for (const NetEvent& e : c->host->TakeEvents()) c->rc->HandleEvent(e);
            }
        }
    }
    Entity Spawn(const Vec3& at, const std::string& archetype = "unit", f32 radius = 0, PeerId owner = kNoPeer) {
        NetIdentity ni;
        SetNetArchetype(ni, archetype);
        ni.relevancy_radius = radius;
        ni.owner = owner;
        return server_world.CreateEntity(ni, Transform{at, Quaternion{}}, NetHealth{});
    }
    u32 NetId(Entity e) { return server_world.GetComponent<NetIdentity>(e)->net_id; }
};

bool Near(const Vec3& a, const Vec3& b) { return (a - b).Length() < 1e-4f; }

} // namespace

AETHER_TEST(Replication_SpawnsUpdatesAndDespawns) {
    ReplSim sim;
    auto& client = sim.AddClient();
    int spawned_units = 0, despawned = 0;
    client.rc->OnSpawn("unit", [&](World& w, Entity e, const NetIdentity& ni) {
        ++spawned_units;
        CHECK(ni.net_id != 0 && std::string(ni.archetype) == "unit" && !ni.locally_owned);
        w.AddComponent(e, ModelRenderer{}); // a component that is not replicated
    });
    client.rc->OnDespawn([&](World&, Entity) { ++despawned; });
    Entity a = sim.Spawn({1, 2, 3}), b = sim.Spawn({4, 5, 6}), c = sim.Spawn({7, 8, 9}, "crate");
    sim.server_world.GetComponent<NetHealth>(a)->regen = 5.0f;
    sim.server_world.GetComponent<NetHealth>(b)->name = "Bob the builder";
    sim.Step(30);
    CHECK(client.rc->EntityCount() == 3 && spawned_units == 2);
    CHECK(sim.NetId(a) != 0 && sim.NetId(a) != sim.NetId(b) && sim.NetId(b) != sim.NetId(c));
    CHECK(sim.rs->FindEntity(sim.NetId(b)) == b);
    for (Entity s : {a, b, c}) {
        Entity e = client.rc->FindEntity(sim.NetId(s));
        CHECK(!e.IsNull());
        CHECK(Near(client.world.GetComponent<Transform>(e)->position, sim.server_world.GetComponent<Transform>(s)->position));
        CHECK(client.world.GetComponent<NetHealth>(e)->name == sim.server_world.GetComponent<NetHealth>(s)->name);
    }
    Entity ca = client.rc->FindEntity(sim.NetId(a));
    CHECK(client.world.GetComponent<NetHealth>(ca)->regen == 1.0f); // not replicated: the default
    CHECK(client.world.HasComponent<ModelRenderer>(ca));
    CHECK(!client.world.HasComponent<ModelRenderer>(client.rc->FindEntity(sim.NetId(c)))); // "crate" has no hook

    // Updates: a move, a rotation and a field.
    sim.server_world.GetComponent<Transform>(a)->position = Vec3(10, 0, 0);
    sim.server_world.GetComponent<Transform>(a)->rotation = Quaternion::FromAxisAngle(Vec3(0, 1, 0), 1.0f);
    sim.server_world.GetComponent<NetHealth>(b)->hp = 42;
    sim.Step(10);
    CHECK(Near(client.world.GetComponent<Transform>(ca)->position, Vec3(10, 0, 0)));
    const Quaternion q = client.world.GetComponent<Transform>(ca)->rotation;
    const Quaternion sq = sim.server_world.GetComponent<Transform>(a)->rotation;
    CHECK(std::fabs(q.y - sq.y) < 1e-6f && std::fabs(q.w - sq.w) < 1e-6f);
    CHECK(client.world.GetComponent<NetHealth>(client.rc->FindEntity(sim.NetId(b)))->hp == 42);

    // A component taken away, then the entity.
    sim.server_world.RemoveComponent<NetHealth>(c);
    sim.Step(10);
    CHECK(!client.world.HasComponent<NetHealth>(client.rc->FindEntity(sim.NetId(c))));
    const u32 b_id = sim.NetId(b);
    sim.server_world.DestroyEntity(b);
    sim.Step(10);
    CHECK(client.rc->EntityCount() == 2 && despawned == 1 && client.rc->FindEntity(b_id).IsNull());
    CHECK(client.world.EntityCount() == 2);
    CHECK(client.rc->ServerTime() > 0.0);
}

AETHER_TEST(Replication_SendsOnlyWhatChanged) {
    ReplSim sim;
    auto& client = sim.AddClient();
    std::vector<Entity> units;
    for (int i = 0; i < 20; ++i) units.push_back(sim.Spawn(Vec3(static_cast<f32>(i), 0, 0)));
    sim.Step(5);
    usize first = 0;
    for (int i = 0; i < 60; ++i) {
        sim.Step();
        if (!sim.server_peers.empty()) first = std::max(first, sim.rs->Stats(sim.server_peers[0]).last_bytes);
    }
    sim.Step(30);
    const auto stats = sim.rs->Stats(sim.server_peers[0]);
    CHECK(client.rc->EntityCount() == 20);
    CHECK(stats.acked != 0xFFFF);
    CHECK(first > 20 * 20);          // the first carries everything
    CHECK(stats.last_bytes == 17);   // idle: just the header
    const u64 written = client.rc->GetStats().fields_written;
    sim.Step(30);
    CHECK(client.rc->GetStats().fields_written == written); // nothing changed, nothing written
    sim.server_world.GetComponent<Transform>(units[7])->position = Vec3(0, 50, 0);
    sim.Step(4); // one send
    const auto moved = sim.rs->Stats(sim.server_peers[0]);
    CHECK(moved.entities_sent == 1);
    CHECK(moved.last_bytes > 17 && moved.last_bytes < 17 + 40);
    sim.Step(10);
    CHECK(client.rc->GetStats().fields_written == written + 1); // just the position
    CHECK(Near(client.world.GetComponent<Transform>(client.rc->FindEntity(sim.NetId(units[7])))->position, Vec3(0, 50, 0)));
}

AETHER_TEST(Replication_ConvergesThroughLoss) {
    ReplSim sim({}, 9);
    sim.net.conditions.latency = 0.05;
    sim.net.conditions.jitter = 0.03;
    sim.net.conditions.loss = 0.3;
    sim.net.conditions.duplicate = 0.05;
    auto& client = sim.AddClient();
    std::vector<Entity> units;
    for (int i = 0; i < 10; ++i) units.push_back(sim.Spawn(Vec3(0, 0, 0)));
    for (int frame = 0; frame < 240; ++frame) {
        const f32 t = static_cast<f32>(frame) * 0.1f;
        for (usize i = 0; i < units.size(); ++i) {
            sim.server_world.GetComponent<Transform>(units[i])->position = Vec3(std::sin(t + static_cast<f32>(i)), t, 0);
            sim.server_world.GetComponent<NetHealth>(units[i])->hp = frame;
        }
        if (frame == 120) sim.server_world.DestroyEntity(units[3]), sim.server_world.DestroyEntity(units[4]);
        if (frame == 150) units.push_back(sim.Spawn(Vec3(5, 5, 5), "late"));
        if (frame == 120) units.erase(units.begin() + 3, units.begin() + 5);
        sim.Step();
    }
    sim.Step(180); // settle
    CHECK(sim.net.Dropped() > 0);
    CHECK(client.rc->EntityCount() == units.size());
    for (Entity s : units) {
        Entity e = client.rc->FindEntity(sim.NetId(s));
        CHECK(!e.IsNull());
        if (e.IsNull()) continue;
        CHECK(Near(client.world.GetComponent<Transform>(e)->position, sim.server_world.GetComponent<Transform>(s)->position));
        CHECK(client.world.GetComponent<NetHealth>(e)->hp == sim.server_world.GetComponent<NetHealth>(s)->hp);
    }
    CHECK(client.rc->GetStats().undecodable == 0);
    CHECK(client.world.EntityCount() == units.size());
}

AETHER_TEST(Replication_RelevancyAndOwnership) {
    ReplSim sim;
    auto& c1 = sim.AddClient();
    auto& c2 = sim.AddClient();
    sim.Step(10);
    CHECK(sim.server_peers.size() == 2);
    const PeerId p1 = sim.server_peers[0], p2 = sim.server_peers[1];
    Entity near = sim.Spawn({0, 0, 0}, "unit", 10);
    Entity far = sim.Spawn({100, 0, 0}, "unit", 10);
    Entity everywhere = sim.Spawn({500, 0, 0}, "unit", 0);
    Entity pawn = sim.Spawn({102, 0, 0}, "pawn", 10, p2); // client 2 views from its pawn
    sim.rs->SetViewer(p1, Vec3(0, 0, 0));
    sim.Step(20);
    CHECK(c1.rc->EntityCount() == 2);
    CHECK(!c1.rc->FindEntity(sim.NetId(near)).IsNull() && !c1.rc->FindEntity(sim.NetId(everywhere)).IsNull());
    CHECK(c2.rc->EntityCount() == 3);
    CHECK(!c2.rc->FindEntity(sim.NetId(far)).IsNull() && c2.rc->FindEntity(sim.NetId(near)).IsNull());
    const Entity my_pawn = c2.rc->FindEntity(sim.NetId(pawn));
    CHECK(!my_pawn.IsNull() && c2.world.GetComponent<NetIdentity>(my_pawn)->locally_owned);
    CHECK(c2.world.GetComponent<NetIdentity>(my_pawn)->owner == p2);

    // Client 1 moves over: what was near goes, what is there comes.
    sim.rs->SetViewer(p1, Vec3(101, 0, 0));
    sim.Step(20);
    CHECK(c1.rc->EntityCount() == 3);
    CHECK(c1.rc->FindEntity(sim.NetId(near)).IsNull() && !c1.rc->FindEntity(sim.NetId(far)).IsNull());
    const Entity their_pawn = c1.rc->FindEntity(sim.NetId(pawn));
    CHECK(!their_pawn.IsNull() && !c1.world.GetComponent<NetIdentity>(their_pawn)->locally_owned);

    // Ownership handed over.
    sim.server_world.GetComponent<NetIdentity>(pawn)->owner = p1;
    sim.Step(20);
    CHECK(c1.world.GetComponent<NetIdentity>(their_pawn)->locally_owned);
    CHECK(c2.world.GetComponent<NetIdentity>(my_pawn)->owner == p1 && !c2.world.GetComponent<NetIdentity>(my_pawn)->locally_owned);

    // A disconnected client is forgotten.
    c2.host->Disconnect(c2.host->Peers()[0], sim.net.Now());
    sim.Step(20);
    CHECK(sim.rs->Stats(p2).last_bytes == 0);
}

AETHER_TEST(Replication_BudgetSharesByPriority) {
    ReplicationConfig config;
    config.byte_budget = 300;
    ReplSim sim(config);
    auto& client = sim.AddClient();
    std::vector<Entity> units;
    for (int i = 0; i < 50; ++i) {
        units.push_back(sim.Spawn(Vec3(static_cast<f32>(i), 0, 0)));
        sim.server_world.GetComponent<NetHealth>(units.back())->name = "unit number " + std::to_string(i);
    }
    sim.Step(10);
    usize max_bytes = 0;
    bool deferred = false;
    for (int i = 0; i < 120; ++i) {
        sim.Step();
        const auto s = sim.rs->Stats(sim.server_peers[0]);
        max_bytes = std::max(max_bytes, s.last_bytes);
        deferred = deferred || s.entities_deferred > 0;
    }
    CHECK(deferred);
    CHECK(max_bytes <= 300);
    CHECK(client.rc->EntityCount() == 50); // everything arrives in the end

    // Everything changes every frame; the important one keeps up best.
    sim.server_world.GetComponent<NetIdentity>(units[25])->priority = 20.0f;
    for (int frame = 1; frame <= 120; ++frame) {
        for (Entity e : units) sim.server_world.GetComponent<NetHealth>(e)->hp = 1000 + frame;
        sim.Step();
    }
    auto lag = [&](Entity s) {
        return sim.server_world.GetComponent<NetHealth>(s)->hp -
               client.world.GetComponent<NetHealth>(client.rc->FindEntity(sim.NetId(s)))->hp;
    };
    i32 worst = 0;
    f64 total = 0;
    for (Entity e : units) {
        worst = std::max(worst, lag(e));
        total += lag(e);
    }
    CHECK(lag(units[25]) <= 12);              // about one send plus the round trip
    CHECK(lag(units[25]) * 2 < total / 50.0); // well ahead of the average
    CHECK(worst > lag(units[25]));
}
