#include "aether/job/job_system.h"
#include "aether/net/host.h"
#include "aether/physics/physics_scene.h"
#include "aether/physics_net/client.h"
#include "aether/physics_net/host.h"
#include "aether/scene/serialization.h"
#include "test_framework.h"

#include <cmath>
#include <cstring>
#include <memory>

using namespace aether;
using namespace aether::physics_net;

// Priority 2: physics over the network. The wire format, the host's grab rules, and two players moving a crate that one of them cannot lift,
// through the real NetHost on a simulated link with latency, jitter, loss and duplication.

namespace {

constexpr f32 kDt = 1.0f / 60.0f;

f32 Length(const Vec3& v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }

// A physics world: a floor, a 50 kg crate on it, and three pawns (holders) around it. Built the same way every time, so the host's and the
// clients' copies agree on GUIDs and start in the same state.
struct Scene3 {
    JobSystem jobs{2};
    World world;
    PhysicsWorld physics{jobs};
    PhysicsScene scene{world, physics};
    GuidIndex guids;
    Entity floor, crate, pawn[3];
    EntityGuid crate_guid;

    Scene3() {
        RegisterPhysicsComponentSerializers();
        BoxCollider floor_box;
        floor_box.half_extents = Vec3(20, 0.5f, 20);
        floor = world.CreateEntity(Transform{Vec3(0, 0, 0), Quaternion::Identity()}, floor_box, IdComponent{EntityGuid{0xC0DE, 1}});
        RigidBody rb;
        rb.mass = 50.0f; // 490 N
        rb.linear_damping = 0.0f;
        rb.angular_damping = 0.0f;
        BoxCollider box;
        box.half_extents = Vec3(0.5f, 0.5f, 0.5f);
        crate_guid = EntityGuid{0xC0DE, 2};
        crate = world.CreateEntity(Transform{Vec3(0, 1.0f, 0), Quaternion::Identity()}, rb, box, IdComponent{crate_guid});
        const Vec3 at[3] = {Vec3(-1.5f, 1.0f, 0), Vec3(1.5f, 1.0f, 0), Vec3(0, 1.0f, 2.0f)};
        for (int i = 0; i < 3; ++i) pawn[i] = world.CreateEntity(Transform{at[i], Quaternion::Identity()}, IdComponent{EntityGuid{0xC0DE, 10u + static_cast<u64>(i)}});
        guids.Rebuild(world);
        for (int i = 0; i < 10; ++i) scene.Step(kDt);
    }
    Vec3 Pos(Entity e) { return world.GetComponent<Transform>(e)->position; }
    void SetPawn(int i, const Vec3& p) { world.GetComponent<Transform>(pawn[i])->position = p; }
};

// A host and `clients` clients on one simulated network, each side with its own copy of the physics world.
struct Game {
    net::LoopbackNetwork net;
    std::unique_ptr<net::DatagramSocket> server_socket;
    std::unique_ptr<net::NetHost> server;
    std::vector<std::unique_ptr<net::DatagramSocket>> sockets;
    std::vector<std::unique_ptr<net::NetHost>> hosts;
    std::vector<net::PeerId> server_side_peer; // each client's id on the server
    std::vector<net::PeerId> server_peer;      // the server's id on each client
    Scene3 host_scene;
    std::vector<std::unique_ptr<Scene3>> client_scenes;
    std::unique_ptr<PhysicsNetHost> physics_host;
    std::vector<std::unique_ptr<PhysicsNetClient>> physics_clients;
    f64 time = 0.0;

    explicit Game(int clients, u64 seed = 7, PhysicsNetHostConfig config = {}) : net(seed) {
        config.policy.settings.max_force = 300.0f; // one holder can't lift the crate, two can
        server_socket = net.Open(7777);
        server = std::make_unique<net::NetHost>(*server_socket);
        server->Listen();
        physics_host = std::make_unique<PhysicsNetHost>(host_scene.scene, *server, host_scene.guids, config);
        for (int i = 0; i < clients; ++i) {
            sockets.push_back(net.Open());
            hosts.push_back(std::make_unique<net::NetHost>(*sockets.back()));
            client_scenes.push_back(std::make_unique<Scene3>());
            server_side_peer.push_back(net::kNoPeer);
            server_peer.push_back(hosts.back()->Connect(net::Address::Loopback(7777), net.Now()));
            physics_clients.push_back(std::make_unique<PhysicsNetClient>(client_scenes.back()->scene, *hosts.back(), server_peer.back()));
            for (int guard = 0; guard < 300 && server_side_peer.back() == net::kNoPeer; ++guard) Frame();
            physics_host->SetPlayer(server_side_peer.back(), host_scene.pawn[i]);
        }
    }

    void Frame() {
        time += kDt;
        net.Advance(kDt);
        server->Update(time);
        for (const net::NetEvent& e : server->TakeEvents()) {
            if (e.type == net::NetEventType::Connected) {
                for (net::PeerId& p : server_side_peer)
                    if (p == net::kNoPeer) {
                        p = e.peer;
                        break;
                    }
            }
            physics_host->HandleEvent(e);
        }
        for (usize i = 0; i < hosts.size(); ++i) {
            hosts[i]->Update(time);
            for (const net::NetEvent& e : hosts[i]->TakeEvents()) physics_clients[i]->HandleEvent(e);
        }
        host_scene.scene.Step(kDt);
        physics_host->Tick(time);
        for (auto& s : client_scenes) s->scene.Step(kDt);
    }
    void Run(int frames) {
        for (int i = 0; i < frames; ++i) Frame();
    }
};

// Two players carry the crate up and sideways. Returns the host's and the clients' crate positions at the end.
struct CarryResult {
    Vec3 host_crate;
    Vec3 client_crate[2];
    PhysicsNetHostStats host_stats;
    PhysicsNetClientStats client_stats[2];
    u64 dropped = 0, sent = 0;
    bool both_held = false;
};
CarryResult Carry(u64 seed, const net::LinkConditions& link) {
    Game game(2, seed);
    game.net.conditions = link;
    const EntityGuid crate = game.host_scene.crate_guid;
    game.physics_clients[0]->RequestGrab(crate, Vec3(-0.5f, 1.5f, 0));
    game.physics_clients[1]->RequestGrab(crate, Vec3(0.5f, 1.5f, 0));
    CarryResult out;
    for (int guard = 0; guard < 600; ++guard) {
        game.Frame();
        if (game.physics_clients[0]->StateOf(crate) == ClientGrabState::Held && game.physics_clients[1]->StateOf(crate) == ClientGrabState::Held) break;
    }
    out.both_held = game.physics_clients[0]->StateOf(crate) == ClientGrabState::Held && game.physics_clients[1]->StateOf(crate) == ClientGrabState::Held;
    // Lift to y = 2 (targets are where each holder's point should go), then carry to x = 2, then hold still.
    const auto aim = [&](f32 x, f32 y) {
        game.physics_clients[0]->SetTarget(crate, Vec3(x - 0.5f, y + 0.5f, 0));
        game.physics_clients[1]->SetTarget(crate, Vec3(x + 0.5f, y + 0.5f, 0));
    };
    for (int i = 0; i < 180; ++i) aim(0.0f, 2.0f), game.Frame();
    for (int i = 1; i <= 150; ++i) aim(2.0f * static_cast<f32>(i) / 150.0f, 2.0f), game.Frame();
    for (int i = 0; i < 240; ++i) aim(2.0f, 2.0f), game.Frame();
    out.host_crate = game.host_scene.Pos(game.host_scene.crate);
    for (int c = 0; c < 2; ++c) {
        out.client_crate[c] = game.client_scenes[c]->Pos(game.client_scenes[c]->crate);
        out.client_stats[c] = game.physics_clients[c]->Stats();
    }
    out.host_stats = game.physics_host->Stats();
    out.dropped = game.net.Dropped();
    out.sent = game.net.Sent();
    return out;
}

} // namespace

AETHER_TEST(PhysicsNet_ProtocolRoundTripsAndRejectsGarbage) {
    const EntityGuid guid{0x1122334455667788ull, 0x99AABBCCDDEEFF00ull};
    GrabRequest request;
    request.request_id = 513, request.object = guid, request.point = Vec3(1.5f, -2.0f, 3.25f);
    GrabRequest request_back;
    AETHER_CHECK(Decode(Encode(request), request_back) && request_back.request_id == 513 && request_back.object == guid && request_back.point.z == 3.25f);
    GrabTarget target;
    target.sequence = 0xFFFFFFF0u, target.object = guid, target.target = Vec3(0, 9, 0);
    GrabTarget target_back;
    AETHER_CHECK(Decode(Encode(target), target_back) && target_back.sequence == 0xFFFFFFF0u && target_back.target.y == 9.0f);
    GrabRelease release;
    release.object = guid;
    GrabRelease release_back;
    AETHER_CHECK(Decode(Encode(release), release_back) && release_back.object == guid);
    GrabResult result;
    result.request_id = 7, result.status = GrabStatus::TooManyHolders, result.object = guid;
    GrabResult result_back;
    AETHER_CHECK(Decode(Encode(result), result_back) && result_back.status == GrabStatus::TooManyHolders && result_back.request_id == 7);
    AETHER_CHECK(PeekMessageType(Encode(request)) == MessageType::GrabRequest && !PeekMessageType(std::span<const u8>{}));
    const std::vector<u8> unknown = {0x01, 0x02};
    AETHER_CHECK(!PeekMessageType(unknown));

    // Truncated, extended, the wrong type, a non-finite number, an unknown status.
    std::vector<u8> bytes = Encode(request);
    GrabRequest sink;
    for (usize n = 0; n < bytes.size(); ++n) AETHER_CHECK(!Decode(std::span<const u8>(bytes.data(), n), sink));
    std::vector<u8> longer = bytes;
    longer.push_back(0);
    AETHER_CHECK(!Decode(longer, sink));
    GrabTarget wrong_type;
    AETHER_CHECK(!Decode(Encode(release), wrong_type));
    std::vector<u8> nan_bytes = bytes;
    const u32 nan_bits = 0x7FC00000u;
    std::memcpy(&nan_bytes[nan_bytes.size() - 4], &nan_bits, 4);
    AETHER_CHECK(!Decode(nan_bytes, sink));
    std::vector<u8> status_bytes = Encode(result);
    status_bytes[3] = 200; // type, id (2), then the status
    GrabResult status_sink;
    AETHER_CHECK(!Decode(status_bytes, status_sink));
    AETHER_CHECK(std::string(GrabStatusName(GrabStatus::RateLimited)) == "RateLimited");
}

AETHER_TEST(PhysicsNet_SnapshotsSplitIntoChunksThatFitOneDatagram) {
    ConstraintSnapshot snapshot;
    for (u64 i = 0; i < 5; ++i) snapshot.joints.push_back(JointStateRecord{EntityGuid{1, i}, true, i % 2 == 0, false, 0.5f * static_cast<f32>(i), 1.0f});
    for (u64 i = 0; i < 60; ++i) {
        BodyStateRecord b;
        b.entity = EntityGuid{2, i};
        b.position = Vec3(static_cast<f32>(i), 1, 2);
        snapshot.bodies.push_back(b);
    }
    const std::vector<std::vector<u8>> chunks = EncodeSnapshotChunks(42, snapshot);
    AETHER_CHECK(chunks.size() > 1);
    usize joints = 0, bodies = 0;
    for (usize i = 0; i < chunks.size(); ++i) {
        AETHER_CHECK(chunks[i].size() <= 1000); // under what an unreliable message may hold (1024)
        SnapshotChunk chunk;
        AETHER_CHECK(Decode(chunks[i], chunk) && chunk.sequence == 42 && chunk.index == i && chunk.count == chunks.size());
        joints += chunk.snapshot.joints.size();
        bodies += chunk.snapshot.bodies.size();
    }
    AETHER_CHECK(joints == 5 && bodies == 60);
    // Nothing to say is still one (empty) chunk, so the sequence advances.
    const std::vector<std::vector<u8>> empty = EncodeSnapshotChunks(1, ConstraintSnapshot{});
    SnapshotChunk empty_chunk;
    AETHER_CHECK(empty.size() == 1 && Decode(empty[0], empty_chunk) && empty_chunk.snapshot.joints.empty() && empty_chunk.snapshot.bodies.empty());
    // An index past the count is refused.
    std::vector<u8> bad = chunks[0];
    bad[5] = 99; // index low byte
    SnapshotChunk bad_chunk;
    AETHER_CHECK(!Decode(bad, bad_chunk));
}

AETHER_TEST(PhysicsNet_GrabPolicyChecksEachRuleInOrder) {
    GrabPolicy policy;
    GrabFacts facts;
    facts.player_known = facts.object_known = facts.grabbable = facts.rate_ok = true;
    facts.holder_position = Vec3(0, 1, 0), facts.object_position = Vec3(1, 1, 0), facts.point = Vec3(1, 1.5f, 0);
    AETHER_CHECK(ValidateGrab(policy, facts) == GrabStatus::Granted);
    const auto with = [&](auto change) {
        GrabFacts f = facts;
        change(f);
        return ValidateGrab(policy, f);
    };
    AETHER_CHECK(with([](GrabFacts& f) { f.player_known = false; }) == GrabStatus::UnknownPlayer);
    AETHER_CHECK(with([](GrabFacts& f) { f.rate_ok = false; }) == GrabStatus::RateLimited);
    AETHER_CHECK(with([](GrabFacts& f) { f.object_known = false; }) == GrabStatus::UnknownObject);
    AETHER_CHECK(with([](GrabFacts& f) { f.grabbable = false; }) == GrabStatus::NotGrabbable);
    AETHER_CHECK(with([](GrabFacts& f) { f.already_holding_object = true; }) == GrabStatus::AlreadyHolding);
    AETHER_CHECK(with([](GrabFacts& f) { f.peer_grabs = 1; }) == GrabStatus::AlreadyHolding);
    AETHER_CHECK(with([](GrabFacts& f) { f.point = Vec3(9, 1, 0); }) == GrabStatus::TooFar);        // out of the holder's reach
    AETHER_CHECK(with([](GrabFacts& f) { f.object_position = Vec3(-3, 1, 0); }) == GrabStatus::TooFar); // not on the object
    AETHER_CHECK(with([](GrabFacts& f) { f.object_holders = 2; }) == GrabStatus::TooManyHolders);
    // The first broken rule wins: an unknown player with everything else wrong is still UnknownPlayer.
    AETHER_CHECK(with([](GrabFacts& f) { f.player_known = false; f.object_known = false; f.point = Vec3(99, 0, 0); }) == GrabStatus::UnknownPlayer);

    // Targets are pulled in to the holder's reach; ones inside, and the holder's own spot, are unchanged.
    const Vec3 holder(0, 1, 0);
    AETHER_CHECK(ClampGrabTarget(policy, holder, Vec3(1, 2, 0)).x == 1.0f);
    const Vec3 far = ClampGrabTarget(policy, holder, Vec3(100, 1, 0));
    AETHER_CHECK(std::fabs(far.x - policy.max_target_distance) < 1e-4f && far.y == 1.0f);
    AETHER_CHECK(ClampGrabTarget(policy, holder, holder).x == 0.0f);

    // The token bucket: a burst, then the refill rate, and time passed in.
    TokenBucket bucket;
    int taken = 0;
    for (int i = 0; i < 20; ++i) taken += bucket.Take(0.0, 4.0, 6.0) ? 1 : 0;
    AETHER_CHECK(taken == 6);
    AETHER_CHECK(!bucket.Take(0.1, 4.0, 6.0));  // 0.4 of a token
    AETHER_CHECK(bucket.Take(0.5, 4.0, 6.0));   // two tokens' worth since the start
    AETHER_CHECK(TokenBucket{}.Take(0.0, 0.0, 0.0)); // a rate of 0 is off
}

AETHER_TEST(PhysicsNet_TwoPlayersCarryACrateOneCannotLiftOnAGoodLink) {
    const CarryResult r = Carry(11, net::LinkConditions{});
    AETHER_CHECK(r.both_held && r.host_stats.granted == 2 && r.host_stats.denied == 0);
    AETHER_CHECK(std::fabs(r.host_crate.x - 2.0f) < 0.2f && std::fabs(r.host_crate.y - 2.0f) < 0.2f);
    for (int c = 0; c < 2; ++c) AETHER_CHECK(Length(r.client_crate[c] - r.host_crate) < 0.05f);
    AETHER_CHECK(r.host_stats.targets_applied > 400 && r.host_stats.snapshots_sent > 100);
}

AETHER_TEST(PhysicsNet_TwoPlayersStillCarryItUnderLatencyJitterLossAndDuplication) {
    for (const f32 loss : {0.2f, 0.4f}) {
        net::LinkConditions link;
        link.latency = loss > 0.3f ? 0.15 : 0.05;
        link.jitter = loss > 0.3f ? 0.08 : 0.03;
        link.loss = loss;
        link.duplicate = 0.05f;
        const CarryResult r = Carry(21, link);
        AETHER_CHECK(r.dropped > 0 && r.dropped < r.sent);
        AETHER_CHECK(r.both_held);
        // Lossy targets and snapshots slow things down but the carry still arrives, and the clients' copies end up with the host's crate.
        AETHER_CHECK(std::fabs(r.host_crate.x - 2.0f) < 0.4f && std::fabs(r.host_crate.y - 2.0f) < 0.3f);
        for (int c = 0; c < 2; ++c) {
            AETHER_CHECK(Length(r.client_crate[c] - r.host_crate) < 0.15f);
            AETHER_CHECK(r.client_stats[c].chunks_applied > 0 && r.client_stats[c].chunks_applied < r.host_stats.snapshots_sent); // some were lost
        }
        // Reordered and duplicated datagrams were dropped, not applied: nothing was malformed.
        AETHER_CHECK(r.host_stats.malformed == 0 && r.client_stats[0].malformed == 0);
    }
}

AETHER_TEST(PhysicsNet_ARunIsRepeatableForTheSameSeed) {
    net::LinkConditions link;
    link.latency = 0.06, link.jitter = 0.04, link.loss = 0.25f, link.duplicate = 0.05f;
    const CarryResult a = Carry(33, link), b = Carry(33, link), c = Carry(34, link);
    AETHER_CHECK(std::memcmp(&a.host_crate, &b.host_crate, sizeof(Vec3)) == 0);
    AETHER_CHECK(a.dropped == b.dropped && a.sent == b.sent);
    AETHER_CHECK(a.host_stats.targets_applied == b.host_stats.targets_applied && a.host_stats.targets_stale == b.host_stats.targets_stale);
    // Another seed drops different datagrams.
    AETHER_CHECK(c.dropped != a.dropped || c.host_stats.targets_stale != a.host_stats.targets_stale);
}

AETHER_TEST(PhysicsNet_HostRejectsWhatTheRulesForbid) {
    PhysicsNetHostConfig config;
    config.policy.first_target_grace = config.policy.target_timeout = 1000.0; // nobody is moving anything here, so nothing should time out
    Game game(3, 7, config);
    const EntityGuid crate = game.host_scene.crate_guid;
    PhysicsNetClient& alice = *game.physics_clients[0];
    PhysicsNetClient& bob = *game.physics_clients[1];
    PhysicsNetClient& carol = *game.physics_clients[2];

    // Too far: the grab point is nowhere near alice's pawn (the client's claim about where it stands doesn't matter; the host's pawn does).
    alice.RequestGrab(crate, Vec3(0, 1.5f, 40.0f));
    game.Run(30);
    AETHER_CHECK(alice.StateOf(crate) == ClientGrabState::Denied && alice.LastStatus(crate) == GrabStatus::TooFar);
    // A point within the pawn's reach but not on the crate: alice stands 5 m away along z and asks for a spot 3.5 m from the crate's centre.
    game.host_scene.SetPawn(0, Vec3(0, 1.0f, 5.0f));
    alice.RequestGrab(crate, Vec3(0, 1.0f, 3.5f));
    game.Run(30);
    AETHER_CHECK(alice.StateOf(crate) == ClientGrabState::Denied && alice.LastStatus(crate) == GrabStatus::TooFar);
    game.host_scene.SetPawn(0, Vec3(-1.5f, 1.0f, 0));
    // An entity that doesn't exist.
    bob.RequestGrab(EntityGuid{0xBAD, 0xBAD}, Vec3(0, 1, 0));
    game.Run(30);
    AETHER_CHECK(bob.StateOf(EntityGuid{0xBAD, 0xBAD}) == ClientGrabState::Denied && bob.LastStatus(EntityGuid{0xBAD, 0xBAD}) == GrabStatus::UnknownObject);
    // A pawn has no body, so it is not a dynamic body either.
    bob.RequestGrab(EntityGuid{0xC0DE, 10}, Vec3(-1.5f, 1, 0));
    game.Run(30);
    AETHER_CHECK(bob.LastStatus(EntityGuid{0xC0DE, 10}) == GrabStatus::UnknownObject);

    // The game's filter.
    game.physics_host->SetGrabbableFilter([](Entity) { return false; });
    bob.RequestGrab(crate, Vec3(0.5f, 1.5f, 0));
    game.Run(30);
    AETHER_CHECK(bob.StateOf(crate) == ClientGrabState::Denied && bob.LastStatus(crate) == GrabStatus::NotGrabbable);
    game.physics_host->SetGrabbableFilter({});

    // Alice and bob hold it; carol is third and turned away; alice asking again is turned away too.
    alice.RequestGrab(crate, Vec3(-0.5f, 1.5f, 0));
    bob.RequestGrab(crate, Vec3(0.5f, 1.5f, 0));
    game.Run(40);
    AETHER_CHECK(alice.StateOf(crate) == ClientGrabState::Held && bob.StateOf(crate) == ClientGrabState::Held);
    carol.RequestGrab(crate, Vec3(0.0f, 1.5f, 0.5f));
    game.Run(40);
    AETHER_CHECK(carol.StateOf(crate) == ClientGrabState::Denied && carol.LastStatus(crate) == GrabStatus::TooManyHolders);
    AETHER_CHECK(game.host_scene.scene.GrabCount(game.host_scene.crate) == 2);
    alice.RequestGrab(crate, Vec3(-0.5f, 1.5f, 0));
    game.Run(40);
    AETHER_CHECK(game.physics_host->GrabsOf(game.server_side_peer[0]) == 1); // still one: the second request was refused
    AETHER_CHECK(game.physics_host->Stats().denied_by[static_cast<usize>(GrabStatus::TooManyHolders)] == 1);

    // Requests past the rate are refused: carol spams.
    for (int i = 0; i < 20; ++i) carol.RequestGrab(crate, Vec3(0.0f, 1.5f, 0.5f));
    game.Run(60);
    AETHER_CHECK(game.physics_host->Stats().denied_by[static_cast<usize>(GrabStatus::RateLimited)] > 0);
    // A peer the host has no pawn for.
    game.physics_host->RemovePlayer(game.server_side_peer[2]);
    carol.RequestGrab(crate, Vec3(0.0f, 1.5f, 0.5f));
    game.Run(30);
    AETHER_CHECK(carol.LastStatus(crate) == GrabStatus::UnknownPlayer);
    AETHER_CHECK(game.physics_host->Stats().malformed == 0);
}

AETHER_TEST(PhysicsNet_GrabsEndWithTheirClientsTimeoutsAndReleases) {
    Game game(2);
    const EntityGuid crate = game.host_scene.crate_guid;
    PhysicsNetClient& alice = *game.physics_clients[0];
    PhysicsNetClient& bob = *game.physics_clients[1];
    alice.RequestGrab(crate, Vec3(-0.5f, 1.5f, 0));
    bob.RequestGrab(crate, Vec3(0.5f, 1.5f, 0));
    game.Run(40);
    AETHER_CHECK(alice.StateOf(crate) == ClientGrabState::Held && bob.StateOf(crate) == ClientGrabState::Held);
    AETHER_CHECK(game.host_scene.scene.GrabCount(game.host_scene.crate) == 2);

    // Both send a first target; then bob keeps sending and alice goes quiet: after the timeout the host lets her grab go and tells her.
    alice.SetTarget(crate, Vec3(-0.5f, 1.5f, 0));
    bob.SetTarget(crate, Vec3(0.5f, 1.5f, 0));
    game.Run(5);
    for (int i = 0; i < 100; ++i) {
        bob.SetTarget(crate, Vec3(0.5f, 1.5f, 0));
        game.Frame();
    }
    AETHER_CHECK(game.physics_host->Stats().timeouts == 1 && game.physics_host->GrabsOf(game.server_side_peer[0]) == 0);
    AETHER_CHECK(alice.StateOf(crate) == ClientGrabState::None && alice.LastStatus(crate) == GrabStatus::Released);
    AETHER_CHECK(game.host_scene.scene.GrabCount(game.host_scene.crate) == 1);
    AETHER_CHECK(bob.StateOf(crate) == ClientGrabState::Held);

    // Bob lets go himself.
    AETHER_CHECK(bob.Release(crate) && !bob.Release(crate));
    game.Run(30);
    AETHER_CHECK(game.host_scene.scene.GrabCount(game.host_scene.crate) == 0 && game.physics_host->Stats().releases == 1);

    // A client that disconnects loses its grabs.
    alice.RequestGrab(crate, Vec3(-0.5f, 1.5f, 0));
    game.Run(30);
    AETHER_CHECK(alice.StateOf(crate) == ClientGrabState::Held && game.host_scene.scene.GrabCount(game.host_scene.crate) == 1);
    game.hosts[0]->DisconnectAll(game.time);
    game.Run(60);
    AETHER_CHECK(game.host_scene.scene.GrabCount(game.host_scene.crate) == 0 && game.physics_host->GrabsOf(game.server_side_peer[0]) == 0);

    // A target for a grab the sender doesn't have is ignored, not applied.
    GrabTarget stray;
    stray.sequence = 1, stray.object = crate, stray.target = Vec3(0, 9, 0);
    game.hosts[1]->Send(game.server_peer[1], kSequencedChannel, Encode(stray));
    game.Run(30);
    AETHER_CHECK(game.physics_host->Stats().targets_ignored > 0);
}

AETHER_TEST(PhysicsNet_StaleChunksAndTargetsAreDropped) {
    Game game(1);
    const EntityGuid crate = game.host_scene.crate_guid;
    PhysicsNetClient& alice = *game.physics_clients[0];
    alice.RequestGrab(crate, Vec3(-0.5f, 1.5f, 0));
    game.Run(40);
    AETHER_CHECK(alice.StateOf(crate) == ClientGrabState::Held);

    // Targets: an older sequence, a repeat, then one that wrapped past the top of the range, sent by hand.
    const auto send = [&](u32 sequence, const Vec3& at) {
        GrabTarget t;
        t.sequence = sequence, t.object = crate, t.target = at;
        game.hosts[0]->Send(game.server_peer[0], kSequencedChannel, Encode(t));
        game.Run(10);
    };
    const u32 before = game.physics_host->Stats().targets_applied, stale_before = game.physics_host->Stats().targets_stale;
    send(0xFFFFFFF0u, Vec3(-1, 2, 0));
    send(0xFFFFFFF0u, Vec3(-1, 2, 0));          // a repeat
    send(0xFFFFFFEFu, Vec3(-1, 2, 0));          // older
    send(0x00000005u, Vec3(-1, 2, 0));          // newer, across the wrap
    AETHER_CHECK(game.physics_host->Stats().targets_applied - before == 2);
    AETHER_CHECK(game.physics_host->Stats().targets_stale - stale_before == 2);

    // Snapshot chunks: one with a newer number moves the client's crate; an older one (late, or duplicated) doesn't move it back.
    Scene3& view = *game.client_scenes[0];
    const auto chunk = [&](u32 sequence, f32 x) {
        ConstraintSnapshot snapshot;
        BodyStateRecord b;
        b.entity = crate;
        b.position = Vec3(x, 1.0f, 0);
        snapshot.bodies.push_back(b);
        SnapshotChunk c;
        c.sequence = sequence, c.snapshot = snapshot;
        net::NetEvent e;
        e.type = net::NetEventType::Message, e.peer = game.server_peer[0], e.channel = kUnreliableChannel, e.data = Encode(c);
        game.physics_clients[0]->HandleEvent(e);
    };
    const u32 stale_chunks = game.physics_clients[0]->Stats().chunks_stale;
    chunk(1000000u, 5.0f);
    AETHER_CHECK(std::fabs(view.Pos(view.crate).x - 5.0f) < 1e-4f);
    chunk(999999u, -5.0f); // older
    AETHER_CHECK(std::fabs(view.Pos(view.crate).x - 5.0f) < 1e-4f && game.physics_clients[0]->Stats().chunks_stale == stale_chunks + 1);
    chunk(1000000u, 6.0f); // the same snapshot's other chunk: still applied
    AETHER_CHECK(std::fabs(view.Pos(view.crate).x - 6.0f) < 1e-4f);
    // A chunk that isn't a valid snapshot is counted, not applied.
    net::NetEvent junk;
    junk.type = net::NetEventType::Message, junk.peer = game.server_peer[0], junk.channel = kUnreliableChannel;
    junk.data = {static_cast<u8>(MessageType::SnapshotChunk), 1, 2, 3};
    AETHER_CHECK(game.physics_clients[0]->HandleEvent(junk) && game.physics_clients[0]->Stats().malformed == 1);
}
