#include "aether/net/interpolation.h"
#include "aether/net/prediction.h"
#include "test_framework.h"

#include <cmath>

// Phase 22 step 4: the interpolation buffer, snapshot interpolation that
// moves smoothly between snapshots, and predicted character movement that
// responds at once, agrees with the server, and reconciles when it doesn't.

using namespace aether;
using namespace aether::net;

#define CHECK(...) AETHER_CHECK((__VA_ARGS__))

namespace {

constexpr f64 kStep = 1.0 / 60.0;

struct PredSim {
    LoopbackNetwork net;
    World world;
    std::unique_ptr<DatagramSocket> socket;
    std::unique_ptr<NetHost> host;
    std::unique_ptr<ReplicationServer> replication;
    std::unique_ptr<MovementServer> movement;
    std::vector<PeerId> peers;
    struct Client {
        World world;
        std::unique_ptr<DatagramSocket> socket;
        std::unique_ptr<NetHost> host;
        std::unique_ptr<ReplicationClient> replication;
        std::unique_ptr<MovementClient> movement;
        std::unique_ptr<SnapshotInterpolation> interpolation;
    };
    std::vector<std::unique_ptr<Client>> clients;

    explicit PredSim(u64 seed = 1) : net(seed) {
        socket = net.Open(7777);
        host = std::make_unique<NetHost>(*socket);
        host->Listen();
        replication = std::make_unique<ReplicationServer>(world, *host);
        movement = std::make_unique<MovementServer>(world, *host, *replication);
    }
    Client& AddClient() {
        auto c = std::make_unique<Client>();
        c->socket = net.Open();
        c->host = std::make_unique<NetHost>(*c->socket);
        const PeerId server = c->host->Connect(Address::Loopback(7777), net.Now());
        c->replication = std::make_unique<ReplicationClient>(c->world, *c->host, server);
        c->movement = std::make_unique<MovementClient>(c->world, *c->host, *c->replication, server);
        c->interpolation = std::make_unique<SnapshotInterpolation>(c->world, *c->replication);
        clients.push_back(std::move(c));
        return *clients.back();
    }
    // One frame: the network, the server, then each client (input happens between frames).
    void Step(int frames = 1) {
        for (int i = 0; i < frames; ++i) {
            net.Advance(kStep);
            host->Update(net.Now());
            for (const NetEvent& e : host->TakeEvents()) {
                if (e.type == NetEventType::Connected) peers.push_back(e.peer);
                if (!replication->HandleEvent(e)) movement->HandleEvent(e);
            }
            movement->Update(net.Now());
            replication->Update(net.Now());
            for (auto& c : clients) {
                c->host->Update(net.Now());
                for (const NetEvent& e : c->host->TakeEvents())
                    if (!c->replication->HandleEvent(e)) c->movement->HandleEvent(e);
                c->interpolation->Update(net.Now());
            }
        }
    }
    Entity SpawnCharacter(PeerId owner, const Vec3& at = {}) {
        NetIdentity ni;
        SetNetArchetype(ni, "character");
        ni.owner = owner;
        return world.CreateEntity(ni, Transform{at, Quaternion{}}, NetMovement{});
    }
    u32 NetId(Entity e) { return world.GetComponent<NetIdentity>(e)->net_id; }
    Vec3 ServerPos(Entity e) { return world.GetComponent<Transform>(e)->position; }
    Vec3 ClientPos(Client& c, Entity server_entity) {
        return c.world.GetComponent<Transform>(c.replication->FindEntity(NetId(server_entity)))->position;
    }
};

bool Near(const Vec3& a, const Vec3& b, f32 tol = 1e-4f) { return (a - b).Length() <= tol; }

} // namespace

AETHER_TEST(Prediction_InterpolationBufferSamples) {
    InterpolationBuffer b;
    CHECK(!b.Sample(0.0, *std::make_unique<Transform>()));
    b.Push(1.0, Transform{Vec3(0, 0, 0), Quaternion{}});
    b.Push(2.0, Transform{Vec3(10, 0, 0), Quaternion::FromAxisAngle(Vec3(0, 1, 0), 1.5707963f)});
    b.Push(1.5, Transform{Vec3(99, 0, 0), Quaternion{}}); // out of order: ignored
    CHECK(b.Size() == 2 && b.Oldest() == 1.0 && b.Newest() == 2.0);
    Transform t;
    CHECK(b.Sample(1.25, t) && Near(t.position, Vec3(2.5f, 0, 0)));
    const Quaternion half = Quaternion::FromAxisAngle(Vec3(0, 1, 0), 0.7853982f);
    CHECK(b.Sample(1.5, t) && std::fabs(t.rotation.y - half.y) < 1e-3f && std::fabs(t.rotation.w - half.w) < 1e-3f);
    CHECK(b.Sample(0.0, t) && Near(t.position, Vec3(0, 0, 0)));  // before: the oldest
    CHECK(b.Sample(9.0, t) && Near(t.position, Vec3(10, 0, 0))); // after: the newest
    // The shorter arc: q and -q are the same rotation.
    Quaternion neg = half;
    neg.x = -neg.x, neg.y = -neg.y, neg.z = -neg.z, neg.w = -neg.w;
    const Quaternion mid = NlerpShortest(half, neg, 0.5f);
    CHECK(std::fabs(std::fabs(mid.w) - std::fabs(half.w)) < 1e-4f);
    for (int i = 0; i < 40; ++i) b.Push(3.0 + i, Transform{});
    CHECK(b.Size() == InterpolationBuffer::kCapacity);
}

AETHER_TEST(Prediction_InterpolationIsSmooth) {
    PredSim sim(3);
    sim.net.conditions.latency = 0.05;
    sim.net.conditions.jitter = 0.01;
    auto& viewer = sim.AddClient();
    sim.Step(10);
    const Entity mover = sim.SpawnCharacter(kNoPeer);
    f64 t = 0;
    auto move = [&] {
        t += kStep;
        sim.world.GetComponent<Transform>(mover)->position = Vec3(static_cast<f32>(t * 5.0), 0, 0);
    };
    for (int i = 0; i < 60; ++i) move(), sim.Step();
    // Raw snapshots arrive at 20 Hz: 0.25 m steps with still frames between.
    // Interpolated, every frame moves about 5/60 m.
    f32 prev = sim.ClientPos(viewer, mover).x, min_delta = 1e9f, max_delta = 0;
    f32 min_lag = 1e9f, max_lag = 0;
    const u32 starved = viewer.interpolation->Starved();
    for (int i = 0; i < 120; ++i) {
        move(), sim.Step();
        const f32 x = sim.ClientPos(viewer, mover).x;
        min_delta = std::min(min_delta, x - prev), max_delta = std::max(max_delta, x - prev);
        const f32 lag = sim.ServerPos(mover).x - x;
        min_lag = std::min(min_lag, lag), max_lag = std::max(max_lag, lag);
        prev = x;
    }
    CHECK(min_delta > 0.03f && max_delta < 0.14f);
    CHECK(min_lag > 0.4f && max_lag < 1.5f); // about (delay + latency + snapshot age) x 5 m/s
    CHECK(viewer.interpolation->Starved() == starved);
    CHECK(viewer.interpolation->Buffer(sim.NetId(mover))->Size() > 2);
    // Gone with its entity.
    const u32 id = sim.NetId(mover);
    sim.world.DestroyEntity(mover);
    sim.Step(20);
    CHECK(viewer.interpolation->Buffer(id) == nullptr);
}

AETHER_TEST(Prediction_RespondsAtOnceAndAgrees) {
    PredSim sim;
    sim.net.conditions.latency = 0.1;
    auto& owner = sim.AddClient();
    auto& other = sim.AddClient();
    sim.Step(10);
    const Entity pawn = sim.SpawnCharacter(sim.peers[0]);
    sim.Step(20);
    const Entity mine = owner.replication->FindEntity(sim.NetId(pawn));
    CHECK(!mine.IsNull() && owner.world.GetComponent<NetIdentity>(mine)->locally_owned);
    // Only the owner predicts.
    CHECK(!other.movement->Predict(other.replication->FindEntity(sim.NetId(pawn)), 1, 0, false, kStep));

    CHECK(owner.movement->Predict(mine, 1, 0, false, kStep));
    CHECK(owner.world.GetComponent<Transform>(mine)->position.x > 0.0f); // this frame, not a round trip later
    CHECK(sim.ServerPos(pawn).x == 0.0f);
    f32 peak_y = 0;
    for (int i = 1; i < 90; ++i) {
        owner.movement->Predict(mine, i < 60 ? 1.0f : 0.0f, i < 60 ? 0.5f : 0.0f, i == 20, kStep);
        sim.Step();
        peak_y = std::max(peak_y, owner.world.GetComponent<Transform>(mine)->position.y);
        if (i < 60 && i > 5) CHECK(owner.world.GetComponent<Transform>(mine)->position.x > sim.ServerPos(pawn).x);
    }
    CHECK(peak_y > 0.5f); // it jumped
    for (int i = 0; i < 30; ++i) owner.movement->Predict(mine, 0, 0, false, kStep), sim.Step();
    CHECK(Near(owner.world.GetComponent<Transform>(mine)->position, sim.ServerPos(pawn), 1e-4f));
    CHECK(sim.ServerPos(pawn).x > 3.0f && sim.ServerPos(pawn).y == 0.0f);
    CHECK(owner.movement->GetStats().corrections == 0 && owner.movement->GetStats().max_error < 1e-4f);
    CHECK(owner.movement->Pending(sim.NetId(pawn)) <= 15);
    sim.Step(20); // the last inputs are still on their way
    CHECK(sim.movement->GetStats().inputs_run == 120 && sim.movement->LastProcessed(sim.NetId(pawn)) == 120);
    CHECK(owner.movement->Pending(sim.NetId(pawn)) == 0);
    // The other client sees it where the server has it, smoothly.
    CHECK(Near(sim.ClientPos(other, pawn), sim.ServerPos(pawn), 1e-3f));
}

AETHER_TEST(Prediction_SurvivesLossAndReconciles) {
    PredSim sim(5);
    sim.net.conditions.latency = 0.06;
    sim.net.conditions.jitter = 0.02;
    sim.net.conditions.loss = 0.2;
    auto& owner = sim.AddClient();
    sim.Step(30);
    const Entity pawn = sim.SpawnCharacter(sim.peers[0]);
    sim.Step(40);
    const Entity mine = owner.replication->FindEntity(sim.NetId(pawn));
    CHECK(!mine.IsNull());
    for (int i = 0; i < 180; ++i) {
        const f32 a = static_cast<f32>(i) * 0.05f;
        owner.movement->Predict(mine, std::cos(a), std::sin(a), i % 50 == 10, kStep);
        sim.Step();
    }
    for (int i = 0; i < 90; ++i) owner.movement->Predict(mine, 0, 0, false, kStep), sim.Step();
    CHECK(sim.net.Dropped() > 0);
    CHECK(Near(owner.world.GetComponent<Transform>(mine)->position, sim.ServerPos(pawn), 1e-3f));
    CHECK(sim.movement->GetStats().duplicates > 0); // the redundancy at work

    // The server moves it on its own (a knockback): the owner is corrected.
    sim.world.GetComponent<Transform>(pawn)->position.x += 3.0f;
    const u32 before = owner.movement->GetStats().corrections;
    for (int i = 0; i < 60; ++i) owner.movement->Predict(mine, 0, 0, false, kStep), sim.Step();
    CHECK(owner.movement->GetStats().corrections > before);
    CHECK(owner.movement->GetStats().max_error > 2.5f);
    CHECK(Near(owner.world.GetComponent<Transform>(mine)->position, sim.ServerPos(pawn), 1e-3f));
}

AETHER_TEST(Prediction_ServerSanitizesAndChecksOwners) {
    // The rule itself: acceleration, top speed, gravity and landing.
    Transform t;
    NetMovement m;
    MovementInput in;
    in.dt = 0.05f, in.move_x = 1.0f;
    StepMovement(t, m, in);
    CHECK(std::fabs(m.velocity.x - 2.0f) < 1e-5f); // 40 m/s^2 x 0.05 s
    for (int i = 0; i < 20; ++i) StepMovement(t, m, in);
    CHECK(std::fabs(m.velocity.x - 6.0f) < 1e-5f);
    in.jump = true;
    StepMovement(t, m, in);
    CHECK(!m.grounded && t.position.y > 0.0f);
    in.jump = false;
    for (int i = 0; i < 40; ++i) StepMovement(t, m, in);
    CHECK(m.grounded && t.position.y == 0.0f && m.velocity.y == 0.0f);
    const MovementInput wild = SanitizeInput({1, 5.0f, 30.0f, 40.0f, false});
    CHECK(wild.dt == 0.1f && std::fabs(std::hypot(wild.move_x, wild.move_z) - 1.0f) < 1e-5f);
    CHECK(SanitizeInput({1, std::nanf(""), std::nanf(""), 0, false}).dt == 0.0f);

    PredSim sim;
    auto& owner = sim.AddClient();
    auto& thief = sim.AddClient();
    sim.Step(10);
    const Entity pawn = sim.SpawnCharacter(sim.peers[0]);
    sim.Step(20);
    const Entity mine = owner.replication->FindEntity(sim.NetId(pawn));
    // A speed hack: a 1 s frame at 10x input runs as a 0.1 s frame at full input, on both sides.
    CHECK(owner.movement->Predict(mine, 10, 0, false, 1.0f));
    sim.Step(10);
    CHECK(Near(owner.world.GetComponent<Transform>(mine)->position, sim.ServerPos(pawn)));
    CHECK(std::fabs(sim.ServerPos(pawn).x - 0.4f) < 1e-5f);
    // Someone else's inputs for it are rejected.
    const Entity theirs = thief.replication->FindEntity(sim.NetId(pawn));
    thief.world.GetComponent<NetIdentity>(theirs)->locally_owned = true; // lies locally
    CHECK(thief.movement->Predict(theirs, 1, 0, false, kStep));
    sim.Step(10);
    CHECK(sim.movement->GetStats().rejected == 1 && std::fabs(sim.ServerPos(pawn).x - 0.4f) < 1e-5f);
}
