#include "aether/net/rpc.h"
#include "test_framework.h"

#include <algorithm>

// Phase 22 step 3: RPCs - Server/Client/Multicast calls over reflection, the
// server's checks (direction, ownership, arguments, rate), reliability and
// malformed or forged messages.

using namespace aether;
using namespace aether::net;

struct NetTestWeapon {
    i32 ammo = 10;
    i32 fired = 0;
    std::vector<i32> order;
    std::string last_text;
    std::string notice;
    i32 notice_value = 0;
    i32 pings = 0;
    i32 shouts = 0;
    i32 private_calls = 0;

    void Fire(i32 count) { // Server RPC
        fired += count;
        ammo -= count;
        order.push_back(count);
    }
    void Say(std::string text, std::vector<i32> numbers) { // Server RPC with several arguments
        last_text = text + ":" + std::to_string(numbers.size());
    }
    void Ping() { ++pings; } // unreliable Server RPC
    void Notify(std::string text, i32 value) { // Client RPC
        notice = text;
        notice_value = value;
    }
    void Shout(std::string text) { // Multicast RPC
        last_text = text;
        ++shouts;
    }
    void Secret() { ++private_calls; } // reflected, but not an RPC
};

AETHER_REFLECT(NetTestWeapon, 1,
    AETHER_FIELD(ammo, Field_EditAnywhere | Field_Replicated),
    AETHER_FIELD(fired, Field_EditAnywhere | Field_Replicated),
    AETHER_FIELD(order, Field_EditAnywhere),
    AETHER_FIELD(last_text, Field_EditAnywhere),
    AETHER_FIELD(notice, Field_EditAnywhere),
    AETHER_FIELD(notice_value, Field_EditAnywhere),
    AETHER_FIELD(pings, Field_EditAnywhere),
    AETHER_FIELD(shouts, Field_EditAnywhere),
    AETHER_FIELD(private_calls, Field_EditAnywhere),
    AETHER_METHOD(Fire, Fn_ServerRPC, {"count"}),
    AETHER_METHOD(Say, Fn_ServerRPC, {"text", "numbers"}),
    AETHER_METHOD(Ping, Fn_ServerRPC | Fn_RpcUnreliable),
    AETHER_METHOD(Notify, Fn_ClientRPC, {"text", "value"}),
    AETHER_METHOD(Shout, Fn_MulticastRPC, {"text"}),
    AETHER_METHOD(Secret, Fn_BlueprintCallable)
)

namespace {

struct Game {
    LoopbackNetwork net;
    LoopbackTransport& server_link;
    NetEndpoint server;
    World server_world;
    ReplicationServer repl;
    RpcServer rpc;
    struct Client {
        LoopbackTransport* link = nullptr;
        std::unique_ptr<NetEndpoint> endpoint;
        std::unique_ptr<World> world;
        std::unique_ptr<ReplicationClient> repl;
        std::unique_ptr<RpcClient> rpc;
        std::vector<RpcResult> results;
    };
    std::vector<std::unique_ptr<Client>> clients;
    std::vector<RpcResult> server_results;
    f64 t = 0.0;

    explicit Game(u64 seed = 1)
        : net(seed), server_link(net.CreateEndpoint()), server(server_link), repl(server_world, server), rpc(server_world, server, repl) {
        RegisterReplicatedComponent<NetTestWeapon>();
        RegisterReplicatedComponent<NetIdentity>();
        server.Listen();
    }

    Client& AddClient() {
        auto c = std::make_unique<Client>();
        c->link = &net.CreateEndpoint();
        c->endpoint = std::make_unique<NetEndpoint>(*c->link);
        c->world = std::make_unique<World>();
        c->repl = std::make_unique<ReplicationClient>(*c->world);
        c->rpc = std::make_unique<RpcClient>(*c->world, *c->endpoint, server_link.LocalAddress(), *c->repl);
        c->endpoint->Connect(server_link.LocalAddress());
        clients.push_back(std::move(c));
        return *clients.back();
    }

    void Tick(f64 dt = 0.02) {
        t += dt;
        net.Advance(dt);
        server.Update(t);
        NetEvent e;
        while (server.Poll(e)) {
            if (IsRpcMessage(e)) server_results.push_back(rpc.Handle(e.peer, e.data, t));
        }
        for (auto& c : clients) {
            c->endpoint->Update(t);
            while (c->endpoint->Poll(e)) {
                if (ReplicationClient::IsReplicationMessage(e)) c->repl->Apply(e.data);
                else if (IsRpcMessage(e)) c->results.push_back(c->rpc->Handle(e.data));
            }
        }
        repl.Replicate();
    }
    void Run(int ticks) {
        for (int i = 0; i < ticks; ++i) Tick();
    }
    // A replicated, owned weapon.
    Entity Spawn(const Client* owner) {
        NetIdentity id;
        id.owner = owner != nullptr ? owner->link->LocalAddress() : 0;
        return server_world.CreateEntity(id, NetTestWeapon{});
    }
    u32 IdOf(Entity e) { return server_world.GetComponent<NetIdentity>(e)->net_id; }
    NetTestWeapon* Server(Entity e) { return server_world.GetComponent<NetTestWeapon>(e); }
    NetTestWeapon* On(Client& c, Entity e) { return c.world->GetComponent<NetTestWeapon>(c.repl->EntityOf(IdOf(e))); }
    Entity Local(Client& c, Entity e) { return c.repl->EntityOf(IdOf(e)); }
};

usize CountOf(const std::vector<RpcResult>& v, RpcResult r) { return static_cast<usize>(std::count(v.begin(), v.end(), r)); }

} // namespace

AETHER_TEST(NetRpc_ListsRpcComponents) {
    RegisterReplicatedComponent<NetTestWeapon>();
    bool found = false;
    for (const RpcComponent& c : RpcComponents()) found = found || std::string(c.type->name) == "NetTestWeapon";
    AETHER_CHECK(found);
}

AETHER_TEST(NetRpc_ClientCallsServerAndTheResultReplicatesBack) {
    Game g;
    Game::Client& a = g.AddClient();
    g.Run(60);
    const Entity w = g.Spawn(&a);
    g.Run(10);
    const Entity lw = g.Local(a, w);
    AETHER_CHECK(!lw.IsNull());

    AETHER_CHECK(a.rpc->Call(lw, "NetTestWeapon", "Fire", 3));
    g.Run(10);
    AETHER_CHECK(g.Server(w)->fired == 3 && g.Server(w)->ammo == 7);
    AETHER_CHECK(CountOf(g.server_results, RpcResult::Ok) == 1);
    // The change flows back through replication.
    AETHER_CHECK(g.On(a, w)->ammo == 7 && g.On(a, w)->fired == 3);
    AETHER_CHECK(a.rpc->Stats().sent == 1 && g.rpc.Stats().executed == 1);
}

AETHER_TEST(NetRpc_ArgumentsOfSeveralTypes) {
    Game g;
    Game::Client& a = g.AddClient();
    g.Run(60);
    const Entity w = g.Spawn(&a);
    g.Run(10);
    AETHER_CHECK(a.rpc->Call(g.Local(a, w), "NetTestWeapon", "Say", std::string("hello"), std::vector<i32>{1, 2, 3, 4}));
    g.Run(10);
    AETHER_CHECK(g.Server(w)->last_text == "hello:4");
}

AETHER_TEST(NetRpc_ServerChecksOwnership) {
    Game g;
    Game::Client& a = g.AddClient();
    Game::Client& b = g.AddClient();
    g.Run(60);
    const Entity mine = g.Spawn(&a);
    const Entity servers = g.Spawn(nullptr); // owner 0: the server's
    g.Run(10);

    // B cannot fire A's weapon, nor can anyone fire the server's.
    AETHER_CHECK(b.rpc->Call(g.Local(b, mine), "NetTestWeapon", "Fire", 1)); // it sends; the server refuses
    AETHER_CHECK(a.rpc->Call(g.Local(a, servers), "NetTestWeapon", "Fire", 1));
    g.Run(10);
    AETHER_CHECK(g.Server(mine)->fired == 0 && g.Server(servers)->fired == 0);
    AETHER_CHECK(CountOf(g.server_results, RpcResult::NotAllowed) == 2 && g.rpc.Stats().rejected == 2);

    // The owner can.
    AETHER_CHECK(a.rpc->Call(g.Local(a, mine), "NetTestWeapon", "Fire", 2));
    g.Run(10);
    AETHER_CHECK(g.Server(mine)->fired == 2);
}

AETHER_TEST(NetRpc_ServerCallsClientAndMulticast) {
    Game g;
    Game::Client& a = g.AddClient();
    Game::Client& b = g.AddClient();
    Game::Client& c = g.AddClient();
    g.Run(60);
    const Entity w = g.Spawn(&a);
    // C never sees this entity.
    g.repl.relevancy = [&](NetAddress peer, Entity) { return peer != c.link->LocalAddress(); };
    g.Run(10);
    AETHER_CHECK(c.repl->EntityCount() == 0);

    // A Client RPC reaches only the owner.
    AETHER_CHECK(g.rpc.Call(w, "NetTestWeapon", "Notify", std::string("reload"), 9));
    g.Run(10);
    AETHER_CHECK(g.On(a, w)->notice == "reload" && g.On(a, w)->notice_value == 9);
    AETHER_CHECK(g.On(b, w)->notice.empty());

    // A Multicast reaches every client that has the entity, not the one that doesn't.
    AETHER_CHECK(g.rpc.Call(w, "NetTestWeapon", "Shout", std::string("boom")));
    g.Run(10);
    AETHER_CHECK(g.On(a, w)->shouts == 1 && g.On(a, w)->last_text == "boom");
    AETHER_CHECK(g.On(b, w)->shouts == 1 && g.On(b, w)->last_text == "boom");
    AETHER_CHECK(c.results.empty());

    // A server-owned entity has no client to call.
    const Entity own = g.Spawn(nullptr);
    g.Run(10);
    AETHER_CHECK(!g.rpc.Call(own, "NetTestWeapon", "Notify", std::string("x"), 1));
}

AETHER_TEST(NetRpc_ServerRpcCalledOnTheServerRunsLocally) {
    Game g;
    g.AddClient();
    g.Run(60);
    const Entity w = g.Spawn(nullptr);
    g.Run(5);
    AETHER_CHECK(g.rpc.Call(w, "NetTestWeapon", "Fire", 4));
    AETHER_CHECK(g.Server(w)->fired == 4);
}

AETHER_TEST(NetRpc_CallSiteValidation) {
    Game g;
    Game::Client& a = g.AddClient();
    g.Run(60);
    const Entity w = g.Spawn(&a);
    g.Run(10);
    const Entity lw = g.Local(a, w);

    AETHER_CHECK(!a.rpc->Call(lw, "NetTestWeapon", "Fire"));                       // too few arguments
    AETHER_CHECK(!a.rpc->Call(lw, "NetTestWeapon", "Fire", 1, 2));                 // too many
    AETHER_CHECK(!a.rpc->Call(lw, "NetTestWeapon", "Fire", std::string("three"))); // wrong type
    AETHER_CHECK(!a.rpc->Call(lw, "NetTestWeapon", "Fire", 1.5f));                 // wrong type
    AETHER_CHECK(!a.rpc->Call(lw, "NetTestWeapon", "Secret"));                     // reflected but not an RPC
    AETHER_CHECK(!a.rpc->Call(lw, "NetTestWeapon", "Notify", std::string("x"), 1)); // a Client RPC: clients can't call it
    AETHER_CHECK(!a.rpc->Call(lw, "NetTestWeapon", "Nope", 1));
    AETHER_CHECK(!a.rpc->Call(lw, "NoSuchComponent", "Fire", 1));
    const Entity unreplicated = a.world->CreateEntity(NetTestWeapon{});
    AETHER_CHECK(!a.rpc->Call(unreplicated, "NetTestWeapon", "Fire", 1));
    AETHER_CHECK(!a.rpc->Call(kNullEntity, "NetTestWeapon", "Fire", 1));
    AETHER_CHECK(!g.rpc.Call(g.server_world.CreateEntity(NetTestWeapon{}), "NetTestWeapon", "Shout", std::string("x"))); // no network id
    // Too big for one message.
    AETHER_CHECK(!a.rpc->Call(lw, "NetTestWeapon", "Say", std::string(5000, 'x'), std::vector<i32>{}));
    g.Run(10);
    AETHER_CHECK(g.Server(w)->fired == 0 && g.server_results.empty());
}

AETHER_TEST(NetRpc_ReliableCallsRunOnceInOrderOverALossyLink) {
    Game g(5);
    Game::Client& a = g.AddClient();
    g.Run(60);
    const Entity w = g.Spawn(&a);
    g.Run(10);
    const Entity lw = g.Local(a, w);
    g.net.conditions.latency = 0.03;
    g.net.conditions.jitter = 0.06;
    g.net.conditions.loss = 0.3;
    g.net.conditions.duplicate = 0.1;

    for (i32 i = 1; i <= 50; ++i) {
        a.rpc->Call(lw, "NetTestWeapon", "Fire", i);
        g.Tick();
    }
    g.Run(600);
    NetTestWeapon* sw = g.Server(w);
    AETHER_CHECK(sw->order.size() == 50);
    bool ordered = true;
    for (i32 i = 0; i < 50 && ordered; ++i) ordered = sw->order[i] == i + 1;
    AETHER_CHECK(ordered && sw->fired == 50 * 51 / 2);
}

AETHER_TEST(NetRpc_UnreliableCallsMayBeLost) {
    Game g(3);
    Game::Client& a = g.AddClient();
    g.Run(60);
    const Entity w = g.Spawn(&a);
    g.Run(10);
    const Entity lw = g.Local(a, w);
    g.net.conditions.loss = 0.5;
    for (int i = 0; i < 100; ++i) {
        a.rpc->Call(lw, "NetTestWeapon", "Ping");
        g.Tick();
    }
    g.Run(20);
    AETHER_CHECK(g.Server(w)->pings > 20 && g.Server(w)->pings < 90); // some lost, none resent
}

AETHER_TEST(NetRpc_ServerRateLimitsAPeer) {
    Game g;
    Game::Client& a = g.AddClient();
    g.Run(60);
    const Entity w = g.Spawn(&a);
    g.Run(10);
    g.rpc.max_calls_per_second = 5;
    const Entity lw = g.Local(a, w);
    for (int i = 0; i < 20; ++i) a.rpc->Call(lw, "NetTestWeapon", "Fire", 1);
    g.Run(5); // all within a second
    AETHER_CHECK(g.Server(w)->fired == 5);
    AETHER_CHECK(CountOf(g.server_results, RpcResult::RateLimited) == 15);

    // A later window starts fresh.
    g.Run(60);
    a.rpc->Call(lw, "NetTestWeapon", "Fire", 1);
    g.Run(5);
    AETHER_CHECK(g.Server(w)->fired == 6);
}

AETHER_TEST(NetRpc_ForgedAndMalformedMessagesAreRejected) {
    Game g;
    Game::Client& a = g.AddClient();
    g.Run(60);
    const Entity w = g.Spawn(&a);
    g.Run(10);
    const u32 id = g.IdOf(w);
    const u32 comp = ComponentHash("NetTestWeapon");
    auto send = [&](const ByteWriter& m) {
        a.endpoint->Send(g.server_link.LocalAddress(), Channel::ReliableOrdered, m.Data());
        g.Run(5);
    };
    auto header = [&](u32 net_id, u32 component, const char* fn) {
        ByteWriter m;
        m.U8(kRpcMessage);
        m.Varint(net_id);
        m.U32(component);
        m.U32(ComponentHash(fn));
        return m;
    };

    // A Client-only function forged as if the client could call it.
    ByteWriter forged = header(id, comp, "Notify");
    forged.String("pwn");
    forged.Varint(0);
    send(forged);
    // A function that isn't an RPC at all.
    send(header(id, comp, "Secret"));
    // Unknown function, component and entity.
    send(header(id, comp, "DoesNotExist"));
    send(header(id, 0x12345678, "Fire"));
    ByteWriter nobody = header(9999, comp, "Fire");
    nobody.Varint(0);
    send(nobody);
    // Arguments missing, and extra bytes after them.
    send(header(id, comp, "Fire"));
    ByteWriter extra = header(id, comp, "Fire");
    extra.Varint(2); // zigzag 1
    extra.U8(0xFF);
    send(extra);
    // Truncated header.
    ByteWriter cut;
    cut.U8(kRpcMessage);
    cut.Varint(id);
    send(cut);

    NetTestWeapon* sw = g.Server(w);
    AETHER_CHECK(sw->notice.empty() && sw->private_calls == 0 && sw->fired == 0);
    AETHER_CHECK(g.server_results.size() == 8);
    AETHER_CHECK(CountOf(g.server_results, RpcResult::Ok) == 0);
    AETHER_CHECK(CountOf(g.server_results, RpcResult::NotAllowed) == 1);
    AETHER_CHECK(CountOf(g.server_results, RpcResult::UnknownEntity) == 1);
    AETHER_CHECK(CountOf(g.server_results, RpcResult::Malformed) >= 2);
    AETHER_CHECK(g.rpc.Stats().rejected == 8 && g.rpc.Stats().executed == 0);
    // The connection is fine afterwards.
    AETHER_CHECK(a.rpc->Call(g.Local(a, w), "NetTestWeapon", "Fire", 1));
    g.Run(10);
    AETHER_CHECK(g.Server(w)->fired == 1);
}

AETHER_TEST(NetRpc_ClientRejectsBadServerMessages) {
    Game g;
    Game::Client& a = g.AddClient();
    g.Run(60);
    const Entity w = g.Spawn(&a);
    g.Run(10);
    const u32 id = g.IdOf(w);
    auto header = [&](u32 net_id, const char* fn) {
        ByteWriter m;
        m.U8(kRpcMessage);
        m.Varint(net_id);
        m.U32(ComponentHash("NetTestWeapon"));
        m.U32(ComponentHash(fn));
        return m;
    };
    // A Server RPC sent to a client is the wrong direction; an unknown entity; truncated.
    AETHER_CHECK(a.rpc->Handle(header(id, "Fire").Data()) == RpcResult::NotAllowed);
    ByteWriter missing = header(777, "Shout");
    missing.String("x");
    AETHER_CHECK(a.rpc->Handle(missing.Data()) == RpcResult::UnknownEntity);
    AETHER_CHECK(a.rpc->Handle(header(id, "Shout").Data()) == RpcResult::Malformed); // argument missing
    const std::vector<u8> junk = {kRpcMessage, 1};
    AETHER_CHECK(a.rpc->Handle(junk) == RpcResult::Malformed);
    const std::vector<u8> wrong = {0x01, 2, 3};
    AETHER_CHECK(a.rpc->Handle(wrong) == RpcResult::Malformed);
    AETHER_CHECK(g.On(a, w)->shouts == 0 && a.rpc->Stats().rejected == 5);
}
