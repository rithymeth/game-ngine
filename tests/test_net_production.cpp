#include "aether/net/rpc.h"
#include "aether/net/session.h"
#include "aether/scene/components.h"
#include "test_framework.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <random>
#include <string>
#include <vector>

// M8.9 (docs/design/M8_PRODUCTION_PLAN.md): the automatable multiplayer
// production checks. A server (SessionHost + ReplicationServer + RpcRouter)
// and four clients (SessionClient + ReplicationClient + RpcRouter) play a
// small scenario on the seeded LoopbackNetwork - moving NPCs that churn
// (despawn/respawn), one owned pawn per player, and a server-authoritative
// score each owner bumps by RPC. Everything runs on simulated time, so each
// measurement is deterministic and asserted against the thresholds recorded
// in docs/design/M8_PRODUCTION_PLAN.md (M8.9 "Recorded thresholds").

using namespace aether;
using namespace aether::net;

#define CHECK(...) AETHER_CHECK((__VA_ARGS__))

namespace prod_test {
struct SoakPawn {
    i32 score = 0;
    void AddScore(i32 n) { score += n; }
};
} // namespace prod_test

AETHER_REFLECT(prod_test::SoakPawn, 1,
    AETHER_FIELD(score, Field_Replicated),
    AETHER_METHOD(AddScore, Fn_Server, {"n"})
)

namespace {

using prod_test::SoakPawn;
constexpr f64 kStep = 1.0 / 60.0;
constexpr u16 kServerPort = 7777;
constexpr u32 kBuild = 89;
constexpr int kClients = 4;
constexpr int kNpcs = 12;

// Recorded thresholds (keep in sync with the M8.9 table in the plan). NPCs move at 3 m/s, so a
// divergence budget is a staleness budget: 0.3 m = 100 ms, 1.5 m = 500 ms, 2.4 m = 800 ms.
struct Thresholds {
    f32 max_divergence;    // metres: worst client-vs-server position error while moving
    f64 converge_seconds;  // after motion stops: every client exact (1e-4) and scores equal
};
constexpr Thresholds kClean{0.3f, 0.5};
constexpr Thresholds kLoss5{1.5f, 1.0};
constexpr Thresholds kLoss20{2.4f, 2.0};
constexpr Thresholds kLatency{1.5f, 1.5};       // 100 ms +0..60 ms jitter, 2% loss
constexpr f64 kMaxBytesPerClientPerSecond = 16000; // server downstream, 12 NPCs + 4 pawns at 20 Hz
constexpr f64 kMaxResyncSeconds = 2.0;          // rejoin -> full, exact state
constexpr f64 kMaxDropDetectSeconds = 5.5;      // HostConfig::timeout (5 s) + a frame margin

f32 Score(World& w, Entity e) { return static_cast<f32>(w.GetComponent<SoakPawn>(e)->score); }

RemoteCallResult CallAddScore(World& world, Entity entity, i32 n) {
    const reflect::TypeInfo& type = reflect::Reflect<SoakPawn>();
    std::vector<reflect::Any> args = {i32{n}};
    RemoteCallResult result = RemoteCallResult::Refused;
    CallFunction(world, entity, type, *type.FindFunction("AddScore"), world.GetComponent<SoakPawn>(entity), args, nullptr,
                 &result);
    return result;
}

// Wraps a socket and keeps a copy of everything sent, for replay attacks.
class RecordingSocket final : public DatagramSocket {
public:
    explicit RecordingSocket(std::unique_ptr<DatagramSocket> inner) : inner_(std::move(inner)) {}
    bool Send(const Address& to, std::span<const u8> data) override {
        if (recording) sent.push_back({to, std::vector<u8>(data.begin(), data.end())});
        return inner_->Send(to, data);
    }
    bool Receive(Address& from, std::vector<u8>& data) override { return inner_->Receive(from, data); }
    Address LocalAddress() const override { return inner_->LocalAddress(); }
    bool recording = false;
    std::vector<std::pair<Address, std::vector<u8>>> sent;

private:
    std::unique_ptr<DatagramSocket> inner_;
};

struct Metrics {
    f32 max_divergence = 0;
    f64 converge_seconds = -1;
    f64 bytes_per_client_per_second = 0;
    u64 rpcs_issued = 0;
};

struct ProdSim {
    LoopbackNetwork net;
    World world;
    std::unique_ptr<DatagramSocket> socket;
    std::unique_ptr<NetHost> host;
    std::unique_ptr<SessionHost> session;
    std::unique_ptr<ReplicationServer> replication;
    std::unique_ptr<RpcRouter> rpc;
    std::vector<Entity> npcs;
    std::vector<std::pair<std::string, Entity>> pawns; // by player name
    std::vector<SessionEvent> session_events;
    bool moving = true;
    u32 churned = 0;

    struct Client {
        std::string name;
        u16 port = 0;
        World world;
        std::unique_ptr<RecordingSocket> socket;
        std::unique_ptr<NetHost> host;
        std::unique_ptr<SessionClient> session;
        std::unique_ptr<ReplicationClient> replication;
        std::unique_ptr<RpcRouter> rpc;
        u64 issued = 0;
    };
    std::vector<std::unique_ptr<Client>> clients;

    explicit ProdSim(u64 seed) : net(seed) {
        (void)GetComponentId<SoakPawn>();
        socket = net.Open(kServerPort);
        host = std::make_unique<NetHost>(*socket);
        host->Listen();
        SessionInfo info;
        info.name = "m8.9 soak";
        info.port = kServerPort;
        info.build = kBuild;
        info.max_players = kClients;
        session = std::make_unique<SessionHost>(*host, info, "letmein");
        replication = std::make_unique<ReplicationServer>(world, *host);
        rpc = std::make_unique<RpcRouter>(world, *host, *replication);
        for (int i = 0; i < kNpcs; ++i) npcs.push_back(SpawnNpc(i));
    }

    Entity SpawnNpc(int i) {
        NetIdentity ni;
        SetNetArchetype(ni, "npc");
        const f32 phase = static_cast<f32>(net.Now()) * 1.5f + static_cast<f32>(i);
        return world.CreateEntity(ni, Transform{Vec3{std::cos(phase) * 2.0f, std::sin(phase) * 2.0f, static_cast<f32>(i)}, Quaternion{}});
    }

    Client& AddClient(const std::string& name, u16 port = 0, const std::string& password = "letmein") {
        auto c = std::make_unique<Client>();
        c->name = name;
        c->socket = std::make_unique<RecordingSocket>(net.Open(port));
        c->port = c->socket->LocalAddress().port;
        c->host = std::make_unique<NetHost>(*c->socket);
        c->session = std::make_unique<SessionClient>(*c->host);
        c->session->Join(Address::Loopback(kServerPort), name, password, kBuild, net.Now());
        const PeerId server = c->session->Server();
        c->replication = std::make_unique<ReplicationClient>(c->world, *c->host, server);
        c->replication->OnSpawn("pawn", [](World& w, Entity e, const NetIdentity&) { w.AddComponent(e, SoakPawn{}); });
        c->rpc = std::make_unique<RpcRouter>(c->world, *c->host, *c->replication, server);
        clients.push_back(std::move(c));
        return *clients.back();
    }

    // A hard drop: the client process dies without saying goodbye.
    void Kill(usize index) { clients.erase(clients.begin() + static_cast<std::ptrdiff_t>(index)); }

    Entity PawnFor(const std::string& name) {
        for (auto& [n, e] : pawns)
            if (n == name) return e;
        return Entity{};
    }

    void OnSessionEvent(const SessionEvent& e) {
        session_events.push_back(e);
        if (e.type != SessionEventType::PlayerJoined) return;
        Entity pawn = PawnFor(e.name);
        if (pawn.IsNull()) {
            NetIdentity ni;
            SetNetArchetype(ni, "pawn");
            pawn = world.CreateEntity(ni, Transform{Vec3{0, 5, 0}, Quaternion{}}, SoakPawn{});
            pawns.push_back({e.name, pawn});
        }
        world.GetComponent<NetIdentity>(pawn)->owner = e.peer; // a rejoin takes its pawn back
    }

    void Simulate(int frame) {
        const f32 t = static_cast<f32>(net.Now());
        if (moving) {
            for (usize i = 0; i < npcs.size(); ++i) {
                const f32 phase = t * 1.5f + static_cast<f32>(i);
                world.GetComponent<Transform>(npcs[i])->position = Vec3{std::cos(phase) * 2.0f, std::sin(phase) * 2.0f, static_cast<f32>(i)};
            }
            for (usize i = 0; i < pawns.size(); ++i)
                world.GetComponent<Transform>(pawns[i].second)->position = Vec3{std::sin(t + static_cast<f32>(i)), 5, 0};
            // Churn: every second one NPC is replaced, so spawn/despawn paths run all soak long.
            if (frame % 60 == 59) {
                const usize slot = churned++ % npcs.size();
                world.DestroyEntity(npcs[slot]);
                npcs[slot] = SpawnNpc(static_cast<int>(slot));
            }
        }
    }

    void Step(int frames = 1, int first_frame = 0) {
        for (int i = 0; i < frames; ++i) {
            net.Advance(kStep);
            const f64 now = net.Now();
            Simulate(first_frame + i);
            host->Update(now);
            for (const NetEvent& e : host->TakeEvents())
                if (!session->HandleEvent(e, now) && !replication->HandleEvent(e)) rpc->HandleEvent(e);
            session->Update(now);
            for (const SessionEvent& e : session->TakeEvents()) OnSessionEvent(e);
            rpc->Tick(now);
            replication->Update(now);
            for (auto& c : clients) {
                c->host->Update(now);
                for (const NetEvent& e : c->host->TakeEvents())
                    if (!c->session->HandleEvent(e) && !c->replication->HandleEvent(e)) c->rpc->HandleEvent(e);
            }
        }
    }

    u32 NetId(Entity e) { return world.GetComponent<NetIdentity>(e)->net_id; }

    // Worst position error over the replicated entities a client has; -1 if `require_all` and one is missing
    // (a just-spawned NPC is allowed to be in flight while measuring divergence).
    f32 Divergence(Client& c, bool require_all = true) {
        f32 worst = 0;
        bool missing = false;
        auto check = [&](Entity s) {
            const Entity e = c.replication->FindEntity(NetId(s));
            if (e.IsNull()) return void(missing = true);
            worst = std::max(worst, (c.world.GetComponent<Transform>(e)->position - world.GetComponent<Transform>(s)->position).Length());
        };
        for (Entity s : npcs) check(s);
        for (auto& [n, s] : pawns) check(s);
        return missing && require_all ? -1.0f : worst;
    }

    bool ScoresMatch(Client& c) {
        for (auto& [n, s] : pawns) {
            const Entity e = c.replication->FindEntity(NetId(s));
            if (e.IsNull() || c.world.GetComponent<SoakPawn>(e) == nullptr) return false;
            if (c.world.GetComponent<SoakPawn>(e)->score != world.GetComponent<SoakPawn>(s)->score) return false;
        }
        return true;
    }

    bool Exact(Client& c) {
        const f32 d = Divergence(c);
        return d >= 0 && d < 1e-4f && ScoresMatch(c) && c.replication->EntityCount() == npcs.size() + pawns.size();
    }

    // Each owner asks for +1 every half second.
    void IssueRpcs(int frame) {
        if (frame % 30 != 0) return;
        for (auto& c : clients) {
            if (c->session->GetState() != SessionClient::State::Joined) continue;
            const Entity pawn = PawnFor(c->name);
            if (pawn.IsNull()) continue;
            const Entity mine = c->replication->FindEntity(NetId(pawn));
            if (mine.IsNull() || !c->world.GetComponent<NetIdentity>(mine)->locally_owned) continue;
            if (c->world.GetComponent<SoakPawn>(mine) == nullptr) continue;
            if (CallAddScore(c->world, mine, 1) == RemoteCallResult::Sent) ++c->issued;
        }
    }

    bool AllJoined() {
        return std::all_of(clients.begin(), clients.end(), [](auto& c) { return c->session->GetState() == SessionClient::State::Joined; }) &&
               pawns.size() == clients.size();
    }

    // Join everyone, run `seconds` of play, stop moving and time convergence.
    Metrics Run(f64 seconds, f64 warmup = 2.0) {
        Metrics m;
        for (int i = 0; i < kClients; ++i) AddClient("p" + std::to_string(i));
        int frame = 0;
        while (!AllJoined() && frame < 600) Step(1, frame++);
        CHECK(AllJoined());
        const f64 start = net.Now();
        const int frames = static_cast<int>(seconds / kStep);
        u64 start_bytes = 0;
        for (PeerId p : host->Peers()) start_bytes += host->Peer(p)->connection->Stats().bytes_sent;
        for (int i = 0; i < frames; ++i, ++frame) {
            IssueRpcs(frame);
            Step(1, frame);
            if (net.Now() - start < warmup) continue;
            for (auto& c : clients) m.max_divergence = std::max(m.max_divergence, Divergence(*c, false));
        }
        u64 bytes = 0;
        for (PeerId p : host->Peers()) bytes += host->Peer(p)->connection->Stats().bytes_sent;
        m.bytes_per_client_per_second = static_cast<f64>(bytes - start_bytes) / kClients / (net.Now() - start);
        moving = false;
        const f64 stopped = net.Now();
        while (net.Now() - stopped < 10.0) {
            Step(1, frame++);
            if (std::all_of(clients.begin(), clients.end(), [&](auto& c) { return Exact(*c); })) {
                m.converge_seconds = net.Now() - stopped;
                break;
            }
        }
        for (auto& c : clients) {
            m.rpcs_issued += c->issued;
            // Reliable RPCs ran exactly once each on the server, whatever the link did.
            CHECK(world.GetComponent<SoakPawn>(PawnFor(c->name))->score == static_cast<i32>(c->issued));
        }
        return m;
    }
};

void Report(const char* name, const Metrics& m) {
    std::printf("    [m8.9] %s: max_divergence=%.3f m converge=%.3f s bw=%.0f B/s/client rpcs=%llu\n", name, m.max_divergence,
                m.converge_seconds, m.bytes_per_client_per_second, static_cast<unsigned long long>(m.rpcs_issued));
}

void CheckAgainst(const Metrics& m, const Thresholds& t) {
    CHECK(m.max_divergence <= t.max_divergence);
    CHECK(m.converge_seconds >= 0 && m.converge_seconds <= t.converge_seconds);
    CHECK(m.bytes_per_client_per_second <= kMaxBytesPerClientPerSecond);
    CHECK(m.rpcs_issued > 0);
}

} // namespace

AETHER_TEST(NetProd_CleanLinkBaseline) {
    ProdSim sim(101);
    const Metrics m = sim.Run(30.0);
    Report("clean", m);
    CheckAgainst(m, kClean);
}

AETHER_TEST(NetProd_Soak) {
    // Five simulated minutes with churn and 2% loss + 50 ms: state stays bounded and in sync.
    ProdSim sim(202);
    sim.net.conditions = {0.05, 0.02, 0.02f, 0.0f};
    const Metrics m = sim.Run(300.0);
    Report("soak", m);
    CheckAgainst(m, kLatency);
    CHECK(sim.churned >= 290);
    // No leak growth: entity tables match the live set, nothing piles up in flight or unacked.
    CHECK(sim.world.EntityCount() == static_cast<usize>(kNpcs + kClients));
    for (auto& c : sim.clients) {
        CHECK(c->replication->EntityCount() == static_cast<usize>(kNpcs + kClients));
        CHECK(c->world.EntityCount() == static_cast<usize>(kNpcs + kClients));
        const auto& s = c->replication->GetStats();
        CHECK(s.spawned - s.despawned == kNpcs + kClients);
    }
    CHECK(sim.net.InFlight() < 64);
    for (PeerId p : sim.host->Peers()) CHECK(sim.host->Peer(p)->connection->Unacked() < 8);
    CHECK(sim.host->PeerCount() == static_cast<usize>(kClients));
}

AETHER_TEST(NetProd_PacketLoss5) {
    ProdSim sim(303);
    sim.net.conditions = {0.02, 0.0, 0.05f, 0.0f};
    const Metrics m = sim.Run(60.0);
    Report("loss5", m);
    CheckAgainst(m, kLoss5);
}

AETHER_TEST(NetProd_PacketLoss20) {
    ProdSim sim(404);
    sim.net.conditions = {0.02, 0.0, 0.20f, 0.05f};
    const Metrics m = sim.Run(60.0);
    Report("loss20", m);
    CheckAgainst(m, kLoss20);
}

AETHER_TEST(NetProd_LatencyAndJitter) {
    // 100 ms one way plus up to 60 ms jitter (100 +/- 30 around 130), reordering, 2% loss.
    ProdSim sim(505);
    sim.net.conditions = {0.10, 0.06, 0.02f, 0.01f};
    const Metrics m = sim.Run(60.0);
    Report("latency", m);
    CheckAgainst(m, kLatency);
}

AETHER_TEST(NetProd_ReconnectResyncs) {
    ProdSim sim(606);
    sim.net.conditions = {0.05, 0.02, 0.05f, 0.0f};
    for (int i = 0; i < kClients; ++i) sim.AddClient("p" + std::to_string(i));
    int frame = 0;
    while (!sim.AllJoined() && frame < 600) sim.Step(1, frame++);
    CHECK(sim.AllJoined());
    for (int i = 0; i < 600; ++i, ++frame) {
        sim.IssueRpcs(frame);
        sim.Step(1, frame);
    }
    // p2 dies with no goodbye: the server notices within its timeout, the others carry on.
    const u16 port = sim.clients[2]->port;
    sim.Kill(2);
    const f64 killed = sim.net.Now();
    f64 detected = -1;
    while (sim.net.Now() - killed < 10.0) {
        sim.IssueRpcs(frame);
        sim.Step(1, frame++);
        if (sim.session->Players().size() == static_cast<usize>(kClients - 1)) {
            detected = sim.net.Now() - killed;
            break;
        }
    }
    std::printf("    [m8.9] reconnect: drop detected after %.3f s\n", detected);
    CHECK(detected > 0 && detected <= kMaxDropDetectSeconds);
    // (Read after the drop: a reliable call already on the wire when it died still lands, once.)
    const i32 score_before = sim.world.GetComponent<SoakPawn>(sim.PawnFor("p2"))->score;
    CHECK(score_before > 0);
    CHECK(std::any_of(sim.session_events.begin(), sim.session_events.end(),
                      [](const SessionEvent& e) { return e.type == SessionEventType::PlayerLeft && e.name == "p2"; }));
    // It restarts on the same address and rejoins: a fresh world resyncs fully and owns its pawn again.
    auto& back = sim.AddClient("p2", port);
    const f64 rejoin = sim.net.Now();
    f64 resync = -1;
    while (sim.net.Now() - rejoin < 10.0) {
        sim.Step(1, frame++);
        if (back.session->GetState() == SessionClient::State::Joined && sim.Divergence(back) >= 0 && sim.ScoresMatch(back)) {
            resync = sim.net.Now() - rejoin;
            break;
        }
    }
    std::printf("    [m8.9] reconnect: rejoin -> full state in %.3f s\n", resync);
    CHECK(resync > 0 && resync <= kMaxResyncSeconds);
    CHECK(sim.pawns.size() == static_cast<usize>(kClients)); // the pawn was reused, not duplicated
    sim.Step(30, frame);
    frame += 30;
    const Entity mine = back.replication->FindEntity(sim.NetId(sim.PawnFor("p2")));
    CHECK(!mine.IsNull() && back.world.GetComponent<NetIdentity>(mine)->locally_owned);
    CHECK(CallAddScore(back.world, mine, 5) == RemoteCallResult::Sent);
    sim.Step(60, frame);
    std::printf("    [m8.9] reconnect: score before %d after %d\n", score_before, sim.world.GetComponent<SoakPawn>(sim.PawnFor("p2"))->score);
    CHECK(sim.world.GetComponent<SoakPawn>(sim.PawnFor("p2"))->score == score_before + 5);

    // An instant restart (same address, before any timeout) replaces the old peer rather than waiting.
    const u16 port0 = sim.clients[0]->port;
    sim.Kill(0);
    auto& quick = sim.AddClient("p0", port0);
    const f64 restarted = sim.net.Now();
    f64 quick_resync = -1;
    while (sim.net.Now() - restarted < 10.0) {
        sim.Step(1, frame++);
        if (quick.session->GetState() == SessionClient::State::Joined && sim.Divergence(quick) >= 0 && sim.ScoresMatch(quick)) {
            quick_resync = sim.net.Now() - restarted;
            break;
        }
    }
    std::printf("    [m8.9] reconnect: same-address restart -> full state in %.3f s\n", quick_resync);
    CHECK(quick_resync > 0 && quick_resync <= kMaxResyncSeconds);
    CHECK(sim.session->Players().size() == static_cast<usize>(kClients) && sim.host->PeerCount() == static_cast<usize>(kClients));
}

AETHER_TEST(NetProd_SecurityGarbageAndOversizedPackets) {
    ProdSim sim(707);
    for (int i = 0; i < kClients; ++i) sim.AddClient("p" + std::to_string(i));
    int frame = 0;
    while (!sim.AllJoined() && frame < 600) sim.Step(1, frame++);
    CHECK(sim.AllJoined());
    auto stranger = sim.net.Open();
    const Address server = Address::Loopback(kServerPort);
    std::mt19937_64 rng(77);
    auto random_bytes = [&](usize n) {
        std::vector<u8> b(n);
        for (u8& x : b) x = static_cast<u8>(rng());
        return b;
    };
    const u32 protocol = HostConfig{}.protocol_id;
    for (int round = 0; round < 200; ++round) {
        // Pure noise, empty, truncated headers, oversized datagrams, and valid headers with random bodies of every kind.
        stranger->Send(server, random_bytes(rng() % 1500));
        stranger->Send(server, {});
        stranger->Send(server, random_bytes(3));
        stranger->Send(server, random_bytes(64 * 1024));
        for (u8 kind = 0; kind < 8; ++kind) {
            std::vector<u8> d = {static_cast<u8>(protocol >> 24), static_cast<u8>(protocol >> 16), static_cast<u8>(protocol >> 8),
                                 static_cast<u8>(protocol), kind};
            for (u8 x : random_bytes(rng() % 64)) d.push_back(x);
            stranger->Send(server, d);
        }
        // The same from a joined client's own address (a spoofed or buggy peer): framed data with junk inside.
        auto& victim = *sim.clients[1];
        std::vector<u8> d = {static_cast<u8>(protocol >> 24), static_cast<u8>(protocol >> 16), static_cast<u8>(protocol >> 8),
                             static_cast<u8>(protocol), 4};
        for (u8 x : random_bytes(rng() % 1300)) d.push_back(x);
        victim.socket->Send(server, d);
        sim.IssueRpcs(frame);
        sim.Step(1, frame++);
    }
    // No crash, no stranger got a peer slot, and the game still converges with every RPC counted once.
    sim.moving = false;
    sim.Step(120, frame);
    CHECK(sim.host->PeerCount() == static_cast<usize>(kClients));
    for (auto& c : sim.clients) CHECK(c->session->GetState() == SessionClient::State::Joined);
    // The client fed junk may have lost reliable ordering state; the other three must be exact.
    for (usize i = 0; i < sim.clients.size(); ++i)
        if (i != 1) CHECK(sim.Exact(*sim.clients[i]));
    std::printf("    [m8.9] security: rpc malformed=%u unknown=%u rejected=%u\n", sim.rpc->Stats().malformed, sim.rpc->Stats().unknown,
                sim.rpc->Stats().rejected);
}

AETHER_TEST(NetProd_SecurityReplayAndAuthority) {
    ProdSim sim(808);
    for (int i = 0; i < kClients; ++i) sim.AddClient("p" + std::to_string(i));
    int frame = 0;
    while (!sim.AllJoined() && frame < 600) sim.Step(1, frame++);
    sim.Step(30, frame);
    frame += 30;
    auto& p0 = *sim.clients[0];
    const Entity pawn0 = sim.PawnFor("p0");
    const Entity mine = p0.replication->FindEntity(sim.NetId(pawn0));
    CHECK(!mine.IsNull());

    // Replay: record p0's datagrams around one RPC, then resend them all later. Exactly one call runs.
    p0.socket->recording = true;
    CHECK(CallAddScore(p0.world, mine, 3) == RemoteCallResult::Sent);
    sim.Step(30, frame);
    frame += 30;
    p0.socket->recording = false;
    CHECK(sim.world.GetComponent<SoakPawn>(pawn0)->score == 3);
    const auto captured = p0.socket->sent;
    CHECK(!captured.empty());
    for (int again = 0; again < 3; ++again) {
        for (const auto& [to, data] : captured) p0.socket->Send(to, data);
        sim.Step(30, frame);
        frame += 30;
    }
    // ...and replayed from another address (a stranger copying traffic): ignored, no peer created.
    auto sniffer = sim.net.Open();
    for (const auto& [to, data] : captured) sniffer->Send(to, data);
    sim.Step(30, frame);
    frame += 30;
    CHECK(sim.world.GetComponent<SoakPawn>(pawn0)->score == 3);
    CHECK(sim.host->PeerCount() == static_cast<usize>(kClients));

    // Authority: another player claiming p0's pawn is refused at home and rejected by the server.
    auto& p1 = *sim.clients[1];
    const Entity theirs = p1.replication->FindEntity(sim.NetId(pawn0));
    CHECK(CallAddScore(p1.world, theirs, 100) == RemoteCallResult::Refused);
    p1.world.GetComponent<NetIdentity>(theirs)->locally_owned = true; // a hacked client
    CHECK(CallAddScore(p1.world, theirs, 100) == RemoteCallResult::Sent);
    sim.Step(30, frame);
    frame += 30;
    CHECK(sim.world.GetComponent<SoakPawn>(pawn0)->score == 3 && sim.rpc->Stats().rejected >= 1);

    // Unauthenticated: wrong password is refused; a raw peer that skips the session gets no authority and is dropped.
    auto& intruder = sim.AddClient("mallory", 0, "guess");
    auto raw_socket = sim.net.Open();
    NetHost raw(*raw_socket);
    const PeerId raw_server = raw.Connect(Address::Loopback(kServerPort), sim.net.Now());
    World raw_world;
    ReplicationClient raw_repl(raw_world, raw, raw_server);
    raw_repl.OnSpawn("pawn", [](World& w, Entity e, const NetIdentity&) { w.AddComponent(e, SoakPawn{}); });
    RpcRouter raw_rpc(raw_world, raw, raw_repl, raw_server);
    const u32 rejected_before = sim.rpc->Stats().rejected;
    bool raw_called = false;
    for (int i = 0; i < 300; ++i) {
        sim.Step(1, frame++);
        raw.Update(sim.net.Now());
        for (const NetEvent& e : raw.TakeEvents())
            if (!raw_repl.HandleEvent(e)) raw_rpc.HandleEvent(e);
        if (!raw_called && raw.PeerCount() == 1) {
            // It never sees snapshots for entities it isn't relevant to... craft the call on a local stand-in.
            const Entity fake = raw_world.CreateEntity(NetIdentity{}, SoakPawn{});
            auto* ni = raw_world.GetComponent<NetIdentity>(fake);
            ni->net_id = sim.NetId(pawn0);
            ni->locally_owned = true;
            raw_called = CallAddScore(raw_world, fake, 1000) == RemoteCallResult::Sent;
        }
    }
    CHECK(raw_called);
    CHECK(intruder.session->GetState() == SessionClient::State::Failed && intruder.session->Result() == JoinResult::WrongPassword);
    CHECK(sim.world.GetComponent<SoakPawn>(pawn0)->score == 3);
    CHECK(sim.rpc->Stats().rejected > rejected_before);
    CHECK(raw.PeerCount() == 0); // dropped for never joining (SessionConfig::join_timeout)
    CHECK(sim.session->Players().size() == static_cast<usize>(kClients) && sim.pawns.size() == static_cast<usize>(kClients));
}
