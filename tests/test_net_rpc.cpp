#include "aether/blueprint/compiler.h"
#include "aether/blueprint/vm.h"
#include "net_rpc_fixture.h"
#include "test_framework.h"

// Phase 22 step 3: RPCs - server calls from the owner only (and refused
// or rejected otherwise), client calls to the owner, multicasts to those
// who have the entity, unreliable calls, single-player worlds, and
// Blueprints routing through the same path.

using namespace aether;
using namespace rpc_test;

#define CHECK(...) AETHER_CHECK((__VA_ARGS__))

AETHER_TEST(Rpc_ServerCallsComeFromTheOwner) {
    RpcSim sim;
    auto& c1 = sim.AddClient();
    auto& c2 = sim.AddClient();
    sim.Step(10);
    CHECK(sim.peers.size() == 2);
    const Entity pawn = sim.Spawn({0, 0, 0}, sim.peers[0]);
    sim.Step(10);
    const Entity mine = c1.replication->FindEntity(sim.NetId(pawn));
    const Entity theirs = c2.replication->FindEntity(sim.NetId(pawn));
    CHECK(!mine.IsNull() && !theirs.IsNull());

    // The owner asks; the server runs it, the client doesn't.
    CHECK(Call(c1.world, mine, "Fire", {i32{7}}) == RemoteCallResult::Sent);
    sim.Step(5);
    CHECK(sim.Weapon(pawn).shots == 1 && sim.Weapon(pawn).power == 7);
    CHECK(sim.Weapon(pawn).caller == sim.peers[0]);
    CHECK(c1.world.GetComponent<RpcWeapon>(mine)->shots == 0);
    CHECK(c1.rpc->Stats().sent == 1 && sim.rpc->Stats().received == 1);

    // Someone else is refused at home...
    CHECK(Call(c2.world, theirs, "Fire", {i32{9}}) == RemoteCallResult::Refused);
    CHECK(c2.rpc->Stats().refused == 1 && c2.rpc->Stats().sent == 0);
    // ...and a client that lies about owning it is rejected by the server.
    c2.world.GetComponent<net::NetIdentity>(theirs)->locally_owned = true;
    CHECK(Call(c2.world, theirs, "Fire", {i32{9}}) == RemoteCallResult::Sent);
    sim.Step(5);
    CHECK(sim.Weapon(pawn).shots == 1 && sim.rpc->Stats().rejected == 1);

    // Unreliable calls arrive too (on a lossless link); on the server, server calls just run.
    CHECK(Call(c1.world, mine, "Ping") == RemoteCallResult::Sent);
    sim.Step(5);
    CHECK(sim.Weapon(pawn).pings == 1);
    CHECK(Call(sim.world, pawn, "Fire", {i32{1}}) == RemoteCallResult::RunLocally && sim.Weapon(pawn).shots == 2);
    // Wrong arguments are refused before anything is sent.
    CHECK(Call(c1.world, mine, "Fire", {std::string("x")}) == RemoteCallResult::Refused);
    // Ownership moves; so does the right to call.
    sim.world.GetComponent<net::NetIdentity>(pawn)->owner = sim.peers[1];
    sim.Step(10);
    CHECK(!c1.world.GetComponent<net::NetIdentity>(mine)->locally_owned);
    CHECK(c2.world.GetComponent<net::NetIdentity>(theirs)->locally_owned);
    CHECK(Call(c1.world, mine, "Fire", {i32{1}}) == RemoteCallResult::Refused);
    CHECK(Call(c2.world, theirs, "Fire", {i32{4}}) == RemoteCallResult::Sent);
    sim.Step(5);
    CHECK(sim.Weapon(pawn).shots == 3 && sim.Weapon(pawn).caller == sim.peers[1]);
}

AETHER_TEST(Rpc_ClientCallsAndMulticasts) {
    RpcSim sim;
    auto& c1 = sim.AddClient();
    auto& c2 = sim.AddClient();
    sim.Step(10);
    const net::PeerId p1 = sim.peers[0], p2 = sim.peers[1];
    const Entity pawn = sim.Spawn({0, 0, 0}, p1);
    const Entity far = sim.Spawn({100, 0, 0}, net::kNoPeer, 10);
    const Entity crate = sim.Spawn({0, 0, 0});
    sim.replication->SetViewer(p1, {0, 0, 0});
    sim.replication->SetViewer(p2, {100, 0, 0});
    sim.Step(10);
    CHECK(sim.Weapon(c1, far) == nullptr && sim.Weapon(c2, far) != nullptr);

    // A client call runs on the owner only.
    CHECK(Call(sim.world, pawn, "Notify", {std::string("you scored")}) == RemoteCallResult::Sent);
    sim.Step(5);
    CHECK(sim.Weapon(c1, pawn)->note == "you scored");
    CHECK(sim.Weapon(c2, pawn)->note.empty() && sim.Weapon(pawn).note.empty());
    // With no owning client, it runs on the server.
    CHECK(Call(sim.world, crate, "Notify", {std::string("mine")}) == RemoteCallResult::RunLocally);
    CHECK(sim.Weapon(crate).note == "mine");

    // A multicast runs on the server and on everyone who has the entity.
    CHECK(Call(sim.world, pawn, "PlayEffect", {std::string("boom")}) == RemoteCallResult::RunLocally);
    CHECK(Call(sim.world, far, "PlayEffect", {std::string("far boom")}) == RemoteCallResult::RunLocally);
    sim.Step(5);
    CHECK(sim.Weapon(pawn).effect == "boom" && sim.Weapon(c1, pawn)->effect == "boom" && sim.Weapon(c2, pawn)->effect == "boom");
    CHECK(sim.Weapon(far).effect == "far boom" && sim.Weapon(c2, far)->effect == "far boom");
    CHECK(c1.rpc->Stats().received == 2 && c2.rpc->Stats().received == 2);

    // Called on a client, a multicast or client call just runs there.
    const Entity c2_pawn = c2.replication->FindEntity(sim.NetId(pawn));
    CHECK(Call(c2.world, c2_pawn, "PlayEffect", {std::string("local")}) == RemoteCallResult::RunLocally);
    sim.Step(5);
    CHECK(sim.Weapon(c2, pawn)->effect == "local" && sim.Weapon(pawn).effect == "boom");
    // A non-remote function never goes anywhere.
    reflect::Any ret;
    const reflect::TypeInfo& type = reflect::Reflect<RpcWeapon>();
    CHECK(CallFunction(c2.world, c2_pawn, type, *type.FindFunction("Shots"), c2.world.GetComponent<RpcWeapon>(c2_pawn), {}, &ret));
    CHECK(ret.Get<i32>() == 0);
}

AETHER_TEST(Rpc_SinglePlayerAndRouterLifetime) {
    World world;
    const Entity e = world.CreateEntity(RpcWeapon{});
    CHECK(GetRemoteCallRouter(world) == nullptr);
    CHECK(Call(world, e, "Fire", {i32{3}}) == RemoteCallResult::RunLocally);
    CHECK(Call(world, e, "Notify", {std::string("hi")}) == RemoteCallResult::RunLocally);
    CHECK(world.GetComponent<RpcWeapon>(e)->shots == 1 && world.GetComponent<RpcWeapon>(e)->note == "hi");
    {
        RpcSim sim;
        CHECK(GetRemoteCallRouter(sim.world) == sim.rpc.get());
        auto& c = sim.AddClient();
        CHECK(GetRemoteCallRouter(c.world) == c.rpc.get());
        sim.rpc.reset();
        CHECK(GetRemoteCallRouter(sim.world) == nullptr);
    }
    // Unknown entities and garbage are counted, not crashed on.
    RpcSim sim;
    auto& c = sim.AddClient();
    sim.Step(10);
    const u8 junk[] = {3, 1, 2};
    c.host->Send(c.host->Peers()[0], 0, junk);
    const u8 nobody[] = {3, 99, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 0};
    c.host->Send(c.host->Peers()[0], 0, nobody);
    sim.Step(5);
    CHECK(sim.rpc->Stats().malformed == 1 && sim.rpc->Stats().unknown == 1);
}

AETHER_TEST(Rpc_BlueprintsRouteThroughTheNetwork) {
    using namespace aether::bp;
    RpcSim sim;
    auto& c1 = sim.AddClient();
    auto& c2 = sim.AddClient();
    sim.Step(10);
    const Entity pawn = sim.Spawn({0, 0, 0}, sim.peers[0]);
    sim.Step(10);
    // BeginPlay: Fire(power = 5) on self, then PlayEffect("bp").
    Blueprint bp;
    Graph g;
    g.name = "EventGraph";
    bp.graphs.push_back(g);
    GraphBuilder b(*bp.FindGraph("EventGraph"));
    const NodeId begin = b.Add("Event.BeginPlay"), fire = b.Add("Call.Native:RpcWeapon.Fire");
    const NodeId effect = b.Add("Call.Native:RpcWeapon.PlayEffect");
    b.Default(fire, "power", 5).Default(effect, "name", std::string("bp"));
    b.Connect(begin, "then", fire, "exec").Connect(fire, "then", effect, "exec");
    CompileResult compiled = CompileBlueprint(bp);
    CHECK(compiled.Ok());

    BlueprintVM vm1(c1.world), vm2(c2.world);
    CHECK(vm1.Attach(c1.replication->FindEntity(sim.NetId(pawn)), compiled.blueprint));
    CHECK(vm2.Attach(c2.replication->FindEntity(sim.NetId(pawn)), compiled.blueprint));
    vm1.BeginPlay();
    vm2.BeginPlay();
    sim.Step(5);
    CHECK(sim.Weapon(pawn).shots == 1 && sim.Weapon(pawn).power == 5); // only the owner's got through
    CHECK(vm1.Errors().empty());
    CHECK(vm2.Errors().size() == 1 && vm2.Errors()[0].code == "BP204");
    // The multicast, called on clients, ran on each of them locally.
    CHECK(sim.Weapon(c1, pawn)->effect == "bp" && sim.Weapon(c2, pawn)->effect == "bp" && sim.Weapon(pawn).effect.empty());
}
