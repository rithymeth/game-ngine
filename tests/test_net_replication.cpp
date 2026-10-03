#include "aether/net/replication.h"
#include "aether/scene/components.h"
#include "test_framework.h"

#include <algorithm>
#include <cmath>
#include <map>

// Phase 22 step 2: reflection-driven replication - the field codec, which
// components and fields replicate, spawn/delta/remove/despawn, late joiners,
// relevancy, loss, the byte budget and malformed input.

using namespace aether;
using namespace aether::net;

enum class NetTestMood : u8 { Calm, Angry, Furious };

struct NetTestStats {
    i32 health = 100;
    f32 speed = 1.0f;
    std::string name;
    bool alive = true;
    u8 team = 0;
    Vec3 spawn;
    i32 local_only = 0; // not replicated
};

struct NetTestBag {
    std::vector<i32> items;
    std::vector<std::string> tags;
    char label[16] = {};
    NetTestMood mood = NetTestMood::Calm;
    i64 big = 0;
    u64 ubig = 0;
    f64 precise = 0.0;
};

AETHER_ENUM(NetTestMood, 1, AETHER_ENUM_VALUE(Calm), AETHER_ENUM_VALUE(Angry), AETHER_ENUM_VALUE(Furious))

AETHER_REFLECT(NetTestStats, 1,
    AETHER_FIELD(health, Field_EditAnywhere | Field_Replicated),
    AETHER_FIELD(speed, Field_EditAnywhere | Field_Replicated),
    AETHER_FIELD(name, Field_EditAnywhere | Field_Replicated),
    AETHER_FIELD(alive, Field_EditAnywhere | Field_Replicated),
    AETHER_FIELD(team, Field_EditAnywhere | Field_Replicated),
    AETHER_FIELD(spawn, Field_EditAnywhere | Field_Replicated),
    AETHER_FIELD(local_only, Field_EditAnywhere)
)

AETHER_REFLECT(NetTestBag, 1,
    AETHER_FIELD(items, Field_EditAnywhere | Field_Replicated),
    AETHER_FIELD(tags, Field_EditAnywhere | Field_Replicated),
    AETHER_FIELD(label, Field_EditAnywhere | Field_Replicated),
    AETHER_FIELD(mood, Field_EditAnywhere | Field_Replicated),
    AETHER_FIELD(big, Field_EditAnywhere | Field_Replicated),
    AETHER_FIELD(ubig, Field_EditAnywhere | Field_Replicated),
    AETHER_FIELD(precise, Field_EditAnywhere | Field_Replicated)
)

namespace {

struct Plain { // not reflected: never replicates
    int x = 0;
};

// A server and any number of clients over one simulated network.
struct Session {
    LoopbackNetwork net;
    LoopbackTransport& server_link;
    NetEndpoint server;
    World server_world;
    ReplicationServer rs;
    struct Client {
        LoopbackTransport* link = nullptr;
        std::unique_ptr<NetEndpoint> endpoint;
        std::unique_ptr<World> world;
        std::unique_ptr<ReplicationClient> repl;
        usize other_messages = 0;
    };
    std::vector<std::unique_ptr<Client>> clients;
    f64 t = 0.0;

    explicit Session(u64 seed = 1)
        : net(seed), server_link(net.CreateEndpoint()), server(server_link), rs(server_world, server) {
        RegisterReplicatedComponent<NetTestStats>();
        RegisterReplicatedComponent<NetTestBag>();
        RegisterReplicatedComponent<Transform>();
        RegisterReplicatedComponent<NetIdentity>();
        server.Listen();
    }

    Client& AddClient() {
        auto c = std::make_unique<Client>();
        c->link = &net.CreateEndpoint();
        c->endpoint = std::make_unique<NetEndpoint>(*c->link);
        c->world = std::make_unique<World>();
        c->repl = std::make_unique<ReplicationClient>(*c->world);
        c->endpoint->Connect(server_link.LocalAddress());
        clients.push_back(std::move(c));
        return *clients.back();
    }

    void Tick(bool replicate = true, f64 dt = 0.02) {
        t += dt;
        net.Advance(dt);
        server.Update(t);
        NetEvent e;
        while (server.Poll(e)) {}
        for (auto& c : clients) {
            c->endpoint->Update(t);
            while (c->endpoint->Poll(e)) {
                if (ReplicationClient::IsReplicationMessage(e)) c->repl->Apply(e.data);
                else if (e.type == NetEventType::Message) ++c->other_messages;
            }
        }
        if (replicate) rs.Replicate();
    }
    void Run(int ticks, bool replicate = true) {
        for (int i = 0; i < ticks; ++i) Tick(replicate);
    }
    u32 IdOf(Entity e) { return server_world.GetComponent<NetIdentity>(e)->net_id; }
};

} // namespace

AETHER_TEST(NetCodec_RoundTripsEveryKind) {
    NetTestBag in;
    in.items = {1, -2, 300000, -4000000};
    in.tags = {"alpha", "", "gamma"};
    std::strcpy(in.label, "label text");
    in.mood = NetTestMood::Furious;
    in.big = -9000000000ll;
    in.ubig = 18000000000000000000ull;
    in.precise = 3.141592653589793;

    ByteWriter w;
    EncodeValue(reflect::Reflect<NetTestBag>(), &in, w);
    NetTestBag out;
    ByteReader r(w.Data());
    AETHER_CHECK(DecodeValue(reflect::Reflect<NetTestBag>(), &out, r) && r.Remaining() == 0);
    AETHER_CHECK(out.items == in.items && out.tags == in.tags && std::string(out.label) == "label text");
    AETHER_CHECK(out.mood == NetTestMood::Furious && out.big == in.big && out.ubig == in.ubig && out.precise == in.precise);

    NetTestStats stats;
    stats.health = -77;
    stats.spawn = Vec3(1.5f, -2.0f, 3.25f);
    stats.alive = false;
    ByteWriter ws;
    EncodeValue(reflect::Reflect<NetTestStats>(), &stats, ws);
    NetTestStats back;
    ByteReader rs(ws.Data());
    AETHER_CHECK(DecodeValue(reflect::Reflect<NetTestStats>(), &back, rs));
    AETHER_CHECK(back.health == -77 && !back.alive && back.spawn.y == -2.0f && back.spawn.z == 3.25f);
    AETHER_CHECK(back.local_only == 0); // the struct codec writes every non-transient field, so it is carried here
}

AETHER_TEST(NetCodec_SmallValuesAreSmall) {
    NetTestStats stats; // defaults
    ByteWriter w;
    const reflect::TypeInfo& type = reflect::Reflect<NetTestStats>();
    EncodeValue(*type.FindField("health")->type, &stats.health, w); // 100 as a zigzag varint: 2 bytes
    AETHER_CHECK(w.Size() == 2);
    ByteWriter b;
    EncodeValue(*type.FindField("team")->type, &stats.team, b);
    AETHER_CHECK(b.Size() == 1);
}

AETHER_TEST(NetCodec_RejectsMalformedBytes) {
    NetTestBag in;
    in.items = {1, 2, 3};
    in.tags = {"x"};
    ByteWriter w;
    EncodeValue(reflect::Reflect<NetTestBag>(), &in, w);
    const std::vector<u8> bytes = w.Data();
    NetTestBag out;
    // Every truncation fails, never crashes.
    for (usize n = 0; n < bytes.size(); ++n) {
        ByteReader r({bytes.data(), n});
        AETHER_CHECK(!DecodeValue(reflect::Reflect<NetTestBag>(), &out, r));
    }
    // An array claiming far more elements than there are bytes.
    ByteWriter bomb;
    bomb.Varint(1ull << 40);
    ByteReader r(bomb.Data());
    AETHER_CHECK(!DecodeValue(reflect::Reflect<NetTestBag>(), &out, r));
}

AETHER_TEST(NetReplication_OnlyReplicatedFieldsAndComponents) {
    const auto& all = ReplicatedComponents();
    auto find = [&](const char* name) -> const ReplicatedComponent* {
        for (const auto& rc : all) {
            if (std::string(rc.type->name) == name) return &rc;
        }
        return nullptr;
    };
    RegisterReplicatedComponent<NetTestStats>();
    RegisterReplicatedComponent<Transform>();
    RegisterReplicatedComponent<NetIdentity>();
    (void)GetComponentId<Plain>();
    const auto& now = ReplicatedComponents();
    for (const auto& rc : now) {
        const std::string n = rc.type->name;
        if (n == "NetTestStats") AETHER_CHECK(rc.fields.size() == 6); // not local_only
        if (n == "Transform") AETHER_CHECK(rc.fields.size() == 2);
        if (n == "NetIdentity") AETHER_CHECK(rc.fields.size() == 2);
        AETHER_CHECK(rc.hash == ComponentHash(GetComponentInfo(rc.id).name) && FindReplicated(rc.hash) == &rc);
    }
    (void)find;
    usize replicated_names = 0;
    for (const auto& rc : now) replicated_names += std::string(rc.type->name) == "NetTestStats" || std::string(rc.type->name) == "Transform";
    AETHER_CHECK(replicated_names == 2);
    AETHER_CHECK(FindReplicated(12345) == nullptr);
}

AETHER_TEST(NetReplication_SpawnCopiesReplicatedState) {
    Session s;
    Session::Client& c = s.AddClient();
    s.Run(60, false); // connect

    NetTestStats stats;
    stats.health = 42;
    stats.name = "orc";
    stats.team = 3;
    stats.spawn = Vec3(1, 2, 3);
    stats.local_only = 7;
    Transform t;
    t.position = Vec3(10, 0, -5);
    t.rotation = Quaternion::FromAxisAngle(Vec3(0, 1, 0), 1.0f);
    const Entity e = s.server_world.CreateEntity(NetIdentity{}, t, stats);
    const Entity ignored = s.server_world.CreateEntity(t, stats); // no NetIdentity: stays server-only
    (void)ignored;

    s.Run(10);
    const u32 id = s.IdOf(e);
    AETHER_CHECK(id != 0 && c.repl->EntityCount() == 1);
    const Entity ce = c.repl->EntityOf(id);
    AETHER_CHECK(!ce.IsNull() && c.world->IsAlive(ce));
    const NetTestStats* cs = c.world->GetComponent<NetTestStats>(ce);
    const Transform* ct = c.world->GetComponent<Transform>(ce);
    AETHER_CHECK(cs != nullptr && ct != nullptr);
    AETHER_CHECK(cs->health == 42 && cs->name == "orc" && cs->team == 3 && cs->spawn.z == 3.0f && cs->alive);
    AETHER_CHECK(cs->local_only == 0); // not replicated
    AETHER_CHECK(ct->position.x == 10.0f && ct->position.z == -5.0f && std::fabs(ct->rotation.w - t.rotation.w) < 1e-6f);
    AETHER_CHECK(c.world->GetComponent<NetIdentity>(ce)->net_id == id);
    AETHER_CHECK(c.world->EntityCount() == 1);
    AETHER_CHECK(s.rs.Knows(c.link->LocalAddress(), id) && s.rs.EntityOf(id) == e);
}

AETHER_TEST(NetReplication_SendsOnlyWhatChanged) {
    Session s;
    Session::Client& c = s.AddClient();
    s.Run(60, false);
    NetTestStats stats;
    stats.name = "a fairly long name so the full state is big";
    const Entity e = s.server_world.CreateEntity(NetIdentity{}, Transform{}, stats);
    s.Run(10);
    const u32 id = s.IdOf(e);
    const u64 spawn_bytes = s.rs.Stats().bytes_sent;
    AETHER_CHECK(spawn_bytes > 40);

    // Nothing changed: nothing sent.
    const u64 records = s.rs.Stats().records_sent;
    s.Run(10);
    AETHER_CHECK(s.rs.Stats().records_sent == records && s.rs.Stats().bytes_sent == spawn_bytes);

    // One field changed: one small record.
    s.server_world.GetComponent<NetTestStats>(e)->health = 55;
    s.Run(10);
    AETHER_CHECK(s.rs.Stats().records_sent == records + 1);
    const u64 delta_bytes = s.rs.Stats().bytes_sent - spawn_bytes;
    AETHER_CHECK(delta_bytes < 14 && delta_bytes < spawn_bytes / 3);
    AETHER_CHECK(c.world->GetComponent<NetTestStats>(c.repl->EntityOf(id))->health == 55);
    AETHER_CHECK(c.world->GetComponent<NetTestStats>(c.repl->EntityOf(id))->name == stats.name); // untouched fields stay

    // Moving updates the Transform only.
    s.server_world.GetComponent<Transform>(e)->position = Vec3(5, 6, 7);
    s.Run(10);
    AETHER_CHECK(c.world->GetComponent<Transform>(c.repl->EntityOf(id))->position.y == 6.0f);
    AETHER_CHECK(s.rs.Stats().updates == 2);
}

AETHER_TEST(NetReplication_ComponentsComeAndGo) {
    Session s;
    Session::Client& c = s.AddClient();
    s.Run(60, false);
    const Entity e = s.server_world.CreateEntity(NetIdentity{}, Transform{});
    s.Run(10);
    const Entity ce = c.repl->EntityOf(s.IdOf(e));
    AETHER_CHECK(!c.world->HasComponent<NetTestStats>(ce));

    NetTestStats stats;
    stats.health = 9;
    s.server_world.AddComponent(e, stats);
    NetTestBag bag;
    bag.items = {4, 5, 6};
    std::strcpy(bag.label, "bag");
    s.server_world.AddComponent(e, bag);
    s.Run(10);
    AETHER_CHECK(c.world->HasComponent<NetTestStats>(ce) && c.world->GetComponent<NetTestStats>(ce)->health == 9);
    AETHER_CHECK(c.world->GetComponent<NetTestBag>(ce)->items == (std::vector<i32>{4, 5, 6}));
    AETHER_CHECK(std::string(c.world->GetComponent<NetTestBag>(ce)->label) == "bag");

    s.server_world.RemoveComponent<NetTestStats>(e);
    s.Run(10);
    AETHER_CHECK(!c.world->HasComponent<NetTestStats>(ce) && c.world->HasComponent<NetTestBag>(ce));
    AETHER_CHECK(s.rs.Stats().removals == 1);

    // The array changes by one element: the whole field is resent and replaced.
    s.server_world.GetComponent<NetTestBag>(e)->items.push_back(7);
    s.Run(10);
    AETHER_CHECK(c.world->GetComponent<NetTestBag>(c.repl->EntityOf(s.IdOf(e)))->items.size() == 4);
}

AETHER_TEST(NetReplication_DespawnAndOwnership) {
    Session s;
    Session::Client& c = s.AddClient();
    s.Run(60, false);
    const Entity a = s.server_world.CreateEntity(NetIdentity{}, Transform{});
    const Entity b = s.server_world.CreateEntity(NetIdentity{}, Transform{});
    s.Run(10);
    const u32 id_a = s.IdOf(a), id_b = s.IdOf(b);
    AETHER_CHECK(id_a != id_b && c.repl->EntityCount() == 2);

    // Ownership is just a replicated field.
    s.server_world.GetComponent<NetIdentity>(a)->owner = c.link->LocalAddress();
    s.Run(10);
    AETHER_CHECK(c.world->GetComponent<NetIdentity>(c.repl->EntityOf(id_a))->owner == c.link->LocalAddress());
    AETHER_CHECK(c.world->GetComponent<NetIdentity>(c.repl->EntityOf(id_b))->owner == 0);

    s.server_world.DestroyEntity(a);
    s.Run(10);
    AETHER_CHECK(c.repl->EntityCount() == 1 && c.repl->EntityOf(id_a).IsNull() && !c.repl->EntityOf(id_b).IsNull());
    AETHER_CHECK(c.world->EntityCount() == 1 && !s.rs.Knows(c.link->LocalAddress(), id_a));
    AETHER_CHECK(s.rs.Stats().despawns == 1);

    // Ids are never reused.
    const Entity again = s.server_world.CreateEntity(NetIdentity{}, Transform{});
    s.Run(10);
    AETHER_CHECK(s.IdOf(again) != id_a && s.IdOf(again) != id_b);
}

AETHER_TEST(NetReplication_LateJoinersGetEverything) {
    Session s;
    Session::Client& first = s.AddClient();
    s.Run(60, false);
    std::vector<Entity> entities;
    for (int i = 0; i < 100; ++i) { // more than fits in one message
        NetTestStats stats;
        stats.health = i;
        stats.name = "entity number " + std::to_string(i);
        Transform t;
        t.position = Vec3(static_cast<f32>(i), 0, 0);
        entities.push_back(s.server_world.CreateEntity(NetIdentity{}, t, stats));
    }
    s.Run(30);
    AETHER_CHECK(first.repl->EntityCount() == 100);

    s.server_world.GetComponent<NetTestStats>(entities[50])->health = 999;
    Session::Client& late = s.AddClient();
    s.Run(60);
    AETHER_CHECK(late.repl->EntityCount() == 100 && late.world->EntityCount() == 100);
    const Entity le = late.repl->EntityOf(s.IdOf(entities[50]));
    AETHER_CHECK(late.world->GetComponent<NetTestStats>(le)->health == 999);
    AETHER_CHECK(late.world->GetComponent<NetTestStats>(le)->name == "entity number 50");
    AETHER_CHECK(first.world->GetComponent<NetTestStats>(first.repl->EntityOf(s.IdOf(entities[50])))->health == 999);
}

AETHER_TEST(NetReplication_ReconnectingClientGetsASpawnAgain) {
    Session s;
    Session::Client& c = s.AddClient();
    s.Run(60, false);
    const Entity e = s.server_world.CreateEntity(NetIdentity{}, Transform{});
    s.Run(10);
    const u32 id = s.IdOf(e);
    AETHER_CHECK(s.rs.Knows(c.link->LocalAddress(), id));

    c.endpoint->Disconnect(s.server_link.LocalAddress());
    c.repl->Clear();
    s.Run(10);
    AETHER_CHECK(!s.rs.Knows(c.link->LocalAddress(), id) && c.world->EntityCount() == 0);

    c.endpoint->Connect(s.server_link.LocalAddress());
    s.Run(60);
    AETHER_CHECK(c.repl->EntityCount() == 1 && c.world->EntityCount() == 1);
}

AETHER_TEST(NetReplication_RelevancyByDistance) {
    Session s;
    Session::Client& c = s.AddClient();
    s.Run(60, false);
    const NetAddress peer = c.link->LocalAddress();
    Vec3 viewer(0, 0, 0);
    s.rs.relevancy = [&](NetAddress p, Entity e) {
        if (p != peer) return false;
        const Transform* t = s.server_world.GetComponent<Transform>(e);
        const Vec3 d = t->position - viewer;
        return d.Length() <= 50.0f;
    };

    Transform near_t, far_t;
    near_t.position = Vec3(10, 0, 0);
    far_t.position = Vec3(200, 0, 0);
    const Entity near_e = s.server_world.CreateEntity(NetIdentity{}, near_t);
    const Entity far_e = s.server_world.CreateEntity(NetIdentity{}, far_t);
    s.Run(10);
    AETHER_CHECK(c.repl->EntityCount() == 1 && !c.repl->EntityOf(s.IdOf(near_e)).IsNull() && c.repl->EntityOf(s.IdOf(far_e)).IsNull());

    // The viewer walks toward the far one: it spawns, and the near one despawns.
    viewer = Vec3(190, 0, 0);
    s.Run(10);
    AETHER_CHECK(c.repl->EntityCount() == 1 && c.repl->EntityOf(s.IdOf(near_e)).IsNull() && !c.repl->EntityOf(s.IdOf(far_e)).IsNull());
    AETHER_CHECK(s.rs.Stats().despawns == 1 && s.rs.Stats().spawns == 2);

    // Walking back brings the near one back with its current state.
    s.server_world.GetComponent<Transform>(near_e)->position = Vec3(11, 0, 0);
    viewer = Vec3(0, 0, 0);
    s.Run(10);
    AETHER_CHECK(c.world->GetComponent<Transform>(c.repl->EntityOf(s.IdOf(near_e)))->position.x == 11.0f);
}

AETHER_TEST(NetReplication_ConvergesUnderLossAndReordering) {
    Session s(9);
    Session::Client& c = s.AddClient();
    s.Run(60, false);
    s.net.conditions.latency = 0.03;
    s.net.conditions.jitter = 0.06;
    s.net.conditions.loss = 0.3;
    s.net.conditions.duplicate = 0.1;

    std::vector<Entity> entities;
    for (int i = 0; i < 10; ++i) entities.push_back(s.server_world.CreateEntity(NetIdentity{}, Transform{}, NetTestStats{}));
    for (int tick = 0; tick < 200; ++tick) {
        for (usize i = 0; i < entities.size(); ++i) {
            if (!s.server_world.IsAlive(entities[i])) continue;
            s.server_world.GetComponent<NetTestStats>(entities[i])->health = tick * 10 + static_cast<i32>(i);
            s.server_world.GetComponent<Transform>(entities[i])->position = Vec3(static_cast<f32>(tick), static_cast<f32>(i), 0);
        }
        if (tick % 40 == 20) s.server_world.DestroyEntity(entities[tick / 40]);
        s.Tick();
    }
    s.Run(400); // let the reliable channel catch up

    usize alive = 0;
    for (usize i = 0; i < entities.size(); ++i) {
        if (!s.server_world.IsAlive(entities[i])) continue;
        ++alive;
        const Entity ce = c.repl->EntityOf(s.IdOf(entities[i]));
        AETHER_CHECK(!ce.IsNull());
        if (ce.IsNull()) continue;
        AETHER_CHECK(c.world->GetComponent<NetTestStats>(ce)->health == 1990 + static_cast<i32>(i));
        AETHER_CHECK(c.world->GetComponent<Transform>(ce)->position.x == 199.0f);
    }
    AETHER_CHECK(alive == 5 && c.repl->EntityCount() == 5 && c.world->EntityCount() == 5);
    AETHER_CHECK(c.repl->Stats().malformed == 0);
}

AETHER_TEST(NetReplication_ByteBudgetDefersAndPrioritizes) {
    Session s;
    Session::Client& c = s.AddClient();
    s.Run(60, false);
    std::vector<Entity> entities;
    for (int i = 0; i < 40; ++i) entities.push_back(s.server_world.CreateEntity(NetIdentity{}, Transform{}, NetTestStats{}));
    s.Run(20);
    AETHER_CHECK(c.repl->EntityCount() == 40);

    s.rs.bytes_per_peer = 40; // a handful of updates per pass
    const u64 deferred_before = s.rs.Stats().deferred;
    for (Entity e : entities) s.server_world.GetComponent<NetTestStats>(e)->health = 500;
    s.Tick();
    const u64 first_pass = s.rs.Stats().updates;
    AETHER_CHECK(first_pass > 0 && first_pass < 40);
    AETHER_CHECK(s.rs.Stats().deferred > deferred_before);

    // Everything still arrives eventually, none starved.
    s.Run(60);
    for (Entity e : entities) AETHER_CHECK(c.world->GetComponent<NetTestStats>(c.repl->EntityOf(s.IdOf(e)))->health == 500);

    // A new spawn is not held back by the budget even when updates are waiting.
    for (Entity e : entities) s.server_world.GetComponent<NetTestStats>(e)->health = 600;
    const Entity fresh = s.server_world.CreateEntity(NetIdentity{}, Transform{}, NetTestStats{});
    s.Tick();
    s.Run(2);
    AETHER_CHECK(!c.repl->EntityOf(s.IdOf(fresh)).IsNull());
}

AETHER_TEST(NetReplication_OversizedRecordsAreSkippedNotFatal) {
    Session s;
    Session::Client& c = s.AddClient();
    s.Run(60, false);
    NetTestStats huge;
    huge.name = std::string(3000, 'x'); // cannot fit one message
    const Entity big = s.server_world.CreateEntity(NetIdentity{}, Transform{}, huge);
    const Entity ok = s.server_world.CreateEntity(NetIdentity{}, Transform{}, NetTestStats{});
    s.Run(20);
    AETHER_CHECK(s.rs.Stats().oversized > 0);
    AETHER_CHECK(c.repl->EntityOf(s.IdOf(big)).IsNull());
    AETHER_CHECK(!c.repl->EntityOf(s.IdOf(ok)).IsNull()); // the others are unaffected

    // Shrinking the field lets it through.
    s.server_world.GetComponent<NetTestStats>(big)->name = "small";
    s.Run(20);
    AETHER_CHECK(!c.repl->EntityOf(s.IdOf(big)).IsNull());

    // A big change to an entity that is already known is split per component and still arrives.
    NetTestBag bag;
    for (int i = 0; i < 100; ++i) bag.tags.push_back(std::string(8, 'a' + (i % 26)));
    s.server_world.AddComponent(ok, bag);
    s.server_world.GetComponent<NetTestStats>(ok)->name = std::string(600, 'n');
    s.Run(20);
    const Entity cok = c.repl->EntityOf(s.IdOf(ok));
    AETHER_CHECK(c.world->GetComponent<NetTestBag>(cok) != nullptr && c.world->GetComponent<NetTestBag>(cok)->tags.size() == 100);
    AETHER_CHECK(c.world->GetComponent<NetTestStats>(cok)->name.size() == 600);
}

AETHER_TEST(NetReplication_ClientRejectsMalformedMessages) {
    World world;
    RegisterReplicatedComponent<NetIdentity>();
    ReplicationClient client(world);
    const std::vector<u8> wrong_kind = {0x10, 1, 2};
    AETHER_CHECK(!client.Apply(wrong_kind));

    const std::vector<u8> unknown_record = {kReplicationMessage, 99, 1};
    AETHER_CHECK(!client.Apply(unknown_record));

    // An update for an entity that was never spawned.
    ByteWriter update;
    update.U8(kReplicationMessage);
    update.U8(2);
    update.Varint(77);
    update.U8(0);
    AETHER_CHECK(!client.Apply(update.Data()));

    // A spawn naming a component the client doesn't know.
    ByteWriter spawn;
    spawn.U8(kReplicationMessage);
    spawn.U8(1);
    spawn.Varint(5);
    spawn.U8(1);
    spawn.U32(0xDEADBEEF);
    spawn.U8(1);
    spawn.Varint(1);
    AETHER_CHECK(!client.Apply(spawn.Data()));

    // A mask with bits past the component's fields.
    ByteWriter mask;
    mask.U8(kReplicationMessage);
    mask.U8(1);
    mask.Varint(6);
    mask.U8(1);
    mask.U32(ComponentHash("NetIdentity"));
    mask.U8(1);
    mask.Varint(0xFF);
    AETHER_CHECK(!client.Apply(mask.Data()));

    // Truncated in the middle.
    ByteWriter cut;
    cut.U8(kReplicationMessage);
    cut.U8(1);
    cut.Varint(9);
    AETHER_CHECK(!client.Apply(cut.Data()));
    AETHER_CHECK(client.Stats().malformed == 6);
}

AETHER_TEST(NetReplication_ClientClearDestroysReplicatedEntities) {
    Session s;
    Session::Client& c = s.AddClient();
    s.Run(60, false);
    for (int i = 0; i < 5; ++i) s.server_world.CreateEntity(NetIdentity{}, Transform{});
    const Entity local = c.world->CreateEntity(Transform{}); // not replicated: must survive
    s.Run(10);
    AETHER_CHECK(c.world->EntityCount() == 6);
    c.repl->Clear();
    AETHER_CHECK(c.repl->EntityCount() == 0 && c.world->EntityCount() == 1 && c.world->IsAlive(local));
}

AETHER_TEST(NetReplication_OtherMessagesPassThrough) {
    Session s;
    Session::Client& c = s.AddClient();
    s.Run(60, false);
    const std::vector<u8> game_message = {0x01, 2, 3};
    s.server.Send(c.link->LocalAddress(), Channel::ReliableOrdered, game_message);
    s.Run(10);
    AETHER_CHECK(c.other_messages == 1);
    NetEvent e;
    e.type = NetEventType::Message;
    e.channel = Channel::Unreliable;
    e.data = {kReplicationMessage};
    AETHER_CHECK(!ReplicationClient::IsReplicationMessage(e)); // replication is reliable-only
}
