#include "aether/net/interpolation.h"
#include "aether/scene/components.h"
#include "test_framework.h"

#include <algorithm>
#include <cmath>

// Phase 22 step 5: snapshot interpolation - the wire format, the clock
// estimate, the per-entity buffer (interpolate, extrapolate, reorder), and
// smooth motion over a lossy, jittery link next to reliable replication.

using namespace aether;
using namespace aether::net;

namespace {

f32 Dist(const Vec3& a, const Vec3& b) { return (a - b).Length(); }

f32 Angle(const Quaternion& a, const Quaternion& b) {
    const f32 dot = std::fabs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w);
    return 2.0f * std::acos(std::min(1.0f, dot));
}

} // namespace

AETHER_TEST(NetSnapshot_MessageRoundTrip) {
    std::vector<EntitySnapshot> in;
    for (u32 i = 1; i <= 5; ++i) {
        EntitySnapshot s;
        s.net_id = i * 1000;
        s.position = Vec3(static_cast<f32>(i), -2.5f * static_cast<f32>(i), 1000.5f);
        s.rotation = Quaternion::FromAxisAngle(Vec3(0.3f, 1.0f, -0.2f), 0.7f * static_cast<f32>(i));
        in.push_back(s);
    }
    const auto messages = BuildSnapshotMessages(12.5, in, 1000);
    AETHER_CHECK(messages.size() == 1 && messages[0][0] == kSnapshotMessage);

    f64 time = 0.0;
    std::vector<EntitySnapshot> out;
    AETHER_CHECK(ParseSnapshotMessage(messages[0], time, out) && time == 12.5 && out.size() == 5);
    for (usize i = 0; i < in.size(); ++i) {
        AETHER_CHECK(out[i].net_id == in[i].net_id && Dist(out[i].position, in[i].position) == 0.0f);
        AETHER_CHECK(Angle(out[i].rotation, in[i].rotation) < 0.001f);
    }
    // 1 + 8 + 1 header, then (2 + 12 + 6) per entity for ids under 16384.
    AETHER_CHECK(messages[0].size() < 10 + 5 * 21);

    // A negated quaternion is the same rotation and survives.
    EntitySnapshot neg = in[0];
    neg.rotation = Quaternion(-neg.rotation.x, -neg.rotation.y, -neg.rotation.z, -neg.rotation.w);
    const auto m2 = BuildSnapshotMessages(1.0, std::span<const EntitySnapshot>(&neg, 1), 1000);
    std::vector<EntitySnapshot> back;
    AETHER_CHECK(ParseSnapshotMessage(m2[0], time, back) && Angle(back[0].rotation, in[0].rotation) < 0.001f);
}

AETHER_TEST(NetSnapshot_LargeSetsSplitAcrossMessages) {
    std::vector<EntitySnapshot> in(200);
    for (u32 i = 0; i < in.size(); ++i) in[i].net_id = i + 1;
    const auto messages = BuildSnapshotMessages(1.0, in, 1000);
    AETHER_CHECK(messages.size() > 1);
    usize total = 0;
    for (const auto& m : messages) {
        AETHER_CHECK(m.size() <= 1000);
        f64 t;
        std::vector<EntitySnapshot> part;
        AETHER_CHECK(ParseSnapshotMessage(m, t, part));
        total += part.size();
    }
    AETHER_CHECK(total == 200);
    AETHER_CHECK(BuildSnapshotMessages(1.0, std::span<const EntitySnapshot>{}, 1000).empty());
}

AETHER_TEST(NetSnapshot_ParseRejectsMalformedMessages) {
    EntitySnapshot s;
    s.net_id = 7;
    const auto good = BuildSnapshotMessages(2.0, std::span<const EntitySnapshot>(&s, 1), 1000)[0];
    f64 t;
    std::vector<EntitySnapshot> out;
    AETHER_CHECK(ParseSnapshotMessage(good, t, out));

    std::vector<u8> wrong = good;
    wrong[0] = 0x01;
    AETHER_CHECK(!ParseSnapshotMessage(wrong, t, out));
    for (usize n = 0; n < good.size(); ++n) AETHER_CHECK(!ParseSnapshotMessage({good.data(), n}, t, out)); // every truncation
    std::vector<u8> extra = good;
    extra.push_back(0);
    AETHER_CHECK(!ParseSnapshotMessage(extra, t, out));

    ByteWriter bomb;
    bomb.U8(kSnapshotMessage);
    bomb.F64(1.0);
    bomb.Varint(1ull << 40);
    AETHER_CHECK(!ParseSnapshotMessage(bomb.Data(), t, out) && out.empty());

    ByteWriter nan;
    nan.U8(kSnapshotMessage);
    nan.F64(1.0);
    nan.Varint(1);
    nan.Varint(1);
    nan.F32(std::nanf(""));
    nan.F32(0), nan.F32(0);
    nan.U16(0), nan.U16(0), nan.U16(0);
    AETHER_CHECK(!ParseSnapshotMessage(nan.Data(), t, out));

    ByteWriter bad_time;
    bad_time.U8(kSnapshotMessage);
    bad_time.F64(INFINITY);
    bad_time.Varint(0);
    AETHER_CHECK(!ParseSnapshotMessage(bad_time.Data(), t, out));
}

AETHER_TEST(NetSnapshot_ClockSyncFindsTheLeastDelayedOffset) {
    ClockSync clock;
    AETHER_CHECK(!clock.Synced());
    // The server's clock is 100 s ahead; packets take 50 ms, sometimes 200 ms.
    const f64 skew = 100.0;
    f64 now = 10.0;
    clock.OnServerTime(now - 0.2 + skew, now); // the first one is delayed
    AETHER_CHECK(clock.Synced());
    AETHER_CHECK_NEAR(clock.ServerNow(now), now + skew - 0.2, 1e-9);
    clock.OnServerTime(now + 1.0 - 0.05 + skew, now + 1.0); // a quicker one: better
    AETHER_CHECK_NEAR(clock.ServerNow(now + 1.0), now + 1.0 + skew - 0.05, 1e-9);
    // A delayed packet doesn't throw the estimate far.
    clock.OnServerTime(now + 2.0 - 0.3 + skew, now + 2.0);
    AETHER_CHECK(std::fabs(clock.ServerNow(now + 2.0) - (now + 2.0 + skew - 0.05)) < 0.01);

    // A restarted server (a very different clock) is adopted at once.
    clock.OnServerTime(5.0, now + 3.0);
    AETHER_CHECK_NEAR(clock.ServerNow(now + 3.0), 5.0, 1e-9);
}

AETHER_TEST(NetSnapshot_BufferInterpolatesBetweenSnapshots) {
    InterpolationBuffer buf;
    Vec3 p;
    Quaternion q;
    AETHER_CHECK(!buf.Sample(1.0, p, q));
    buf.Push(1.0, Vec3(0, 0, 0), Quaternion::Identity());
    buf.Push(1.1, Vec3(10, 0, 0), Quaternion::FromAxisAngle(Vec3(0, 1, 0), 1.0f));
    buf.Push(1.2, Vec3(10, 10, 0), Quaternion::FromAxisAngle(Vec3(0, 1, 0), 1.0f));

    bool extra = true;
    AETHER_CHECK(buf.Sample(1.05, p, q, &extra) && !extra);
    AETHER_CHECK_NEAR(p.x, 5.0f, 1e-4f);
    AETHER_CHECK_NEAR(Angle(q, Quaternion::FromAxisAngle(Vec3(0, 1, 0), 0.5f)), 0.0f, 0.01f); // halfway round
    AETHER_CHECK(buf.Sample(1.15, p, q) && Dist(p, Vec3(10, 5, 0)) < 1e-4f);
    AETHER_CHECK(buf.Sample(1.1, p, q) && Dist(p, Vec3(10, 0, 0)) < 1e-5f); // exactly on a snapshot

    // Before the oldest: held at the oldest.
    AETHER_CHECK(buf.Sample(0.5, p, q, &extra) && Dist(p, Vec3(0, 0, 0)) < 1e-6f && !extra);
}

AETHER_TEST(NetSnapshot_BufferRotatesTheShortWay) {
    InterpolationBuffer buf;
    const Quaternion a = Quaternion::FromAxisAngle(Vec3(0, 1, 0), 0.1f);
    const Quaternion b = Quaternion::FromAxisAngle(Vec3(0, 1, 0), 6.2f); // nearly a full turn: just behind `a`
    buf.Push(0.0, Vec3(), a);
    buf.Push(1.0, Vec3(), b);
    Vec3 p;
    Quaternion q;
    buf.Sample(0.5, p, q);
    // Half way is near 0 radians, not near 3.
    AETHER_CHECK(Angle(q, Quaternion::Identity()) < 0.2f);
    AETHER_CHECK_NEAR(q.Length(), 1.0f, 1e-4f);
}

AETHER_TEST(NetSnapshot_BufferExtrapolatesBrieflyThenHolds) {
    InterpolationConfig cfg;
    cfg.max_extrapolation = 0.2;
    InterpolationBuffer buf(cfg);
    buf.Push(1.0, Vec3(0, 0, 0), Quaternion::Identity());
    buf.Push(1.1, Vec3(1, 0, 0), Quaternion::Identity()); // 10 m/s
    Vec3 p;
    Quaternion q;
    bool extra = false;
    AETHER_CHECK(buf.Sample(1.2, p, q, &extra) && extra);
    AETHER_CHECK_NEAR(p.x, 2.0f, 1e-3f); // 0.1 s past the newest
    AETHER_CHECK(buf.Sample(1.3, p, q, &extra) && extra);
    AETHER_CHECK_NEAR(p.x, 3.0f, 1e-3f); // 0.2 s: the limit
    AETHER_CHECK(buf.Sample(5.0, p, q, &extra) && extra);
    AETHER_CHECK_NEAR(p.x, 3.0f, 1e-3f); // held, not flying off

    // With a single snapshot there is no velocity: it just holds.
    InterpolationBuffer one(cfg);
    one.Push(1.0, Vec3(4, 0, 0), Quaternion::Identity());
    AETHER_CHECK(one.Sample(1.5, p, q, &extra) && extra && p.x == 4.0f);
}

AETHER_TEST(NetSnapshot_BufferHandlesReorderingAndCapacity) {
    InterpolationBuffer buf;
    buf.Push(1.0, Vec3(0, 0, 0), Quaternion::Identity());
    buf.Push(1.2, Vec3(2, 0, 0), Quaternion::Identity());
    buf.Push(1.1, Vec3(1, 0, 0), Quaternion::Identity()); // late: slotted in between
    AETHER_CHECK(buf.Size() == 3);
    Vec3 p;
    Quaternion q;
    buf.Sample(1.15, p, q);
    AETHER_CHECK_NEAR(p.x, 1.5f, 1e-4f);

    buf.Push(1.1, Vec3(5, 0, 0), Quaternion::Identity()); // the same time again: replaced
    AETHER_CHECK(buf.Size() == 3);
    buf.Sample(1.1, p, q);
    AETHER_CHECK_NEAR(p.x, 5.0f, 1e-4f);

    InterpolationConfig cfg;
    cfg.capacity = 4;
    InterpolationBuffer small(cfg);
    for (int i = 0; i < 10; ++i) small.Push(1.0 + i * 0.1, Vec3(static_cast<f32>(i), 0, 0), Quaternion::Identity());
    AETHER_CHECK(small.Size() == 4 && std::fabs(small.NewestTime() - 1.9) < 1e-9);
    small.Push(0.5, Vec3(), Quaternion::Identity()); // older than everything kept: dropped
    AETHER_CHECK(small.Size() == 4);
}

AETHER_TEST(NetSnapshot_CaptureReadsReplicatedTransforms) {
    World world;
    Transform t1, t2;
    t1.position = Vec3(1, 2, 3);
    t2.position = Vec3(4, 5, 6);
    NetIdentity a, b, unassigned;
    a.net_id = 5;
    b.net_id = 2;
    b.owner = 9;
    world.CreateEntity(a, t1);
    world.CreateEntity(b, t2);
    world.CreateEntity(unassigned, t1); // no id yet: skipped
    world.CreateEntity(t1);             // no identity: skipped

    auto all = CaptureSnapshots(world);
    AETHER_CHECK(all.size() == 2 && all[0].net_id == 2 && all[1].net_id == 5 && all[0].position.x == 4.0f);
    auto filtered = CaptureSnapshots(world, [](const NetIdentity& id) { return id.owner != 9; });
    AETHER_CHECK(filtered.size() == 1 && filtered[0].net_id == 5);
}

AETHER_TEST(NetSnapshot_MotionIsSmoothOverALossyJitteryLink) {
    LoopbackNetwork net(5);
    LoopbackTransport& s_link = net.CreateEndpoint();
    LoopbackTransport& c_link = net.CreateEndpoint();
    NetEndpoint server(s_link), client(c_link);
    server.Listen();
    client.Connect(s_link.LocalAddress());

    World server_world, client_world;
    ReplicationServer repl_server(server_world, server);
    repl_server.ReplicateOnlyOnSpawn<Transform>();
    ReplicationClient repl_client(client_world);
    RegisterReplicatedComponent<Transform>();
    RegisterReplicatedComponent<NetIdentity>();
    SnapshotInterpolator interp;

    // One entity circling at 5 m/s along x, another owned by the client (it predicts that one).
    NetIdentity mover_id, own_id;
    own_id.owner = c_link.LocalAddress();
    const Entity mover = server_world.CreateEntity(mover_id, Transform{});
    const Entity own = server_world.CreateEntity(own_id, Transform{});
    server_world.GetComponent<NetIdentity>(mover)->owner = 0;

    f64 t = 0.0;
    const f64 dt = 1.0 / 60.0;
    NetEvent e;
    auto tick = [&](int frame, f32& client_x, f64* expect_time) {
        t += dt;
        // The server's world moves every frame; snapshots go out at 20 Hz.
        server_world.GetComponent<Transform>(mover)->position = Vec3(static_cast<f32>(5.0 * t), 0, 0);
        server_world.GetComponent<Transform>(own)->position = Vec3(static_cast<f32>(5.0 * t), 1, 0);
        net.Advance(dt);
        server.Update(t);
        client.Update(t);
        while (server.Poll(e)) {}
        while (client.Poll(e)) {
            if (ReplicationClient::IsReplicationMessage(e)) repl_client.Apply(e.data);
            else if (e.type == NetEventType::Message && !e.data.empty() && e.data[0] == kSnapshotMessage) interp.OnSnapshotMessage(e.data, t);
        }
        repl_server.Replicate();
        if (frame % 3 == 0) {
            for (const auto& m : BuildSnapshotMessages(t, CaptureSnapshots(server_world), 1000)) {
                for (NetAddress peer : server.ConnectedPeers()) server.Send(peer, Channel::Unreliable, m);
            }
        }
        interp.Apply(client_world, repl_client, t, c_link.LocalAddress());
        const Entity ce = repl_client.EntityOf(server_world.GetComponent<NetIdentity>(mover)->net_id);
        if (!ce.IsNull()) client_x = client_world.GetComponent<Transform>(ce)->position.x;
        if (expect_time != nullptr) *expect_time = t;
    };

    f32 client_x = 0.0f;
    for (int frame = 0; frame < 120; ++frame) tick(frame, client_x, nullptr); // connect and spawn
    net.conditions.latency = 0.05;
    net.conditions.jitter = 0.02;
    net.conditions.loss = 0.1;
    for (int frame = 120; frame < 200; ++frame) tick(frame, client_x, nullptr); // let the clock estimate settle

    const f32 own_x_after_spawn = [&] {
        const Entity c = repl_client.EntityOf(server_world.GetComponent<NetIdentity>(own)->net_id);
        return c.IsNull() ? -1.0f : client_world.GetComponent<Transform>(c)->position.x;
    }();
    f32 last_x = client_x;
    f32 biggest_step = 0.0f, worst_error = 0.0f;
    for (int frame = 200; frame < 800; ++frame) {
        tick(frame, client_x, nullptr);
        biggest_step = std::max(biggest_step, std::fabs(client_x - last_x));
        last_x = client_x;
        // The client shows the server as it was about (one-way latency + delay) ago.
        worst_error = std::max(worst_error, std::fabs(client_x - static_cast<f32>(5.0 * (t - 0.05 - 0.1))));
    }
    AETHER_CHECK(biggest_step < 0.2f);   // no jumps (steady is 5/60 = 0.083 per frame)
    AETHER_CHECK(worst_error < 0.25f);   // tracks the server's motion in the past
    AETHER_CHECK(interp.Stats().snapshots > 100 && interp.Stats().malformed == 0);

    // The entity the client owns is left to the client's own prediction.
    const Entity cown = repl_client.EntityOf(server_world.GetComponent<NetIdentity>(own)->net_id);
    AETHER_CHECK(!cown.IsNull() && own_x_after_spawn >= 0.0f && client_world.GetComponent<Transform>(cown)->position.x == own_x_after_spawn);
    // Transform replication was spawn-only: far fewer updates than frames.
    AETHER_CHECK(repl_server.Stats().updates < 20);
}

AETHER_TEST(NetSnapshot_SnapshotsForUnspawnedEntitiesWait) {
    World world;
    ReplicationClient repl(world);
    RegisterReplicatedComponent<NetIdentity>();
    RegisterReplicatedComponent<Transform>();
    SnapshotInterpolator interp;
    EntitySnapshot s;
    s.net_id = 3;
    s.position = Vec3(9, 0, 0);
    for (int i = 0; i < 4; ++i) {
        const auto m = BuildSnapshotMessages(1.0 + i * 0.05, std::span<const EntitySnapshot>(&s, 1), 1000)[0];
        AETHER_CHECK(interp.OnSnapshotMessage(m, 1.0 + i * 0.05));
    }
    interp.Apply(world, repl, 2.0); // nothing to apply to yet; must not crash
    AETHER_CHECK(interp.EntityCount() == 1 && world.EntityCount() == 0);

    const std::vector<u8> junk = {kSnapshotMessage, 1, 2};
    AETHER_CHECK(!interp.OnSnapshotMessage(junk, 2.0) && interp.Stats().malformed == 1);
    interp.Remove(3);
    AETHER_CHECK(interp.EntityCount() == 0);
}
