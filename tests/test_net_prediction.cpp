#include "aether/net/bytes.h"
#include "aether/net/prediction.h"
#include "test_framework.h"

#include <algorithm>
#include <cmath>

// Phase 22 step 4: prediction and reconciliation - the movement step, input
// sanitizing, prediction that agrees with the server, corrections that ease
// instead of snapping, loss, replay, and a client that cheats.

using namespace aether;
using namespace aether::net;

namespace {

constexpr f32 kDt = 1.0f / 60.0f;

MoveSettings Settings() { return {}; }

// A client and a server over a simulated network, one fixed tick at a time.
struct Link {
    LoopbackNetwork net;
    LoopbackTransport& server_link;
    LoopbackTransport& client_link;
    NetEndpoint server_ep;
    NetEndpoint client_ep;
    PredictionServer server;
    PredictionClient client;
    f64 t = 0.0;
    u64 states_received = 0;

    Link(MoveStepFn server_step, MoveStepFn client_step, u64 seed = 1)
        : net(seed),
          server_link(net.CreateEndpoint()),
          client_link(net.CreateEndpoint()),
          server_ep(server_link),
          client_ep(client_link),
          server(std::move(server_step)),
          client(std::move(client_step)) {
        server_ep.Listen();
        client_ep.Connect(server_link.LocalAddress());
        for (int i = 0; i < 100; ++i) Step(nullptr, false);
        server.AddPeer(client_link.LocalAddress(), MoveState{});
        client.Reset(MoveState{});
    }

    void Step(const MoveInput* input, bool play = true) {
        if (play && input != nullptr) {
            client_ep.Send(server_link.LocalAddress(), Channel::Unreliable, client.Predict(*input));
        }
        t += kDt;
        net.Advance(kDt);
        client_ep.Update(t);
        server_ep.Update(t);
        NetEvent e;
        while (server_ep.Poll(e)) {
            if (e.type == NetEventType::Message && !e.data.empty() && e.data[0] == kInputMessage) {
                server.OnInputMessage(e.peer, e.data);
            }
        }
        if (play) {
            server.Tick();
            const std::vector<u8> state = server.BuildStateMessage(client_link.LocalAddress());
            if (!state.empty()) server_ep.Send(client_link.LocalAddress(), Channel::Unreliable, state);
        }
        server_ep.Update(t);
        while (client_ep.Poll(e)) {
            if (e.type == NetEventType::Message && !e.data.empty() && e.data[0] == kStateMessage) {
                if (client.OnStateMessage(e.data)) ++states_received;
            }
        }
        client.Update(kDt);
    }
    void Run(const MoveInput& input, int ticks) {
        for (int i = 0; i < ticks; ++i) Step(&input);
    }
    const MoveState& ServerState() { return *server.StateOf(client_link.LocalAddress()); }
};

f32 Dist(const Vec3& a, const Vec3& b) { return (a - b).Length(); }

} // namespace

AETHER_TEST(NetPrediction_StepMovement) {
    const MoveSettings s = Settings();
    MoveState state;
    MoveInput forward;
    forward.move_x = 1.0f;
    // Accelerates toward walk speed without overshooting it.
    for (int i = 0; i < 120; ++i) StepMovement(s, {}, state, forward);
    AETHER_CHECK_NEAR(state.velocity.x, s.walk_speed, 1e-4f);
    AETHER_CHECK(state.position.x > 6.0f && state.grounded);
    forward.run = true;
    for (int i = 0; i < 120; ++i) StepMovement(s, {}, state, forward);
    AETHER_CHECK_NEAR(state.velocity.x, s.run_speed, 1e-4f);

    // Braking to a stop.
    MoveInput none;
    for (int i = 0; i < 60; ++i) StepMovement(s, {}, state, none);
    AETHER_CHECK_NEAR(state.velocity.x, 0.0f, 1e-4f);

    // A jump goes up, comes down and lands.
    MoveState j;
    MoveInput jump;
    jump.jump = true;
    StepMovement(s, {}, j, jump);
    AETHER_CHECK(!j.grounded && j.velocity.y > 0.0f && j.position.y > 0.0f);
    f32 peak = 0.0f;
    for (int i = 0; i < 120; ++i) {
        StepMovement(s, {}, j, none);
        peak = std::max(peak, j.position.y);
    }
    AETHER_CHECK(peak > 1.0f && peak < 1.4f); // v^2 / 2g = 1.27 m
    AETHER_CHECK(j.grounded && j.position.y == 0.0f && j.velocity.y == 0.0f);

    // No double jump in the air.
    MoveState air;
    StepMovement(s, {}, air, jump);
    const f32 vy = air.velocity.y;
    StepMovement(s, {}, air, jump);
    AETHER_CHECK(air.velocity.y < vy);

    // Ground follows a height function, and walking off an edge makes you fall.
    GroundFn ramp = [](f32 x, f32) { return x < 10.0f ? 0.1f * x : 0.0f; };
    MoveState r;
    MoveInput east;
    east.move_x = 1.0f;
    for (int i = 0; i < 200; ++i) StepMovement(s, ramp, r, east);
    AETHER_CHECK(r.position.x > 10.0f && r.grounded && r.position.y == 0.0f);
}

AETHER_TEST(NetPrediction_StepIsDeterministic) {
    const MoveStepFn step = MakeMoveStep(Settings());
    auto run = [&] {
        MoveState s;
        for (int i = 0; i < 500; ++i) {
            MoveInput in;
            in.move_x = std::sin(static_cast<f32>(i) * 0.07f);
            in.move_z = std::cos(static_cast<f32>(i) * 0.05f);
            in.jump = i % 97 == 0;
            step(s, Sanitize(in));
        }
        return s;
    };
    const MoveState a = run(), b = run();
    AETHER_CHECK(a.position.x == b.position.x && a.position.y == b.position.y && a.position.z == b.position.z);
}

AETHER_TEST(NetPrediction_SanitizeClampsAndQuantizes) {
    MoveInput big;
    big.move_x = 3.0f;
    big.move_z = 4.0f;
    const MoveInput s = Sanitize(big);
    AETHER_CHECK_NEAR(std::sqrt(s.move_x * s.move_x + s.move_z * s.move_z), 1.0f, 0.01f);
    AETHER_CHECK_NEAR(s.move_x, 0.6f, 0.01f);

    MoveInput odd;
    odd.move_x = 0.33333f;
    const MoveInput q = Sanitize(odd);
    AETHER_CHECK(q.move_x != odd.move_x && std::fabs(q.move_x - odd.move_x) < 0.005f);
    AETHER_CHECK(Sanitize(q).move_x == q.move_x); // quantizing is stable

    MoveInput nan;
    nan.move_x = std::nanf("");
    nan.move_z = INFINITY;
    const MoveInput n = Sanitize(nan);
    AETHER_CHECK(n.move_x == 0.0f && n.move_z == 0.0f);
}

AETHER_TEST(NetPrediction_ClientMovesImmediatelyAndAgreesWithTheServer) {
    const MoveStepFn step = MakeMoveStep(Settings());
    Link link(step, step);
    link.net.conditions.latency = 0.05;
    MoveInput forward;
    forward.move_x = 1.0f;
    link.Step(&forward);
    AETHER_CHECK(link.client.State().position.x > 0.0f);          // moved this very tick
    AETHER_CHECK(link.ServerState().position.x == 0.0f);          // the server hasn't heard yet

    MoveInput jump = forward;
    for (int i = 0; i < 180; ++i) {
        jump.jump = i == 30 || i == 100;
        link.Step(&jump);
    }
    MoveInput idle;
    link.Run(idle, 120);
    // The server and the client's prediction never disagreed, so nothing was corrected.
    AETHER_CHECK(link.client.Stats().reconciliations == 0 && link.states_received > 100);
    AETHER_CHECK(Dist(link.client.State().position, link.ServerState().position) < 1e-3f);
    AETHER_CHECK(link.client.Acknowledged() == link.client.LastSequence() - 0 || link.client.Unacknowledged() <= 12);
    AETHER_CHECK(link.server.Stats().processed >= link.client.Stats().inputs - 12);
}

AETHER_TEST(NetPrediction_ServerWallCorrectsTheClientSmoothly) {
    const MoveSettings s = Settings();
    // The server knows about a wall at x = 3 that the client doesn't.
    const MoveStepFn plain = MakeMoveStep(s);
    const MoveStepFn walled = [plain](MoveState& st, const MoveInput& in) {
        plain(st, in);
        if (st.position.x > 3.0f) {
            st.position.x = 3.0f;
            st.velocity.x = 0.0f;
        }
    };
    Link link(walled, plain);
    link.net.conditions.latency = 0.03;
    MoveInput forward;
    forward.move_x = 1.0f;

    f32 biggest_visual_step = 0.0f;
    Vec3 last_visual = link.client.VisualPosition();
    for (int i = 0; i < 240; ++i) {
        link.Step(&forward);
        biggest_visual_step = std::max(biggest_visual_step, Dist(link.client.VisualPosition(), last_visual));
        last_visual = link.client.VisualPosition();
    }
    AETHER_CHECK(link.client.Stats().reconciliations > 0);
    AETHER_CHECK(link.client.Stats().snaps == 0); // small errors ease; they don't jump
    // The view never moved by more than the character's own speed allows in one tick, plus a bit.
    AETHER_CHECK(biggest_visual_step < 7.0f * kDt * 1.5f);
    // Still pushing forward, the client predicts a few in-flight inputs past the wall (about
    // round-trip time x speed), never more; the server holds it at the wall.
    AETHER_CHECK(link.client.State().position.x > 3.0f && link.client.State().position.x < 3.6f);
    AETHER_CHECK_NEAR(link.ServerState().position.x, 3.0f, 1e-3f);
    // Letting go, the prediction settles exactly where the server has it.
    MoveInput idle;
    link.Run(idle, 90);
    AETHER_CHECK_NEAR(link.client.State().position.x, 3.0f, 0.01f);
    AETHER_CHECK_NEAR(link.client.VisualPosition().x, 3.0f, 0.01f);
}

AETHER_TEST(NetPrediction_ReconcileReplaysUnacknowledgedInputs) {
    const MoveSettings s = Settings();
    const MoveStepFn step = MakeMoveStep(s);
    PredictionClient client(step);
    MoveInput forward;
    forward.move_x = 1.0f;
    std::vector<MoveState> after;
    for (int i = 0; i < 10; ++i) {
        client.Predict(forward);
        after.push_back(client.State());
    }
    // The server says that after input 5 the character was 0.5 m further along.
    MoveState server = after[4];
    server.position.x += 0.5f;
    client.Reconcile(5, server);
    AETHER_CHECK(client.Stats().reconciliations == 1 && client.Unacknowledged() == 5);
    AETHER_CHECK_NEAR(client.Stats().last_error, 0.5f, 1e-4f);
    AETHER_CHECK(client.Stats().replayed_inputs == 5);

    // The result equals starting from the server's state and applying inputs 6..10.
    MoveState expected = server;
    for (int i = 0; i < 5; ++i) step(expected, Sanitize(forward));
    AETHER_CHECK(Dist(client.State().position, expected.position) < 1e-5f);
    // The view hasn't moved yet: the offset hides the 0.5 m and eases out.
    AETHER_CHECK(Dist(client.VisualPosition(), after[9].position) < 1e-5f);
    for (int i = 0; i < 120; ++i) client.Update(kDt);
    AETHER_CHECK(Dist(client.VisualPosition(), client.State().position) < 1e-3f);

    // A matching report changes nothing; an old one is ignored.
    client.Reconcile(7, [&] {
        MoveState m = after[6];
        m.position.x = client.State().position.x; // unrelated: just not applied below
        return m;
    }());
    const u64 reconciliations = client.Stats().reconciliations;
    client.Reconcile(3, server);
    AETHER_CHECK(client.Stats().reconciliations == reconciliations);
}

AETHER_TEST(NetPrediction_BigDisagreementsSnap) {
    const MoveStepFn step = MakeMoveStep(Settings());
    Link link(step, step);
    MoveInput idle;
    link.Run(idle, 30);
    link.server.SetState(link.client_link.LocalAddress(), MoveState{Vec3(50, 0, 0), Vec3(0, 0, 0), true}); // teleported
    link.Run(idle, 30);
    AETHER_CHECK(link.client.Stats().snaps == 1);
    AETHER_CHECK(Dist(link.client.VisualPosition(), link.client.State().position) < 1e-4f);
    AETHER_CHECK_NEAR(link.client.State().position.x, 50.0f, 1e-3f);
}

AETHER_TEST(NetPrediction_RedundancySurvivesLoss) {
    const MoveStepFn step = MakeMoveStep(Settings());
    Link link(step, step, 17);
    link.net.conditions.latency = 0.02;
    link.net.conditions.jitter = 0.02;
    link.net.conditions.loss = 0.3;
    for (int i = 0; i < 400; ++i) {
        MoveInput in;
        in.move_x = std::sin(static_cast<f32>(i) * 0.1f);
        in.move_z = std::cos(static_cast<f32>(i) * 0.13f);
        in.jump = i % 70 == 0;
        link.Step(&in);
    }
    link.net.conditions.loss = 0.0;
    MoveInput idle;
    link.Run(idle, 180);

    const u64 sent = link.client.Stats().inputs;
    AETHER_CHECK(link.server.Stats().processed + 8 >= sent); // all but, at worst, a handful were heard
    AETHER_CHECK(Dist(link.client.State().position, link.ServerState().position) < 0.02f);
    AETHER_CHECK(link.server.LastProcessed(link.client_link.LocalAddress()) + 4 >= link.client.LastSequence()); // the newest are still in flight
}

AETHER_TEST(NetPrediction_SpeedHackIsLimited) {
    const MoveStepFn step = MakeMoveStep(Settings());
    PredictionServer server(step);
    server.AddPeer(1, MoveState{});
    u32 sequence = 1;
    for (int tick = 0; tick < 60; ++tick) {
        // Sixteen new inputs every tick instead of one.
        ByteWriter w;
        w.U8(kInputMessage);
        w.Varint(16);
        w.Varint(sequence);
        for (int i = 0; i < 16; ++i) {
            w.U8(0);
            w.U8(127);
            w.U8(0);
        }
        sequence += 16;
        AETHER_CHECK(server.OnInputMessage(1, w.Data()));
        server.Tick();
    }
    AETHER_CHECK(server.Stats().processed <= 60 + 4); // one per tick on average, plus the burst allowance
    AETHER_CHECK(server.Stats().dropped > 500);       // the rest never fit the queue
    AETHER_CHECK(server.Queued(1) <= 32);
    // Moved no further than an honest client could in the same time.
    MoveState honest;
    MoveInput forward;
    forward.move_x = 1.0f;
    for (int i = 0; i < 64; ++i) StepMovement(Settings(), {}, honest, forward);
    AETHER_CHECK(server.StateOf(1)->position.x <= honest.position.x + 1e-3f);
}

AETHER_TEST(NetPrediction_ServerRejectsBadInput) {
    const MoveStepFn step = MakeMoveStep(Settings());
    PredictionServer server(step);
    server.AddPeer(1, MoveState{});
    auto msg = [](u8 kind, u64 count, u64 first, int bodies) {
        ByteWriter w;
        w.U8(kind);
        w.Varint(count);
        w.Varint(first);
        for (int i = 0; i < bodies * 3; ++i) w.U8(0);
        return w;
    };
    AETHER_CHECK(!server.OnInputMessage(1, msg(kStateMessage, 1, 1, 1).Data())); // wrong kind
    AETHER_CHECK(!server.OnInputMessage(1, msg(kInputMessage, 0, 1, 0).Data()));  // no inputs
    AETHER_CHECK(!server.OnInputMessage(1, msg(kInputMessage, 17, 1, 17).Data())); // too many
    AETHER_CHECK(!server.OnInputMessage(1, msg(kInputMessage, 2, 1, 1).Data()));   // body too short
    AETHER_CHECK(!server.OnInputMessage(1, msg(kInputMessage, 1, 0, 1).Data()));   // sequence 0
    AETHER_CHECK(!server.OnInputMessage(1, msg(kInputMessage, 1, 1ull << 40, 1).Data()));
    AETHER_CHECK(!server.OnInputMessage(99, msg(kInputMessage, 1, 1, 1).Data()));  // not a peer
    AETHER_CHECK(server.Stats().malformed == 6);

    // An over-long move vector is clamped, not trusted.
    ByteWriter cheat;
    cheat.U8(kInputMessage);
    cheat.Varint(1);
    cheat.Varint(1);
    cheat.U8(0);
    cheat.U8(127);
    cheat.U8(127);
    AETHER_CHECK(server.OnInputMessage(1, cheat.Data()));
    AETHER_CHECK(server.Stats().clamped == 1);
    for (int i = 0; i < 200; ++i) {
        ByteWriter more;
        more.U8(kInputMessage);
        more.Varint(1);
        more.Varint(2 + i);
        more.U8(0);
        more.U8(127);
        more.U8(127);
        server.OnInputMessage(1, more.Data());
        server.Tick();
    }
    const MoveState* s = server.StateOf(1);
    AETHER_CHECK(std::sqrt(s->velocity.x * s->velocity.x + s->velocity.z * s->velocity.z) <= Settings().walk_speed * 1.01f);

    // Duplicates and stale sequences are dropped.
    const u64 dropped = server.Stats().dropped;
    ByteWriter old;
    old.U8(kInputMessage);
    old.Varint(1);
    old.Varint(3);
    old.U8(0), old.U8(0), old.U8(0);
    AETHER_CHECK(server.OnInputMessage(1, old.Data()));
    AETHER_CHECK(server.Stats().dropped == dropped + 1);
}

AETHER_TEST(NetPrediction_InputsRunInOrderEvenWhenReordered) {
    const MoveStepFn step = MakeMoveStep(Settings());
    PredictionServer server(step);
    server.AddPeer(1, MoveState{});
    auto one = [](u32 seq, bool jump) {
        ByteWriter w;
        w.U8(kInputMessage);
        w.Varint(1);
        w.Varint(seq);
        w.U8(jump ? 1 : 0), w.U8(0), w.U8(0);
        return w;
    };
    server.OnInputMessage(1, one(3, false).Data());
    server.OnInputMessage(1, one(1, true).Data());  // the jump arrives late
    server.OnInputMessage(1, one(2, false).Data());
    AETHER_CHECK(server.Queued(1) == 3);
    server.Tick();
    server.Tick();
    server.Tick();
    AETHER_CHECK(server.LastProcessed(1) == 3 && server.StateOf(1)->position.y > 0.0f); // 1 ran first: it jumped
}

AETHER_TEST(NetPrediction_StateMessagesAreValidated) {
    const MoveStepFn step = MakeMoveStep(Settings());
    PredictionClient client(step);
    PredictionServer server(step);
    server.AddPeer(1, MoveState{Vec3(1, 2, 3), Vec3(0, 0, 0), false});
    std::vector<u8> good = server.BuildStateMessage(1);
    AETHER_CHECK(!good.empty() && client.OnStateMessage(good));
    AETHER_CHECK(server.BuildStateMessage(2).empty());

    std::vector<u8> wrong = good;
    wrong[0] = 0x01;
    AETHER_CHECK(!client.OnStateMessage(wrong));
    std::vector<u8> truncated(good.begin(), good.end() - 3);
    AETHER_CHECK(!client.OnStateMessage(truncated));
    std::vector<u8> extra = good;
    extra.push_back(0);
    AETHER_CHECK(!client.OnStateMessage(extra));

    ByteWriter nan;
    nan.U8(kStateMessage);
    nan.U32(0);
    nan.F32(std::nanf(""));
    for (int i = 0; i < 5; ++i) nan.F32(0.0f);
    nan.Bool(true);
    AETHER_CHECK(!client.OnStateMessage(nan.Data()));
}

AETHER_TEST(NetPrediction_HistoryIsBounded) {
    const MoveStepFn step = MakeMoveStep(Settings());
    PredictionConfig cfg;
    cfg.max_history = 20;
    PredictionClient client(step, cfg);
    MoveInput forward;
    forward.move_x = 1.0f;
    for (int i = 0; i < 100; ++i) client.Predict(forward);
    AETHER_CHECK(client.Unacknowledged() == 20 && client.LastSequence() == 100);

    // The message repeats at most redundancy + 1 inputs.
    const std::vector<u8> msg = client.Predict(forward);
    ByteReader r(msg);
    r.U8();
    AETHER_CHECK(r.Varint() == cfg.redundancy + 1);
    AETHER_CHECK(r.Varint() == 101 - cfg.redundancy);
}
