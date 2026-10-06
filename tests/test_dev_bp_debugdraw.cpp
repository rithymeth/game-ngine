// Phase 23 step 6: Debug.Draw* blueprint nodes compile to Op::Draw and the
// VM forwards them to the host's debug-draw sink.
#include "aether/blueprint/compiler.h"
#include "aether/blueprint/vm.h"
#include "aether/ecs/world.h"
#include "test_framework.h"

using namespace aether;
using namespace aether::bp;
using nlohmann::json;

namespace {

Blueprint Parse(const char* text) {
    Blueprint bp;
    std::string error;
    AETHER_CHECK(BlueprintFromJson(json::parse(text), bp, &error));
    return bp;
}

std::shared_ptr<const CompiledBlueprint> Compile(const Blueprint& bp) {
    CompileResult result = CompileBlueprint(bp);
    for (const Diagnostic& d : result.diagnostics.diagnostics) {
        if (d.severity == Severity::Error) std::printf("    %s node %u: %s\n", d.code.c_str(), d.node, d.message.c_str());
    }
    AETHER_CHECK(result.Ok());
    return result.blueprint;
}

const char* kDraw = R"({
  "$type": "Blueprint", "$version": 1,
  "graphs": [
    { "name": "EventGraph", "kind": "EventGraph",
      "nodes": [
        { "id": 1, "type": "Event.BeginPlay" },
        { "id": 2, "type": "Debug.DrawLine",
          "defaults": { "from": [0, 0, 0], "to": [1, 2, 3], "color": -256, "duration": 2.0 } },
        { "id": 3, "type": "Debug.DrawBox",
          "defaults": { "center": [5, 5, 5], "halfExtent": [1, 1, 1], "color": 16711680, "duration": 1.0 } },
        { "id": 4, "type": "Debug.DrawSphere",
          "defaults": { "center": [0, 0, 0], "radius": 7.5, "color": -1, "duration": 0.5 } },
        { "id": 5, "type": "Debug.DrawPoint",
          "defaults": { "location": [10, 0, 0], "size": 3.0, "color": -257, "duration": 0.25 } },
        { "id": 6, "type": "Debug.DrawText",
          "defaults": { "location": [0, 10, 0], "text": "hello", "color": -65281, "duration": 4.0 } }
      ],
      "links": [
        { "from": [1, "then"], "to": [2, "exec"] },
        { "from": [2, "then"], "to": [3, "exec"] },
        { "from": [3, "then"], "to": [4, "exec"] },
        { "from": [4, "then"], "to": [5, "exec"] },
        { "from": [5, "then"], "to": [6, "exec"] }
      ] }
  ]
})";

} // namespace

AETHER_TEST(BpDebugDraw_CompliesAndRunsDrawOpcodes) {
    const Blueprint bp = Parse(kDraw);
    std::shared_ptr<const CompiledBlueprint> compiled = Compile(bp);
    AETHER_CHECK(compiled != nullptr);
    if (!compiled) return;
    AETHER_CHECK(!compiled->debug_draws.empty());
    AETHER_CHECK(compiled->debug_draws.size() == 5);
    // Kind flags compiled correctly (dispatch order = link order).
    AETHER_CHECK(compiled->debug_draws[0].kind == DrawInfo::Kind::Line);
    AETHER_CHECK(compiled->debug_draws[1].kind == DrawInfo::Kind::Box);
    AETHER_CHECK(compiled->debug_draws[2].kind == DrawInfo::Kind::Sphere);
    AETHER_CHECK(compiled->debug_draws[3].kind == DrawInfo::Kind::Point);
    AETHER_CHECK(compiled->debug_draws[4].kind == DrawInfo::Kind::Text);

    World world;
    BlueprintVM vm(world, {});
    std::vector<BlueprintVM::DebugDrawCall> calls;
    vm.SetDebugDrawHandler([&](Entity, const BlueprintVM::DebugDrawCall& call) { calls.push_back(call); });

    const Entity e = world.CreateEntity();
    AETHER_CHECK(vm.Attach(e, compiled));
    vm.Dispatch(e, "Event.BeginPlay");

    AETHER_CHECK(calls.size() == 5);
    if (calls.size() < 5) return;
    // Line: from -> to.
    AETHER_CHECK(calls[0].kind == BlueprintVM::DebugDrawCall::Kind::Line);
    AETHER_CHECK_NEAR(calls[0].from.x, 0.0f, 1e-4f);
    AETHER_CHECK_NEAR(calls[0].to.x, 1.0f, 1e-4f);
    AETHER_CHECK_NEAR(calls[0].to.z, 3.0f, 1e-4f);
    AETHER_CHECK(calls[0].color == 0xFFFFFF00u);
    AETHER_CHECK_NEAR(calls[0].duration, 2.0f, 1e-4f);
    // Box: center + half-extent.
    AETHER_CHECK(calls[1].kind == BlueprintVM::DebugDrawCall::Kind::Box);
    AETHER_CHECK_NEAR(calls[1].from.x, 5.0f, 1e-4f);
    AETHER_CHECK_NEAR(calls[1].size.y, 1.0f, 1e-4f);
    AETHER_CHECK(calls[1].color == 0x00FF0000u);
    // Sphere: center + radius.
    AETHER_CHECK(calls[2].kind == BlueprintVM::DebugDrawCall::Kind::Sphere);
    AETHER_CHECK_NEAR(calls[2].to.x, 7.5f, 1e-4f);
    AETHER_CHECK_NEAR(calls[2].duration, 0.5f, 1e-4f);
    // Point.
    AETHER_CHECK(calls[3].kind == BlueprintVM::DebugDrawCall::Kind::Point);
    AETHER_CHECK_NEAR(calls[3].from.x, 10.0f, 1e-4f);
    // Text.
    AETHER_CHECK(calls[4].kind == BlueprintVM::DebugDrawCall::Kind::Text);
    AETHER_CHECK(calls[4].text == "hello");
    AETHER_CHECK_NEAR(calls[4].from.y, 10.0f, 1e-4f);
    AETHER_CHECK(calls[4].color == 0xFFFF00FFu);
    AETHER_CHECK_NEAR(calls[4].duration, 4.0f, 1e-4f);
}
