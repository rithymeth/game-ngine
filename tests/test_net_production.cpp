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
        bytes_sent += data.size();
        if (recording) sent.push_back({to, std::vector<u8>(data.begin(), data.end())});
        return inner_->Send(to, data);
    }
    bool Receive(Address& from, std::vector<u8>& data) override { return inner_->Receive(from, data); }
    Address LocalAddress() const override { return inner_->LocalAddress(); }
    bool recording = false;
    u64 bytes_sent = 0; // on the wire, host header and MAC included
    std::vector<std::pair<Address, std::vector<u8>>> sent;

private:
    std::unique_ptr<DatagramSocket> inner_;
};

struct Metrics {
    f32 max_divergence = 0;
    f64 converge_seconds = -1;
    f64 bytes_per_client_per_second = 0;      // Connection payload (what the thresholds have always measured)
    f64 wire_bytes_per_client_per_second = 0; // plus the host header and the 8-byte MAC per datagram
    u64 rpcs_issued = 0;
};

struct ProdSim {
    LoopbackNetwork net;
    World world;
    std::unique_ptr<RecordingSocket> socket;
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
        socket = std::make_unique<RecordingSocket>(net.Open(kServerPort));
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

    Client& AddClient(const std::string& name, u16 port = 0, const std::string& password = "letmein", const std::string& token = {},
                      bool record = false) {
        auto c = std::make_unique<Client>();
        c->name = name;
        c->socket = std::make_unique<RecordingSocket>(net.Open(port));
        c->port = c->socket->LocalAddress().port;
        c->socket->recording = record;
        c->host = std::make_unique<NetHost>(*c->socket);
        c->session = std::make_unique<SessionClient>(*c->host);
        c->session->Join(Address::Loopback(kServerPort), name, password, kBuild, net.Now(), token);
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
        const u64 start_wire = socket->bytes_sent;
        for (int i = 0; i < frames; ++i, ++frame) {
            IssueRpcs(frame);
            Step(1, frame);
            if (net.Now() - start < warmup) continue;
            for (auto& c : clients) m.max_divergence = std::max(m.max_divergence, Divergence(*c, false));
        }
        u64 bytes = 0;
        for (PeerId p : host->Peers()) bytes += host->Peer(p)->connection->Stats().bytes_sent;
        m.bytes_per_client_per_second = static_cast<f64>(bytes - start_bytes) / kClients / (net.Now() - start);
        m.wire_bytes_per_client_per_second = static_cast<f64>(socket->bytes_sent - start_wire) / kClients / (net.Now() - start);
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
    std::printf("    [m8.9] %s: max_divergence=%.3f m converge=%.3f s bw=%.0f B/s/client (wire %.0f) rpcs=%llu\n", name,
                m.max_divergence, m.converge_seconds, m.bytes_per_client_per_second, m.wire_bytes_per_client_per_second,
                static_cast<unsigned long long>(m.rpcs_issued));
}

void CheckAgainst(const Metrics& m, const Thresholds& t) {
    CHECK(m.max_divergence <= t.max_divergence);
    CHECK(m.converge_seconds >= 0 && m.converge_seconds <= t.converge_seconds);
    CHECK(m.bytes_per_client_per_second <= kMaxBytesPerClientPerSecond);
    CHECK(m.wire_bytes_per_client_per_second <= kMaxBytesPerClientPerSecond); // the MAC fits in the same budget
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
        // Pure noise, empty, truncated headers (the protocol id is little-endian on the wire; before M8.9 this test
        // wrote it big-endian, so its "framed" junk was discarded as foreign and never reached the data path), oversized datagrams, and valid headers with random bodies of every kind.
        stranger->Send(server, random_bytes(rng() % 1500));
        stranger->Send(server, {});
        stranger->Send(server, random_bytes(3));
        stranger->Send(server, random_bytes(64 * 1024));
        for (u8 kind = 0; kind < 8; ++kind) {
            std::vector<u8> d = {static_cast<u8>(protocol), static_cast<u8>(protocol >> 8), static_cast<u8>(protocol >> 16),
                                 static_cast<u8>(protocol >> 24), kind};
            for (u8 x : random_bytes(rng() % 64)) d.push_back(x);
            stranger->Send(server, d);
        }
        // The same from a joined client's own address (a spoofed or buggy peer): framed data with junk inside.
        auto& victim = *sim.clients[1];
        std::vector<u8> d = {static_cast<u8>(protocol), static_cast<u8>(protocol >> 8), static_cast<u8>(protocol >> 16),
                             static_cast<u8>(protocol >> 24), 4};
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
    // M8.9: the junk "from" the joined client's address fails the session MAC, so even that client stays exact
    // (before packet authentication it could lose reliable ordering state and was exempt).
    for (auto& c : sim.clients) CHECK(sim.Exact(*c));
    CHECK(sim.host->SecurityStats().bad_macs >= 200);
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

// --- M8.9 security/auth hardening ------------------------------------------------------------------

namespace {
std::vector<u8> Framed(u8 kind, std::span<const u8> body = {}) {
    const u32 protocol = HostConfig{}.protocol_id;
    std::vector<u8> d = {static_cast<u8>(protocol), static_cast<u8>(protocol >> 8), static_cast<u8>(protocol >> 16),
                         static_cast<u8>(protocol >> 24), kind};
    d.insert(d.end(), body.begin(), body.end());
    return d;
}
} // namespace

AETHER_TEST(NetProd_SipHashTestVectors) {
    // From the SipHash reference (github.com/veorq/SipHash, vectors.h): key 00..0f, message 00..(n-1).
    SipKey key{};
    for (u8 i = 0; i < 16; ++i) key[i] = i;
    std::vector<u8> msg;
    for (u8 i = 0; i < 64; ++i) msg.push_back(i);
    CHECK(SipHash24(key, std::span<const u8>(msg).first(0)) == 0x726fdb47dd0e0e31ull);
    CHECK(SipHash24(key, std::span<const u8>(msg).first(1)) == 0x74f839c593dc67fdull);
    CHECK(SipHash24(key, std::span<const u8>(msg).first(8)) == 0x93f5f5799a932462ull);
    CHECK(SipHash24(key, std::span<const u8>(msg).first(15)) == 0xa129ca6149be45e5ull); // the paper's example
    CHECK(SipHash24(key, std::span<const u8>(msg).first(63)) == 0x958a324ceb064572ull);
    CHECK(TagEqual(1, 1) && !TagEqual(1, 2));
}

AETHER_TEST(NetProd_SecuritySpoofedAndForgedFramesAreDropped) {
    ProdSim sim(909);
    for (int i = 0; i < kClients; ++i) sim.AddClient("p" + std::to_string(i));
    int frame = 0;
    while (!sim.AllJoined() && frame < 600) sim.Step(1, frame++);
    CHECK(sim.AllJoined());
    sim.Step(30, frame), frame += 30;
    auto& p0 = *sim.clients[0];
    const Address server = Address::Loopback(kServerPort);
    const Entity pawn0 = sim.PawnFor("p0");
    const Entity mine = p0.replication->FindEntity(sim.NetId(pawn0));
    // A real RPC datagram from p0, captured.
    p0.socket->recording = true;
    CHECK(CallAddScore(p0.world, mine, 5) == RemoteCallResult::Sent);
    p0.host->Update(sim.net.Now()); // flushes it without the server seeing it yet
    p0.socket->recording = false;
    sim.Step(30, frame), frame += 30;
    CHECK(sim.world.GetComponent<SoakPawn>(pawn0)->score == 5);
    std::vector<u8> real;
    for (const auto& [to, d] : p0.socket->sent)
        if (d.size() > 5 + 8 + 8 && d[4] == 4) real = d;
    CHECK(!real.empty());
    const u64 bad_before = sim.host->SecurityStats().bad_macs;
    // Forged: the captured frame with one payload byte (and then the tag) flipped, from p0's own address.
    std::vector<u8> forged = real;
    forged[5 + 2] ^= 0x01; // the ack field
    p0.socket->Send(server, forged);
    std::vector<u8> retagged = real;
    retagged.back() ^= 0x80;
    p0.socket->Send(server, retagged);
    // A different message, correctly framed, with a guessed tag, "from" p0: a spoofed injection.
    std::vector<u8> injected = Framed(4, std::vector<u8>(64, 0x5A));
    p0.socket->Send(server, injected);
    // A spoofed goodbye for p0: the session must survive it.
    std::vector<u8> bye = Framed(5, std::vector<u8>(8, 0));
    p0.socket->Send(server, bye);
    sim.Step(30, frame), frame += 30;
    CHECK(sim.host->SecurityStats().bad_macs >= bad_before + 4);
    CHECK(sim.world.GetComponent<SoakPawn>(pawn0)->score == 5);
    CHECK(p0.session->GetState() == SessionClient::State::Joined && sim.host->PeerCount() == static_cast<usize>(kClients));
    // The server's frames to a client are authenticated too: a spoofed "server" goodbye is ignored.
    const u64 client_bad = p0.host->SecurityStats().bad_macs;
    sim.socket->Send(Address::Loopback(p0.port), bye); // from the server's real address, no valid tag
    sim.Step(10, frame), frame += 10;
    CHECK(p0.host->SecurityStats().bad_macs == client_bad + 1);
    CHECK(p0.session->GetState() == SessionClient::State::Joined);
    sim.moving = false;
    sim.Step(60, frame);
    for (auto& c : sim.clients) CHECK(sim.Exact(*c));
    std::printf("    [m8.9] security: forged/spoofed frames dropped by MAC: server %llu, client %llu\n",
                static_cast<unsigned long long>(sim.host->SecurityStats().bad_macs - bad_before),
                static_cast<unsigned long long>(p0.host->SecurityStats().bad_macs));
}

AETHER_TEST(NetProd_SecurityHandshakeIsStatelessAndUnreplayable) {
    ProdSim sim(1001);
    auto& c = sim.AddClient("p0", 0, "letmein", {}, /*record=*/true);
    int frame = 0;
    while (!sim.AllJoined() && frame < 600) sim.Step(1, frame++);
    CHECK(sim.AllJoined());
    // Anti-amplification: every server reply before verification is no bigger than the request.
    std::vector<u8> request, response;
    for (const auto& [to, d] : c.socket->sent) {
        if (d.size() >= 5 && d[4] == 1) request = d;
        if (d.size() >= 5 && d[4] == 7) response = d;
    }
    CHECK(request.size() == NetHost::kConnectRequestSize && response.size() == NetHost::kConnectRequestSize);
    CHECK(sim.host->SecurityStats().challenges_sent >= 1);

    // A stranger (or a spoofer that can't see replies) sends requests: it gets cookies, the server keeps no peer.
    auto stranger = sim.net.Open();
    const Address server = Address::Loopback(kServerPort);
    for (int i = 0; i < 4; ++i) stranger->Send(server, request);
    // Short requests (an amplification attempt) aren't answered at all.
    stranger->Send(server, Framed(1, std::vector<u8>(8, 1)));
    sim.Step(5, frame), frame += 5;
    usize reply_bytes = 0, replies = 0;
    Address from;
    std::vector<u8> d;
    while (stranger->Receive(from, d)) reply_bytes = std::max(reply_bytes, d.size()), ++replies;
    CHECK(replies == 4 && reply_bytes <= request.size());
    CHECK(sim.host->PeerCount() == 1);
    // The real client's cookie response, copied to another address: bound to the source address, refused.
    const u64 bad_cookies = sim.host->SecurityStats().bad_cookies;
    stranger->Send(server, response);
    sim.Step(5, frame), frame += 5;
    CHECK(sim.host->SecurityStats().bad_cookies == bad_cookies + 1 && sim.host->PeerCount() == 1);
    // Forged cookie from the client's address.
    std::vector<u8> forged = response;
    forged[5 + 16] ^= 1;
    c.socket->Send(server, forged);
    sim.Step(5, frame), frame += 5;
    CHECK(sim.host->SecurityStats().bad_cookies == bad_cookies + 2);
    // While the peer lives, a duplicate response just re-sends the Accept (no new peer, no replacement).
    const PeerId peer = sim.host->Peers()[0];
    c.socket->Send(server, response);
    sim.Step(5, frame), frame += 5;
    CHECK(sim.host->Peers().size() == 1 && sim.host->Peers()[0] == peer && c.session->GetState() == SessionClient::State::Joined);
    // The client leaves; replaying its handshake from its own address (within the cookie lifetime) is refused.
    c.session->Leave(sim.net.Now());
    sim.Step(10, frame), frame += 10;
    CHECK(sim.host->PeerCount() == 0);
    const u64 replays = sim.host->SecurityStats().replayed_handshakes;
    c.socket->Send(server, response);
    sim.Step(10, frame), frame += 10;
    CHECK(sim.host->SecurityStats().replayed_handshakes == replays + 1 && sim.host->PeerCount() == 0);
    // ...and once the cookie has expired it is refused as stale.
    sim.Step(static_cast<int>(HostConfig{}.cookie_lifetime / kStep) + 30, frame);
    c.socket->Send(server, response);
    sim.Step(5, frame);
    CHECK(sim.host->SecurityStats().bad_cookies == bad_cookies + 3 && sim.host->PeerCount() == 0);
    std::printf("    [m8.9] handshake: request %zu B, largest pre-verification reply %zu B; replays refused %llu\n", request.size(),
                reply_bytes, static_cast<unsigned long long>(sim.host->SecurityStats().replayed_handshakes));
}

AETHER_TEST(NetProd_SecurityJoinTokens) {
    const std::string secret = "a-very-secret-issuer-key-0123456789";
    const u64 now = 1'800'000'000;
    // The token format on its own.
    const std::string good = IssueJoinToken(secret, {"acct-42", now + 600, kBuild});
    JoinToken claims;
    CHECK(VerifyJoinToken(secret, good, kBuild, now, &claims) == TokenCheck::Ok && claims.player == "acct-42");
    CHECK(VerifyJoinToken(secret, good, kBuild, now + 600) == TokenCheck::Expired);
    CHECK(VerifyJoinToken(secret, good, kBuild + 1, now) == TokenCheck::WrongBuild);
    CHECK(VerifyJoinToken("another-issuer-secret-xxxxxxxx", good, kBuild, now) == TokenCheck::BadSignature);
    std::string tampered = good;
    tampered[good.find('.') + 3] = tampered[good.find('.') + 3] == '0' ? '1' : '0'; // a payload digit
    CHECK(VerifyJoinToken(secret, tampered, kBuild, now) == TokenCheck::BadSignature);
    CHECK(VerifyJoinToken(secret, "aet1.zz.00", kBuild, now) == TokenCheck::Malformed);
    CHECK(VerifyJoinToken(secret, "", kBuild, now) == TokenCheck::Malformed);

    // Through the session: tokens required alongside the password.
    ProdSim sim(1101);
    sim.session->SetJoinTokens({secret, true, [&] { return now; }});
    sim.AddClient("ok", 0, "letmein", good);
    sim.AddClient("expired", 0, "letmein", IssueJoinToken(secret, {"acct-1", now - 1, kBuild}));
    sim.AddClient("forged", 0, "letmein", IssueJoinToken("not-the-issuer-secret-at-all", {"acct-2", now + 600, kBuild}));
    sim.AddClient("oldbuild", 0, "letmein", IssueJoinToken(secret, {"acct-3", now + 600, kBuild - 1}));
    sim.AddClient("none", 0, "letmein");
    sim.AddClient("nopass", 0, "guess", IssueJoinToken(secret, {"acct-4", now + 600, kBuild}));
    for (int frame = 0; frame < 300; ++frame) sim.Step(1, frame);
    auto result = [&](usize i) { return sim.clients[i]->session->Result(); };
    CHECK(sim.clients[0]->session->GetState() == SessionClient::State::Joined);
    CHECK(sim.session->Players().size() == 1 && sim.session->Players()[0].account == "acct-42");
    CHECK(result(1) == JoinResult::TokenExpired);
    CHECK(result(2) == JoinResult::BadToken);
    CHECK(result(3) == JoinResult::BadToken);
    CHECK(result(4) == JoinResult::BadToken);
    CHECK(result(5) == JoinResult::WrongPassword); // a token doesn't waive a configured password
    for (usize i = 1; i < sim.clients.size(); ++i) CHECK(sim.clients[i]->session->GetState() == SessionClient::State::Failed);
}

AETHER_TEST(NetProd_SecurityRateLimitsPerAddress) {
    ProdSim sim(1201);
    for (int i = 0; i < kClients; ++i) sim.AddClient("p" + std::to_string(i));
    int frame = 0;
    while (!sim.AllJoined() && frame < 600) sim.Step(1, frame++);
    CHECK(sim.AllJoined());
    const Address server = Address::Loopback(kServerPort);
    const HostConfig cfg;
    // A handshake flood from one address: 2000 requests a frame for 1 s. Cookies are capped by the bucket.
    auto flooder = sim.net.Open();
    std::vector<u8> req = Framed(1, std::vector<u8>(NetHost::kConnectRequestSize - 5, 0));
    const u64 challenges = sim.host->SecurityStats().challenges_sent, limited = sim.host->SecurityStats().rate_limited;
    u64 sent = 0;
    for (int i = 0; i < 60; ++i) {
        for (int j = 0; j < 2000; ++j) flooder->Send(server, req), ++sent;
        sim.IssueRpcs(frame);
        sim.Step(1, frame++);
    }
    const u64 answered = sim.host->SecurityStats().challenges_sent - challenges;
    CHECK(answered <= static_cast<u64>(cfg.handshake_burst + cfg.handshakes_per_second * 1.1) + 1);
    CHECK(sim.host->SecurityStats().rate_limited - limited >= sent - answered);
    // A data flood "from" a joined client (or the client itself misbehaving): capped at the packet budget,
    // the other players unaffected.
    auto& victim = *sim.clients[2];
    const std::vector<u8> junk = Framed(4, std::vector<u8>(100, 7));
    const u64 limited2 = sim.host->SecurityStats().rate_limited;
    for (int i = 0; i < 60; ++i) {
        for (int j = 0; j < 500; ++j) victim.socket->Send(server, junk);
        sim.IssueRpcs(frame);
        sim.Step(1, frame++);
    }
    CHECK(sim.host->SecurityStats().rate_limited - limited2 >= 30000 - static_cast<u64>(cfg.packet_burst + cfg.packets_per_second * 1.1));
    sim.moving = false;
    sim.Step(120, frame);
    for (usize i = 0; i < sim.clients.size(); ++i)
        if (i != 2) CHECK(sim.Exact(*sim.clients[i]));
    std::printf("    [m8.9] rate limits: %llu handshake datagrams -> %llu cookies; total dropped over budget %llu\n",
                static_cast<unsigned long long>(sent), static_cast<unsigned long long>(answered),
                static_cast<unsigned long long>(sim.host->SecurityStats().rate_limited));

    // The address table is bounded: 64 sources against a 16-entry table.
    LoopbackNetwork net(5);
    auto server_socket = net.Open(9000);
    HostConfig small;
    small.max_tracked_addresses = 16;
    NetHost host(*server_socket, small);
    host.Listen();
    std::vector<std::unique_ptr<DatagramSocket>> sources;
    for (int i = 0; i < 64; ++i) sources.push_back(net.Open());
    for (int round = 0; round < 3; ++round) {
        for (auto& s : sources) s->Send(Address::Loopback(9000), req);
        net.Advance(0.05);
        host.Update(net.Now());
    }
    CHECK(host.TrackedAddresses() <= 16 && host.PeerCount() == 0);
}


AETHER_TEST(NetProd_GhostShift_OnlyOwnerCanChangeSharedState) {
    // Ghost Shift engine prerequisite: an authenticated client may mutate only its own
    // replicated pawn. Other clients cannot forge ownership to change its state.
    ProdSim sim(1207);
    for (int i = 0; i < kClients; ++i) sim.AddClient("p" + std::to_string(i));
    int frame = 0;
    while (!sim.AllJoined() && frame < 600) sim.Step(1, frame++);
    CHECK(sim.AllJoined());
    sim.Step(60, frame);
    frame += 60;

    const Entity target = sim.PawnFor("p0");
    const u32 net_id = sim.NetId(target);
    const Entity owner_entity = sim.clients[0]->replication->FindEntity(net_id);
    const Entity attacker_entity = sim.clients[1]->replication->FindEntity(net_id);
    CHECK(!owner_entity.IsNull() && !attacker_entity.IsNull());
    CHECK(CallAddScore(sim.clients[0]->world, owner_entity, 7) == RemoteCallResult::Sent);
    CHECK(CallAddScore(sim.clients[1]->world, attacker_entity, 99) == RemoteCallResult::Refused);
    // A compromised client may lie about its local ownership; the host must still reject.
    sim.clients[1]->world.GetComponent<NetIdentity>(attacker_entity)->locally_owned = true;
    CHECK(CallAddScore(sim.clients[1]->world, attacker_entity, 99) == RemoteCallResult::Sent);
    sim.Step(90, frame);
    frame += 90;
    CHECK(sim.world.GetComponent<SoakPawn>(target)->score == 7);
    CHECK(sim.rpc->Stats().rejected >= 1);
    // The authoritative result must be visible to every participant.
    sim.moving = false;
    sim.Step(120, frame);
    for (auto& c : sim.clients) CHECK(sim.ScoresMatch(*c));
}


AETHER_TEST(NetProd_GhostShift_RejectsFifthPlayerWithoutBreakingSession) {
    // A 1-4 player game must not silently accept a fifth player. Existing
    // participants must retain authority and replication after a full-lobby join.
    ProdSim sim(1211);
    for (int i = 0; i < kClients; ++i) sim.AddClient("p" + std::to_string(i));
    int frame = 0;
    while (!sim.AllJoined() && frame < 600) sim.Step(1, frame++);
    CHECK(sim.AllJoined());
    sim.Step(60, frame);
    frame += 60;
    const Entity pawn = sim.PawnFor("p0");
    const Entity owned = sim.clients[0]->replication->FindEntity(sim.NetId(pawn));
    CHECK(!owned.IsNull());
    CHECK(CallAddScore(sim.clients[0]->world, owned, 4) == RemoteCallResult::Sent);
    sim.Step(60, frame);
    frame += 60;
    CHECK(sim.world.GetComponent<SoakPawn>(pawn)->score == 4);

    auto& fifth = sim.AddClient("p4");
    for (int i = 0; i < 300; ++i) sim.Step(1, frame++);
    CHECK(fifth.session->GetState() != SessionClient::State::Joined);
    CHECK(sim.session->Players().size() == static_cast<usize>(kClients));
    CHECK(sim.pawns.size() == static_cast<usize>(kClients));
    CHECK(sim.PawnFor("p4").IsNull());
    for (int i = 0; i < kClients; ++i)
        CHECK(sim.clients[i]->session->GetState() == SessionClient::State::Joined);

    sim.moving = false;
    sim.Step(120, frame);
    for (int i = 0; i < kClients; ++i) CHECK(sim.Exact(*sim.clients[i]));
}


AETHER_TEST(NetProd_GhostShift_ReconnectPreservesSharedStateAndOwnership) {
    // A disconnect must not erase the authoritative shared state or give a
    // returning player a duplicate pawn. Other players keep participating.
    ProdSim sim(1217);
    sim.net.conditions = {0.05, 0.02, 0.05f, 0.0f};
    for (int i = 0; i < kClients; ++i) sim.AddClient("p" + std::to_string(i));
    int frame = 0;
    while (!sim.AllJoined() && frame < 600) sim.Step(1, frame++);
    CHECK(sim.AllJoined());
    sim.Step(60, frame);
    frame += 60;

    const Entity p0 = sim.PawnFor("p0");
    const Entity p1 = sim.PawnFor("p1");
    const u32 original_id = sim.NetId(p0);
    const Entity original_client_pawn = sim.clients[0]->replication->FindEntity(original_id);
    CHECK(!original_client_pawn.IsNull());
    CHECK(CallAddScore(sim.clients[0]->world, original_client_pawn, 11) == RemoteCallResult::Sent);
    sim.Step(90, frame);
    frame += 90;
    CHECK(sim.world.GetComponent<SoakPawn>(p0)->score == 11);

    const u16 port = sim.clients[0]->port;
    sim.Kill(0);
    const f64 dropped_at = sim.net.Now();
    while (sim.session->Players().size() == static_cast<usize>(kClients) &&
           sim.net.Now() - dropped_at < 10.0) sim.Step(1, frame++);
    CHECK(sim.session->Players().size() == static_cast<usize>(kClients - 1));
    // The remaining player can still change its own state while p0 is away.
    const Entity other = sim.clients[0]->replication->FindEntity(sim.NetId(p1));
    CHECK(!other.IsNull());
    CHECK(CallAddScore(sim.clients[0]->world, other, 3) == RemoteCallResult::Sent);
    sim.Step(90, frame);
    frame += 90;
    CHECK(sim.world.GetComponent<SoakPawn>(p1)->score == 3);

    auto& back = sim.AddClient("p0", port);
    const f64 rejoined_at = sim.net.Now();
    bool restored = false;
    while (sim.net.Now() - rejoined_at < 10.0) {
        sim.Step(1, frame++);
        if (back.session->GetState() == SessionClient::State::Joined &&
            sim.Divergence(back) >= 0 && sim.ScoresMatch(back)) {
            restored = true;
            break;
        }
    }
    CHECK(restored);
    CHECK(sim.NetId(sim.PawnFor("p0")) == original_id);
    CHECK(sim.pawns.size() == static_cast<usize>(kClients));
    CHECK(sim.world.GetComponent<SoakPawn>(p0)->score == 11);
    CHECK(sim.world.GetComponent<SoakPawn>(p1)->score == 3);
    const Entity returned = back.replication->FindEntity(original_id);
    CHECK(!returned.IsNull());
    CHECK(back.world.GetComponent<NetIdentity>(returned)->locally_owned);
    CHECK(CallAddScore(back.world, returned, 2) == RemoteCallResult::Sent);
    sim.Step(90, frame);
    frame += 90;
    CHECK(sim.world.GetComponent<SoakPawn>(p0)->score == 13);
    sim.moving = false;
    sim.Step(120, frame);
    for (auto& c : sim.clients) CHECK(sim.Exact(*c));
}


AETHER_TEST(NetProd_GhostShift_SharedObjectTransformConvergesAfterLoss) {
    // Host-owned movable objects (doors/furniture) use authoritative Transform
    // replication. A client-local edit must not persist after the next snapshot.
    // This is transform sync coverage, not a full Jolt rigid-body prediction test.
    ProdSim sim(1223);
    sim.net.conditions = {0.08, 0.03, 0.10f, 0.02f};
    for (int i = 0; i < kClients; ++i) sim.AddClient("p" + std::to_string(i));
    int frame = 0;
    while (!sim.AllJoined() && frame < 600) sim.Step(1, frame++);
    CHECK(sim.AllJoined());
    sim.Step(120, frame);
    frame += 120;

    const Entity prop = sim.npcs[0];
    const u32 prop_id = sim.NetId(prop);
    // Stop the test harness from moving/churning NPCs. The host alone moves
    // this shared prop, then clients must converge despite lost snapshots.
    sim.moving = false;
    const Vec3 final_position{12.0f, -3.0f, 7.0f};
    sim.world.GetComponent<Transform>(prop)->position = final_position;
    for (auto& c : sim.clients) {
        const Entity remote = c->replication->FindEntity(prop_id);
        CHECK(!remote.IsNull());
        c->world.GetComponent<Transform>(remote)->position = Vec3{-90.0f, 50.0f, 0.0f};
    }
    bool converged = false;
    for (int i = 0; i < 300; ++i) {
        sim.Step(1, frame++);
        converged = std::all_of(sim.clients.begin(), sim.clients.end(), [&](const auto& c) {
            const Entity remote = c->replication->FindEntity(prop_id);
            return !remote.IsNull() &&
                   (c->world.template GetComponent<Transform>(remote)->position - final_position).Length() < 1e-4f;
        });
        if (converged) break;
    }
    CHECK(converged);
    sim.Step(120, frame);
    for (auto& c : sim.clients) CHECK(sim.Exact(*c));
}


AETHER_TEST(NetProd_GhostShift_LateJoinReceivesCurrentSharedObjectState) {
    // A player joining an in-progress contract must receive the host's current
    // prop positions, not the initial spawn transform. The other players must
    // retain their authoritative state while the new peer receives its baseline.
    ProdSim sim(1231);
    sim.net.conditions = {0.05, 0.02, 0.05f, 0.0f};
    for (int i = 0; i < kClients - 1; ++i) sim.AddClient("p" + std::to_string(i));
    int frame = 0;
    while (sim.session->Players().size() != static_cast<usize>(kClients - 1) && frame < 600)
        sim.Step(1, frame++);
    CHECK(sim.session->Players().size() == static_cast<usize>(kClients - 1));
    sim.Step(120, frame);
    frame += 120;
    sim.moving = false;

    const Entity prop = sim.npcs[0];
    const Vec3 moved{24.0f, -8.0f, 2.0f};
    sim.world.GetComponent<Transform>(prop)->position = moved;
    sim.Step(120, frame);
    frame += 120;
    const u32 prop_id = sim.NetId(prop);
    for (const auto& c : sim.clients) {
        const Entity remote = c->replication->FindEntity(prop_id);
        CHECK(!remote.IsNull());
        CHECK((c->world.GetComponent<Transform>(remote)->position - moved).Length() < 1e-4f);
    }

    auto& late = sim.AddClient("p3");
    bool synchronized = false;
    for (int i = 0; i < 600; ++i) {
        sim.Step(1, frame++);
        if (late.session->GetState() != SessionClient::State::Joined) continue;
        const Entity remote = late.replication->FindEntity(prop_id);
        if (!remote.IsNull() &&
            (late.world.GetComponent<Transform>(remote)->position - moved).Length() < 1e-4f &&
            sim.Exact(late)) {
            synchronized = true;
            break;
        }
    }
    CHECK(synchronized);
    CHECK(sim.session->Players().size() == static_cast<usize>(kClients));
    CHECK(sim.pawns.size() == static_cast<usize>(kClients));
    sim.Step(120, frame);
    for (const auto& c : sim.clients) CHECK(sim.Exact(*c));
}
