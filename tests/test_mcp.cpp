#include "editor_tools.h"
#include "mcp_server.h"
#include "test_framework.h"

#include <filesystem>
#include <set>
#include <sstream>

using namespace aether;
using namespace aether::mcp;

namespace {

struct Harness {
    EditorSession session;
    McpServer server{"test", "0"};
    int next_id = 1;

    Harness() {
        RegisterBuiltinComponents();
        RegisterEditorTools(server, session);
    }

    Json Rpc(const std::string& method, Json params = Json::object()) {
        Json req = {{"jsonrpc", "2.0"}, {"id", next_id++}, {"method", method}, {"params", params}};
        auto resp = server.HandleMessage(req);
        return resp ? *resp : Json(nullptr);
    }

    // The outcome of a tool call: `value` is the parsed JSON text on success;
    // `text` is the raw text (the message, on error).
    struct Result {
        bool is_error = false;
        Json value;
        std::string text;
    };
    Result Call(const std::string& tool, Json args = Json::object()) {
        Json resp = Rpc("tools/call", {{"name", tool}, {"arguments", args}});
        Result r;
        if (!resp.contains("result")) {
            r.is_error = true;
            r.text = resp.dump();
            return r;
        }
        r.is_error = resp["result"]["isError"];
        r.text = resp["result"]["content"][0]["text"];
        if (!r.is_error) {
            r.value = Json::parse(r.text, nullptr, false);
        }
        return r;
    }

    std::string Create(Json components = Json::object()) {
        Result r = Call("create_entity", {{"components", components}});
        return r.value["entity"];
    }

    Json Components(const std::string& guid) { return Call("get_entity", {{"entity", guid}}).value["components"]; }
};

const Json kPos123 = Json::array({1, 2, 3});

} // namespace

AETHER_TEST(Mcp_InitializeHandshake) {
    Harness h;
    Json resp = h.Rpc("initialize", {{"protocolVersion", "2024-11-05"}});
    AETHER_CHECK(resp["result"]["protocolVersion"] == "2024-11-05");
    AETHER_CHECK(resp["result"]["serverInfo"]["name"] == "test");
    AETHER_CHECK(resp["result"]["capabilities"].contains("tools"));

    Json newer = h.Rpc("initialize", {{"protocolVersion", "2099-01-01"}});
    AETHER_CHECK(newer["result"]["protocolVersion"] == "2025-06-18"); // offers its own

    // Notifications get no response; ping answers with {}.
    AETHER_CHECK(!h.server.HandleMessage({{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}}).has_value());
    AETHER_CHECK(h.Rpc("ping")["result"].empty());
}

AETHER_TEST(Mcp_ProtocolErrors) {
    Harness h;
    AETHER_CHECK(h.Rpc("no/such/method")["error"]["code"] == -32601);
    AETHER_CHECK(h.server.HandleText("{not json")->at("error")["code"] == -32700);
    AETHER_CHECK(h.server.HandleMessage(Json::array())->at("error")["code"] == -32600);
    AETHER_CHECK(h.Rpc("tools/call", {{"name", "nope"}})["error"]["code"] == -32602);
    AETHER_CHECK(h.Rpc("tools/call", {{"arguments", Json::object()}})["error"]["code"] == -32602);
}

AETHER_TEST(Mcp_ToolsListHasSchemas) {
    Harness h;
    Json tools = h.Rpc("tools/list")["result"]["tools"];
    AETHER_CHECK(tools.size() >= 10);
    bool has_create = false;
    for (const Json& t : tools) {
        AETHER_CHECK(t["inputSchema"]["type"] == "object");
        AETHER_CHECK(!t["description"].get<std::string>().empty());
        has_create = has_create || t["name"] == "create_entity";
    }
    AETHER_CHECK(has_create);
}

AETHER_TEST(Mcp_ListComponentTypes) {
    Harness h;
    Json types = h.Call("list_component_types").value;
    bool transform = false;
    for (const Json& t : types) {
        transform = transform || t["name"] == "Transform";
    }
    AETHER_CHECK(transform);
}

AETHER_TEST(Mcp_CreateAndGetEntity) {
    Harness h;
    std::string guid = h.Create({{"Transform", {{"position", {1, 2, 3}}}}});
    AETHER_CHECK(!guid.empty());

    Json c = h.Components(guid);
    AETHER_CHECK(c["Transform"]["position"] == kPos123);
    AETHER_CHECK(c.contains("IdComponent"));

    AETHER_CHECK(h.Call("list_entities").value["count"] == 1);
    AETHER_CHECK(h.Call("list_entities", {{"component", "Parent"}}).value["count"] == 0);
}

AETHER_TEST(Mcp_BadRequestsAreToolErrorsAndChangeNothing) {
    Harness h;
    auto bad = h.Call("create_entity", {{"components", {{"NoSuchComponent", Json::object()}}}});
    AETHER_CHECK(bad.is_error);
    AETHER_CHECK(bad.text.find("Known components") != std::string::npos);
    AETHER_CHECK(h.session.world.EntityCount() == 0);

    AETHER_CHECK(h.Call("get_entity", {{"entity", "not-a-guid"}}).is_error);
    AETHER_CHECK(h.Call("get_entity", {{"entity", "5c0a9e2e-7b1d-4c3a-9f10-2a6b8e4d1f77"}}).is_error);
    AETHER_CHECK(h.Call("undo").is_error); // nothing to undo
}

AETHER_TEST(Mcp_SetComponentIsPartialAndUndoable) {
    Harness h;
    std::string guid = h.Create({{"Transform", {{"position", {1, 2, 3}}}}});
    Json rotation = h.Components(guid)["Transform"]["rotation"];

    auto set = h.Call("set_component",
                      {{"entity", guid}, {"component", "Transform"}, {"value", {{"position", {9, 2, 3}}}}});
    AETHER_CHECK(!set.is_error);
    AETHER_CHECK(set.value["value"]["position"] == Json::array({9, 2, 3}));

    AETHER_CHECK(h.Call("undo").value["undone"] == "Edit Transform");
    AETHER_CHECK(h.Components(guid)["Transform"]["position"] == kPos123);
    AETHER_CHECK(h.Call("redo").value["redone"] == "Edit Transform");
    Json after = h.Components(guid)["Transform"];
    AETHER_CHECK(after["position"] == Json::array({9, 2, 3}));
    AETHER_CHECK(after["rotation"] == rotation); // fields not named are untouched

    // Wrongly-typed values are reported, not silently applied.
    auto wrong =
        h.Call("set_component", {{"entity", guid}, {"component", "Transform"}, {"value", {{"position", "x"}}}});
    AETHER_CHECK(wrong.is_error || wrong.value.contains("warnings"));
    AETHER_CHECK(h.Components(guid)["Transform"]["position"] == Json::array({9, 2, 3}));
}

AETHER_TEST(Mcp_AddAndRemoveComponent) {
    Harness h;
    std::string guid = h.Create();
    AETHER_CHECK(!h.Call("add_component", {{"entity", guid},
                                           {"component", "Transform"},
                                           {"value", {{"position", {4, 5, 6}}}}})
                      .is_error);
    AETHER_CHECK(h.Call("add_component", {{"entity", guid}, {"component", "Transform"}}).is_error); // already there
    AETHER_CHECK(h.Components(guid)["Transform"]["position"] == Json::array({4, 5, 6}));

    AETHER_CHECK(h.Call("undo").value["undone"] == "Add Transform"); // add + initial value are one step
    AETHER_CHECK(!h.Components(guid).contains("Transform"));

    h.Call("add_component", {{"entity", guid}, {"component", "Transform"}});
    AETHER_CHECK(!h.Call("remove_component", {{"entity", guid}, {"component", "Transform"}}).is_error);
    AETHER_CHECK(h.Call("remove_component", {{"entity", guid}, {"component", "IdComponent"}}).is_error);
}

AETHER_TEST(Mcp_DestroyAndUndoKeepsTheGuid) {
    Harness h;
    std::string guid = h.Create({{"Transform", {{"position", {7, 0, 0}}}}});
    AETHER_CHECK(!h.Call("destroy_entity", {{"entity", guid}}).is_error);
    AETHER_CHECK(h.Call("get_entity", {{"entity", guid}}).is_error);

    h.Call("undo");
    auto back = h.Call("get_entity", {{"entity", guid}});
    AETHER_CHECK(!back.is_error);
    AETHER_CHECK(back.value["components"]["Transform"]["position"] == Json::array({7, 0, 0}));
}

AETHER_TEST(Mcp_ReparentRejectsCycles) {
    Harness h;
    std::string a = h.Create();
    std::string b = h.Create();
    AETHER_CHECK(!h.Call("reparent", {{"entity", b}, {"parent", a}}).is_error);
    AETHER_CHECK(h.Components(b)["Parent"]["parent"] == a);
    AETHER_CHECK(h.Call("reparent", {{"entity", a}, {"parent", b}}).is_error); // a under its own child
    AETHER_CHECK(h.Call("reparent", {{"entity", a}, {"parent", a}}).is_error);

    AETHER_CHECK(!h.Call("reparent", {{"entity", b}}).is_error); // detach
    AETHER_CHECK(!h.Components(b).contains("Parent"));
}

AETHER_TEST(Mcp_CreateWithParent) {
    Harness h;
    std::string parent = h.Create();
    auto child = h.Call("create_entity", {{"parent", parent}});
    AETHER_CHECK(child.value["components"]["Parent"]["parent"] == parent);
}

AETHER_TEST(Mcp_SaveAndLoadScene) {
    auto path = (std::filesystem::temp_directory_path() / "aether_mcp_test_scene.json").string();
    std::string guid;
    {
        Harness h;
        guid = h.Create({{"Transform", {{"position", {1, 2, 3}}}}});
        AETHER_CHECK(!h.Call("save_scene", {{"path", path}}).is_error);
    }
    Harness h2;
    auto loaded = h2.Call("load_scene", {{"path", path}});
    AETHER_CHECK(!loaded.is_error);
    AETHER_CHECK(loaded.value["entities"] == 1);
    AETHER_CHECK(h2.Components(guid)["Transform"]["position"] == kPos123);

    // replace=true clears first rather than duplicating.
    AETHER_CHECK(h2.Call("load_scene", {{"path", path}, {"replace", true}}).value["entities"] == 1);
    AETHER_CHECK(h2.Call("load_scene", {{"path", "/definitely/not/here.json"}}).is_error);
    std::filesystem::remove(path);
}

AETHER_TEST(Mcp_PlayModeRestoresTheScene) {
    Harness h;
    std::string guid = h.Create({{"Transform", {{"position", {1, 2, 3}}}}});
    AETHER_CHECK(h.Call("editor_state").value["play_state"] == "editing");

    AETHER_CHECK(h.Call("play").value["play_state"] == "playing");
    h.Call("set_component", {{"entity", guid}, {"component", "Transform"}, {"value", {{"position", {50, 50, 50}}}}});
    h.Call("create_entity");
    AETHER_CHECK(h.session.world.EntityCount() == 2);
    AETHER_CHECK(h.Call("load_scene", {{"path", "x.json"}}).is_error); // not while playing

    AETHER_CHECK(h.Call("stop").value["play_state"] == "editing");
    AETHER_CHECK(h.session.world.EntityCount() == 1);
    AETHER_CHECK(h.Components(guid)["Transform"]["position"] == kPos123);
}

AETHER_TEST(Mcp_StdioLoop) {
    Harness h;
    std::stringstream in;
    in << R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18"}})" << "\r\n"
       << R"({"jsonrpc":"2.0","method":"notifications/initialized"})" << "\n"
       << "\n"
       << R"({"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"editor_state"}})" << "\n"
       << "garbage\n";
    std::stringstream out;
    h.server.RunStdio(in, out);

    std::vector<Json> lines;
    std::string line;
    while (std::getline(out, line)) {
        lines.push_back(Json::parse(line));
    }
    AETHER_CHECK(lines.size() == 3); // initialize, editor_state, parse error (notification and blank line: none)
    AETHER_CHECK(lines[0]["id"] == 1);
    AETHER_CHECK(lines[1]["id"] == 2 && lines[1]["result"]["isError"] == false);
    AETHER_CHECK(lines[2]["error"]["code"] == -32700);
}

// Hardening (Phase 44 step 1): requests with fields of the wrong type, a throwing tool, limits and the schema
// version. None of these may throw out of the server or end it.
AETHER_TEST(Mcp_WrongTypedFieldsAnswerWithErrors) {
    McpServer server("test", "0");
    bool threw = false;
    try {
        // "jsonrpc" as a number, "method" as a number, "id" as an object and an array, "params" as a string.
        for (const char* text : {R"({"jsonrpc":2,"id":1,"method":"ping"})", R"({"jsonrpc":"2.0","id":1,"method":5})",
                                 R"({"jsonrpc":"2.0","id":{"a":1},"method":"ping"})", R"({"jsonrpc":"2.0","id":[1],"method":"ping"})",
                                 R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":7}})",
                                 R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":"text"})",
                                 R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":"text"})", R"([{"jsonrpc":"2.0","id":1,"method":"ping"}])",
                                 R"(5)", R"(null)", R"("x")"}) {
            const std::optional<Json> response = server.HandleText(text);
            AETHER_CHECK(response.has_value() && response->contains("error") + response->contains("result") == 1);
        }
    } catch (...) {
        threw = true;
    }
    AETHER_CHECK(!threw);
    // The id of a bad-id request is answered with null, not echoed back as an object.
    const std::optional<Json> bad_id = server.HandleText(R"({"jsonrpc":"2.0","id":{"a":1},"method":"ping"})");
    AETHER_CHECK(bad_id && (*bad_id)["id"].is_null() && (*bad_id)["error"]["code"] == -32600);
}

AETHER_TEST(Mcp_ToolFailuresAndRequiredArguments) {
    McpServer server("test", "0");
    AETHER_CHECK(!server.AddTool(Tool{"", "no name", Json::object(), [](const Json&) { return Json("x"); }}));
    AETHER_CHECK(!server.AddTool(Tool{"no_handler", "no handler", Json::object(), nullptr}));
    Tool throwing{"throws", "throws a std::exception", {{"type", "object"}}, [](const Json&) -> Json { throw std::runtime_error("boom"); }};
    AETHER_CHECK(server.AddTool(throwing));
    Tool needs{"needs", "needs an argument", {{"type", "object"}, {"required", Json::array({"path"})}}, [](const Json&) { return Json("ok"); }};
    AETHER_CHECK(server.AddTool(needs));
    Tool typed{"typed", "typed arguments", {{"type", "object"}, {"properties", {{"path", {{"type", "string"}}}, {"count", {{"type", "integer"}}}}}}, [](const Json&) { return Json("typed"); }};
    AETHER_CHECK(server.AddTool(typed));
    Tool huge{"huge", "returns too much", {{"type", "object"}}, [](const Json&) { return Json(std::string(kMaxResultBytes + 1, 'x')); }};
    AETHER_CHECK(server.AddTool(huge));
    // Adding a tool of the same name replaces it.
    AETHER_CHECK(server.AddTool(Tool{"needs", "replaced", {{"type", "object"}}, [](const Json&) { return Json("ok2"); }}) && server.Tools().size() == 4);

    const auto call = [&](const char* tool) {
        return *server.HandleMessage({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"}, {"params", {{"name", tool}}}});
    };
    const Json thrown = call("throws");
    AETHER_CHECK(thrown["result"]["isError"] == true && thrown["result"]["content"][0]["text"].get<std::string>().find("boom") != std::string::npos);
    AETHER_CHECK(call("needs")["result"]["content"][0]["text"] == "ok2"); // replaced: nothing required now
    AETHER_CHECK(call("huge")["result"]["isError"] == true);

    server.AddTool(needs); // required "path" again
    const Json missing = call("needs");
    AETHER_CHECK(missing["error"]["code"] == -32602 && missing["error"]["message"].get<std::string>().find("path") != std::string::npos);
    const Json present = *server.HandleMessage({{"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/call"}, {"params", {{"name", "needs"}, {"arguments", {{"path", "a"}}}}}});
    AETHER_CHECK(present["result"]["isError"] == false);
    const Json wrong_type = *server.HandleMessage({{"jsonrpc", "2.0"}, {"id", 3}, {"method", "tools/call"}, {"params", {{"name", "typed"}, {"arguments", {{"path", 7}, {"count", "many"}}}}}});
    AETHER_CHECK(wrong_type["error"]["code"] == -32602);
    const Json correct_types = *server.HandleMessage({{"jsonrpc", "2.0"}, {"id", 4}, {"method", "tools/call"}, {"params", {{"name", "typed"}, {"arguments", {{"path", "a"}, {"count", 2}}}}}});
    AETHER_CHECK(correct_types["result"]["isError"] == false);
}

AETHER_TEST(Mcp_ListReportsTheSchemaVersionAndStdioSkipsHugeLines) {
    McpServer server("test", "0");
    const Json list = *server.HandleMessage({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/list"}});
    AETHER_CHECK(list["result"]["schemaVersion"] == kToolSchemaVersion && list["result"]["tools"].is_array());

    std::stringstream in;
    in << std::string(kMaxMessageBytes + 10, 'x') << "\n" << R"({"jsonrpc":"2.0","id":9,"method":"ping"})" << "\n";
    std::stringstream out;
    server.RunStdio(in, out);
    std::vector<Json> lines;
    std::string line;
    while (std::getline(out, line)) lines.push_back(Json::parse(line));
    AETHER_CHECK(lines.size() == 2);
    AETHER_CHECK(lines[0]["error"]["code"] == -32600 && lines[1]["id"] == 9 && lines[1].contains("result"));
}

AETHER_TEST(Mcp_ExposesEveryModuleComponent) {
    Harness h;
    Json types = h.Call("list_component_types").value;
    std::set<std::string> names;
    for (const Json& t : types) names.insert(t["name"].get<std::string>());
    for (const char* wanted : {"Transform", "Camera", "ParticleSystem", "AudioSource", "Animator", "NavAgent", "WidgetComponent",
                               "Rigidbody2D", "AbilityContainer", "StreamingSource", "BlueprintInstance", "SequenceComponent"}) {
        AETHER_CHECK(names.count(wanted) == 1);
    }
}

AETHER_TEST(Mcp_SystemsRunOnlyWhilePlaying) {
    Harness h;
    AETHER_CHECK(h.Call("list_systems").is_error);
    AETHER_CHECK(h.Call("step_simulation").is_error);

    std::string e = h.Create({{"Transform", {{"position", kPos123}}}});
    AETHER_CHECK(!h.Call("play").is_error);
    Json systems = h.Call("list_systems").value;
    AETHER_CHECK(systems.size() >= 2);
    AETHER_CHECK(systems[0]["enabled"] == true);

    Harness::Result stepped = h.Call("step_simulation", {{"frames", 30}});
    AETHER_CHECK(!stepped.is_error);
    AETHER_CHECK(stepped.value["frames_run"] == 30);
    AETHER_CHECK(stepped.value["fixed_steps"] == 30);

    AETHER_CHECK(h.Call("step_simulation", {{"frames", 0}}).is_error);
    AETHER_CHECK(h.Call("step_simulation", {{"dt", -1}}).is_error);
    AETHER_CHECK(h.Call("set_system_enabled", {{"system", "NoSuchSystem"}, {"enabled", false}}).is_error);
    AETHER_CHECK(!h.Call("set_system_enabled", {{"system", systems[0]["name"]}, {"enabled", false}}).is_error);
    AETHER_CHECK(h.Call("list_systems").value[0]["enabled"] == false);

    AETHER_CHECK(!h.Call("stop").is_error);
    AETHER_CHECK(h.Call("step_simulation").is_error);
    AETHER_CHECK(h.Components(e)["Transform"]["position"] == kPos123);
}

#if AETHER_MCP_PHYSICS
AETHER_TEST(Mcp_PhysicsFallsWhilePlayingAndStopRestores) {
    Harness h;
    std::string ball = h.Create({{"Transform", {{"position", {0, 10, 0}}}}, {"RigidBody", Json::object()}, {"SphereCollider", Json::object()}});
    h.Create({{"Transform", Json::object()}, {"RigidBody", {{"motion", "Static"}}}, {"BoxCollider", {{"half_extents", {20, 0.5, 20}}}}});
    h.Call("play");
    h.Call("step_simulation", {{"frames", 120}});
    const f32 fallen = h.Components(ball)["Transform"]["position"][1].get<f32>();
    AETHER_CHECK(fallen < 2.0f);
    AETHER_CHECK(fallen > 0.0f);
    h.Call("stop");
    AETHER_CHECK(h.Components(ball)["Transform"]["position"][1].get<f32>() == 10.0f);
}
#endif

#include "aether/assets/asset_guid.h"
#include "aether/pak/pak.h"
#include "game_tools.h"

AETHER_TEST(Mcp_GameToolsHostACookedGame) {
    namespace stdfs = std::filesystem;
    const stdfs::path dir = stdfs::temp_directory_path() / "aether_mcp_game_test";
    stdfs::remove_all(dir);
    stdfs::create_directories(dir);

    const Json transform = {{"position", {1, 2, 3}}, {"rotation", {0, 0, 0, 1}}, {"scale", {1, 1, 1}}};
    const Json manifest = {{"$type", "CookManifest"}, {"$version", 1}, {"project", "McpDemo"}, {"configuration", "Development"},
                           {"startup_scene", "Scenes/start.ascene"}, {"fixed_timestep_hz", 60.0}, {"gravity", {0.0, -9.81, 0.0}},
                           {"layers", {"Default"}}, {"collision_matrix", Json::array()},
                           {"assets", Json::array({{{"guid", assets::ToString(assets::NewAssetGuid())}, {"path", "Scenes/start.ascene"}, {"importer", "Scene"}}})},
                           {"files", Json::array()}};
    const Json scene = {{"$type", "Scene"}, {"$version", 1},
                        {"entities", Json::array({{{"components", {{"Transform", transform}, {"Tags", {{"names", {"Hero"}}}}}}}})}};
    pak::PakWriter writer;
    writer.Add("Manifest.json", manifest.dump());
    writer.Add("Content/Scenes/start.ascene", scene.dump());
    const stdfs::path file = dir / "game.apak";
    std::string error;
    AETHER_CHECK(writer.Write(file.string(), &error));

    Harness h;
    RegisterGameTools(h.server);
    AETHER_CHECK(h.Call("game_state").is_error); // nothing loaded yet
    AETHER_CHECK(h.Call("game_load", {{"paks", {(dir / "missing.apak").string()}}, {"user_dir", (dir / "user").string()}}).is_error);

    Harness::Result loaded = h.Call("game_load", {{"paks", {file.string()}}, {"user_dir", (dir / "user").string()}});
    AETHER_CHECK(!loaded.is_error);
    AETHER_CHECK(loaded.value["project"] == "McpDemo");
    AETHER_CHECK(loaded.value["playing"] == true);
    AETHER_CHECK(loaded.value["entities"] == 1);

    Harness::Result stepped = h.Call("game_step", {{"frames", 10}});
    AETHER_CHECK(!stepped.is_error);
    AETHER_CHECK(stepped.value["frames"] == 10);
    AETHER_CHECK(h.Call("game_step", {{"frames", 0}}).is_error);

    Json systems = h.Call("game_systems").value;
    AETHER_CHECK(systems.contains("FixedUpdate"));

    Harness::Result listed = h.Call("game_list_entities", {{"tag", "Hero"}});
    AETHER_CHECK(listed.value["total"] == 1);
    const std::string hero = listed.value["entities"][0]["entity"];
    AETHER_CHECK(h.Call("game_get_entity", {{"entity", hero}}).value["components"]["Transform"]["position"] == kPos123);

    Harness::Result set = h.Call("game_set_component", {{"entity", hero}, {"component", "Transform"}, {"values", {{"position", {9, 9, 9}}}}});
    AETHER_CHECK(!set.is_error);
    AETHER_CHECK(h.Call("game_get_entity", {{"entity", hero}}).value["components"]["Transform"]["position"] == Json::array({9, 9, 9}));
    AETHER_CHECK(h.Call("game_set_component", {{"entity", hero}, {"component", "Nope"}, {"values", Json::object()}}).is_error);
    AETHER_CHECK(h.Call("game_input", {{"set", {{"NoSuchKey", 1}}}}).is_error);

    AETHER_CHECK(!h.Call("game_unload").is_error);
    AETHER_CHECK(h.Call("game_state").is_error);
    stdfs::remove_all(dir);
}
