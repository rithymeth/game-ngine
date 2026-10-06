#include "aether/scene/entity_guid.h"
#include "aether/script/luau_host.h"
#include "net_rpc_fixture.h"
#include "test_framework.h"

// Phase 22 step 3: remote calls made from Luau go through the same router.

using namespace aether;
using namespace aether::script;
using namespace rpc_test;

#define CHECK(...) AETHER_CHECK((__VA_ARGS__))

AETHER_TEST(Rpc_LuauCallsRouteThroughTheNetwork) {
    RpcSim sim;
    auto& c1 = sim.AddClient();
    auto& c2 = sim.AddClient();
    sim.Step(10);
    const Entity pawn = sim.Spawn({0, 0, 0}, sim.peers[0]);
    sim.Step(10);
    GuidIndex g1, g2;
    LuauHost h1, h2;
    h1.BindWorld(&c1.world, &g1);
    h2.BindWorld(&c2.world, &g2);
    h1.SetGlobal("pawn", EntityRef{c1.replication->FindEntity(sim.NetId(pawn))});
    h2.SetGlobal("pawn", EntityRef{c2.replication->FindEntity(sim.NetId(pawn))});

    ScriptResult r = h1.Run("pawn:Get('RpcWeapon'):Fire(3); return pawn:Get('RpcWeapon').shots");
    CHECK(r.ok && r.values.size() == 1 && std::get<f64>(r.values[0]) == 0.0); // it ran on the server, not here
    r = h2.Run("pawn:Get('RpcWeapon'):Fire(3)");
    CHECK(!r.ok && r.error.find("was refused") != std::string::npos);
    sim.Step(5);
    CHECK(sim.Weapon(pawn).shots == 1 && sim.Weapon(pawn).power == 3);
    // A multicast on a client runs there only.
    CHECK(h2.Run("pawn:Get('RpcWeapon'):PlayEffect('lua')").ok);
    CHECK(sim.Weapon(c2, pawn)->effect == "lua" && sim.Weapon(c1, pawn)->effect.empty());
}
