#include "editor_tools.h"
#include "mcp_server.h"
#include "test_framework.h"

#include <cmath>
#include <filesystem>
#include <map>
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

#include "build_tools.h"
#include "process.h"

#include <fstream>

AETHER_TEST(Mcp_RunProcessCapturesOutputAndKillsOnTimeout) {
    ProcessSpec echo;
    echo.command_line = "echo hello-from-child";
    ProcessResult ok = RunProcess(echo);
    AETHER_CHECK(ok.started);
    AETHER_CHECK(ok.exit_code == 0);
    AETHER_CHECK(ok.output.find("hello-from-child") != std::string::npos);

    ProcessSpec fail;
    fail.command_line = "exit 3";
    AETHER_CHECK(RunProcess(fail).exit_code == 3);

    ProcessSpec missing;
    missing.argv = {"definitely-not-a-real-program-xyz"};
    AETHER_CHECK(!RunProcess(missing).started || RunProcess(missing).exit_code != 0);

    ProcessSpec slow;
#ifdef _WIN32
    slow.command_line = "ping -n 30 127.0.0.1 >nul";
#else
    slow.command_line = "sleep 30";
#endif
    slow.timeout_ms = 300;
    ProcessResult killed = RunProcess(slow);
    AETHER_CHECK(killed.timed_out);
    AETHER_CHECK(killed.seconds < 10.0);
}

AETHER_TEST(Mcp_BuildToolsFindTheBuildAndRefuseEscapes) {
    namespace stdfs = std::filesystem;
    const stdfs::path dir = stdfs::temp_directory_path() / "aether_mcp_build_test";
    stdfs::remove_all(dir);
    stdfs::create_directories(dir / "sub" / "deeper");
    {
        std::ofstream cache(dir / "CMakeCache.txt");
        cache << "CMAKE_HOME_DIRECTORY:INTERNAL=/src/project\nCMAKE_GENERATOR:INTERNAL=Ninja\nAETHER_BUILD_MCP:BOOL=ON\n";
    }
    BuildContext found = FindBuildContext("", dir / "sub" / "deeper"); // walks up to the cache
    AETHER_CHECK(!found.build_dir.empty());
    AETHER_CHECK(found.source_dir == "/src/project");
    AETHER_CHECK(FindBuildContext((dir / "nope").string(), dir).build_dir.empty());

    Harness h;
    RegisterBuildTools(h.server, found);
    Harness::Result info = h.Call("build_info");
    AETHER_CHECK(!info.is_error);
    AETHER_CHECK(info.value["generator"] == "Ninja");
    AETHER_CHECK(info.value["options"]["AETHER_BUILD_MCP"] == "ON");

    AETHER_CHECK(h.Call("run_program", {{"program", "../../../Windows/System32/cmd"}}).is_error);
    AETHER_CHECK(h.Call("run_program", {{"program", "/bin/sh"}}).is_error);
    AETHER_CHECK(h.Call("run_program", {{"program", "missing_tool"}}).is_error);
    AETHER_CHECK(h.Call("build", {{"target", "--evil"}}).is_error);
    AETHER_CHECK(h.Call("build", {{"timeout_seconds", 0}}).is_error);
    AETHER_CHECK(h.Call("run_tests").is_error); // aether_tests isn't in this fake build

    Harness none;
    RegisterBuildTools(none.server, BuildContext{});
    AETHER_CHECK(none.Call("build").is_error);
    stdfs::remove_all(dir);
}

AETHER_TEST(Mcp_ToolsCanReturnImages) {
    McpServer server("test", "0");
    server.AddTool({"pic", "An image", {{"type", "object"}, {"properties", Json::object()}}, [](const Json&) -> Json {
                        return {{"note", "hi"}, {"mcp_content", Json::array({{{"type", "image"}, {"data", "AAAA"}, {"mimeType", "image/png"}}})}};
                    }});
    Json req = {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"}, {"params", {{"name", "pic"}, {"arguments", Json::object()}}}};
    Json resp = *server.HandleMessage(req);
    const Json& content = resp["result"]["content"];
    AETHER_CHECK(resp["result"]["isError"] == false);
    AETHER_CHECK(content.size() == 2);
    AETHER_CHECK(content[0]["type"] == "text");
    AETHER_CHECK(content[0]["text"].get<std::string>().find("mcp_content") == std::string::npos);
    AETHER_CHECK(content[1]["type"] == "image");
    AETHER_CHECK(content[1]["mimeType"] == "image/png");
}

#include "asset_tools.h"
#include "audio_tools.h"
#include "blueprint_tools.h"
#include "kit_tools.h"
#include "nav_tools.h"
#include "terrain_tools.h"
#include "vfx_tools.h"
#include "aether/project/project.h"

AETHER_TEST(Mcp_AssetToolsImportMoveAndCookAProject) {
    namespace stdfs = std::filesystem;
    const stdfs::path dir = stdfs::temp_directory_path() / "aether_mcp_asset_test";
    stdfs::remove_all(dir);
    stdfs::create_directories(dir);

    ProjectPaths paths;
    std::string error;
    AETHER_CHECK(CreateProject(dir, "McpAssets", &paths, &error));

    // A 2x2 uncompressed 32-bit TGA to import.
    const stdfs::path tga = dir / "swatch.tga";
    {
        std::ofstream out(tga, std::ios::binary);
        const unsigned char header[18] = {0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 2, 0, 32, 8};
        out.write(reinterpret_cast<const char*>(header), sizeof(header));
        const unsigned char pixels[16] = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255};
        out.write(reinterpret_cast<const char*>(pixels), sizeof(pixels));
    }

    Harness h;
    auto project = MakeAssetHost();
    RegisterAssetTools(h.server, project);
    RegisterKitTools(h.server, project);
    AETHER_CHECK(h.Call("asset_list").is_error); // no project open yet
    AETHER_CHECK(h.Call("project_open", {{"project_file", (dir / "nope.aproject").string()}}).is_error);

    Harness::Result opened = h.Call("project_open", {{"project_file", paths.file.string()}});
    AETHER_CHECK(!opened.is_error);
    AETHER_CHECK(opened.value["name"] == "McpAssets");

    // Paths cannot leave Content/.
    AETHER_CHECK(h.Call("content_write", {{"path", "../escape.json"}, {"text", "{}"}}).is_error);
    AETHER_CHECK(h.Call("content_write", {{"path", "Scenes/x.ameta"}, {"text", "{}"}}).is_error);
    AETHER_CHECK(h.Call("content_read", {{"path", "../../etc/passwd"}}).is_error);

    const Json scene = {{"$type", "Scene"}, {"$version", 1},
                        {"entities", Json::array({{{"components", {{"Transform", {{"position", {1, 2, 3}}, {"rotation", {0, 0, 0, 1}}, {"scale", {1, 1, 1}}}}}}}})}};
    Harness::Result wrote = h.Call("content_write", {{"path", "Scenes/start.ascene"}, {"text", scene.dump()}});
    AETHER_CHECK(!wrote.is_error);
    AETHER_CHECK(wrote.value["asset"]["importer"] == "Scene");
    AETHER_CHECK(h.Call("content_read", {{"path", "Scenes/start.ascene"}}).value["text"] == scene.dump());
    AETHER_CHECK(!h.Call("project_set", {{"startup_scene", "Scenes/start.ascene"}}).is_error);

    Harness::Result added = h.Call("asset_add_files", {{"sources", {tga.string()}}, {"folder", "Textures"}});
    AETHER_CHECK(!added.is_error);
    AETHER_CHECK(added.value["import"]["failed"] == 0);
    AETHER_CHECK(added.value["assets"][0]["importer"] == "Texture");
    AETHER_CHECK(h.Call("asset_add_files", {{"sources", {tga.string()}}, {"folder", "Textures"}}).is_error); // exists

    Json info = h.Call("asset_info", {{"asset", "Textures/swatch.tga"}}).value;
    AETHER_CHECK(info["needs_import"] == false);
    AETHER_CHECK(h.Call("asset_info", {{"asset", info["guid"]}}).value["path"] == "Textures/swatch.tga");
    AETHER_CHECK(h.Call("asset_list", {{"types", {"Texture"}}, {"recursive", true}}).value["assets"].size() == 1);

    AETHER_CHECK(!h.Call("asset_set_import_settings", {{"asset", "Textures/swatch.tga"}, {"settings", {{"mips", false}}}}).is_error);
    AETHER_CHECK(h.Call("asset_info", {{"asset", "Textures/swatch.tga"}}).value["settings"]["mips"] == false);

    AETHER_CHECK(!h.Call("asset_move", {{"from", "Textures/swatch.tga"}, {"to", "Textures/renamed.tga"}}).is_error);
    Json moved = h.Call("asset_info", {{"asset", "Textures/renamed.tga"}}).value;
    AETHER_CHECK(moved["guid"] == info["guid"]); // the GUID survives the move
    AETHER_CHECK(h.Call("asset_info", {{"asset", "Textures/swatch.tga"}}).is_error);

    Harness::Result cooked = h.Call("asset_cook", {{"output_dir", (dir / "Cooked").string()}, {"always_cook", {"Textures/"}}});
    AETHER_CHECK(!cooked.is_error);
    AETHER_CHECK(cooked.value["ok"] == true);
    AETHER_CHECK(cooked.value["assets_cooked"].get<int>() >= 2);
    AETHER_CHECK(stdfs::exists(cooked.value["pak_file"].get<std::string>()));

    // The cooked archive runs in the hosted game.
    RegisterGameTools(h.server);
    Harness::Result loaded = h.Call("game_load", {{"paks", {cooked.value["pak_file"]}}, {"user_dir", (dir / "user").string()}});
    AETHER_CHECK(!loaded.is_error);
    AETHER_CHECK(loaded.value["entities"] == 1);
    h.Call("game_unload");

    AETHER_CHECK(!h.Call("asset_delete", {{"asset", "Textures/renamed.tga"}}).is_error);
    AETHER_CHECK(h.Call("asset_info", {{"asset", "Textures/renamed.tga"}}).is_error);
    AETHER_CHECK(h.Call("asset_cook", {{"output_dir", (dir / "Cooked2").string()}, {"configuration", "bogus"}}).is_error);
    stdfs::remove_all(dir);
}

AETHER_TEST(Mcp_KitToolsValidateWriteAndCrossCheckDefinitions) {
    namespace stdfs = std::filesystem;
    const stdfs::path dir = stdfs::temp_directory_path() / "aether_mcp_kit_test";
    stdfs::remove_all(dir);
    stdfs::create_directories(dir);
    ProjectPaths paths;
    std::string error;
    AETHER_CHECK(CreateProject(dir, "McpKits", &paths, &error));

    Harness h;
    auto project = MakeAssetHost();
    RegisterAssetTools(h.server, project);
    RegisterKitTools(h.server, project);
    AETHER_CHECK(h.Call("kit_list").is_error); // no project open
    AETHER_CHECK(!h.Call("project_open", {{"project_file", paths.file.string()}}).is_error);

    // Every type's sample is a definition its own kit accepts.
    Json types = h.Call("kit_types").value;
    AETHER_CHECK(types.size() >= 4);
    for (const Json& t : types) {
        Json schema = h.Call("kit_schema", {{"type", t["type"]}}).value;
        AETHER_CHECK(schema.contains("notes"));
        if (schema.contains("sample")) {
            Json verdict = h.Call("kit_validate", {{"type", t["type"]}, {"definition", schema["sample"]}}).value;
            AETHER_CHECK(verdict["ok"] == true);
        }
    }
    AETHER_CHECK(h.Call("kit_schema", {{"type", "nonsense"}}).is_error);

    Json bad = h.Call("kit_validate", {{"type", "effect"}, {"definition", {{"name", ""}}}}).value;
    AETHER_CHECK(bad["ok"] == false);
    AETHER_CHECK(bad["error"].get<std::string>().find("effect.name_empty") != std::string::npos);

    const Json heal = {{"name", "Heal"}, {"duration_policy", "instant"}, {"modifiers", {{{"attribute", "Health"}, {"op", "add"}, {"magnitude", 25}}}}};
    Harness::Result put = h.Call("kit_put", {{"type", "effect"}, {"path", "Gameplay/Heal"}, {"definition", heal}});
    AETHER_CHECK(!put.is_error);
    AETHER_CHECK(put.value["path"] == "Gameplay/Heal.aeffect");
    AETHER_CHECK(stdfs::exists(paths.content / "Gameplay" / "Heal.aeffect"));
    AETHER_CHECK(h.Call("kit_put", {{"type", "effect"}, {"path", "Gameplay/Heal"}, {"definition", heal}}).is_error); // exists
    AETHER_CHECK(!h.Call("kit_put", {{"type", "effect"}, {"path", "Gameplay/Heal"}, {"definition", heal}, {"overwrite", true}}).is_error);

    // Invalid definitions are refused and nothing is written.
    AETHER_CHECK(h.Call("kit_put", {{"type", "effect"}, {"path", "Gameplay/Bad"}, {"definition", {{"name", "Bad"}, {"duration_policy", "Instant"}}}}).is_error);
    AETHER_CHECK(!stdfs::exists(paths.content / "Gameplay" / "Bad.aeffect"));
    AETHER_CHECK(h.Call("kit_put", {{"type", "effect"}, {"path", "../Escape"}, {"definition", heal}}).is_error);

    AETHER_CHECK(!h.Call("kit_put", {{"type", "ability"}, {"path", "Gameplay/Potion"}, {"definition", {{"name", "Potion"}, {"cost", "Heal"}, {"cooldown", "Missing"}}}}).is_error);
    Json got = h.Call("kit_get", {{"asset", "Gameplay/Heal.aeffect"}}).value;
    AETHER_CHECK(got["type"] == "effect");
    AETHER_CHECK(got["definition"]["name"] == "Heal");

    Json check = h.Call("kit_check").value;
    AETHER_CHECK(check["ok"] == false);
    AETHER_CHECK(check["problems"].size() == 1);
    AETHER_CHECK(check["problems"][0]["problem"].get<std::string>().find("Missing") != std::string::npos);

    // A hand-edited, broken file shows up as invalid.
    AETHER_CHECK(!h.Call("content_write", {{"path", "Gameplay/Broken.aeffect"}, {"text", "{ not json"}}).is_error);
    Json listed = h.Call("kit_list", {{"type", "effect"}}).value;
    AETHER_CHECK(listed.size() == 2);
    unsigned invalid = 0;
    for (const Json& row : listed) invalid += row["valid"] == false ? 1 : 0;
    AETHER_CHECK(invalid == 1);
    stdfs::remove_all(dir);
}

AETHER_TEST(Mcp_AttributeToolsEditUndoAndCheckTheProject) {
    namespace stdfs = std::filesystem;
    Harness h;
    std::string hero = h.Create({{"Transform", Json::object()}});
    AETHER_CHECK(h.Call("attribute_list", {{"entity", hero}}).value["attributes"].empty());

    Harness::Result defined = h.Call("attribute_define", {{"entity", hero}, {"attributes", {{{"name", "Mana"}, {"base", 50}, {"min", 0}, {"max", 100}},
                                                                                                   {{"name", "Health"}, {"base", 150}, {"min", 0}, {"max", 100}}}}});
    AETHER_CHECK(!defined.is_error);
    Json attrs = h.Call("attribute_list", {{"entity", hero}}).value["attributes"];
    AETHER_CHECK(attrs.size() == 2);
    AETHER_CHECK(attrs[0]["name"] == "Health"); // sorted by name
    AETHER_CHECK(attrs[0]["base"] == 100);      // clamped into the bounds
    AETHER_CHECK(attrs[1]["base"] == 50);

    // Redefine one, undo the whole step, redo.
    AETHER_CHECK(!h.Call("attribute_define", {{"entity", hero}, {"attributes", {{{"name", "Mana"}, {"base", 75}, {"min", 0}, {"max", 100}}}}}).is_error);
    AETHER_CHECK(h.Call("attribute_list", {{"entity", hero}}).value["attributes"][1]["base"] == 75);
    h.Call("undo");
    AETHER_CHECK(h.Call("attribute_list", {{"entity", hero}}).value["attributes"][1]["base"] == 50);
    h.Call("redo");
    AETHER_CHECK(h.Call("attribute_list", {{"entity", hero}}).value["attributes"][1]["base"] == 75);

    AETHER_CHECK(!h.Call("attribute_remove", {{"entity", hero}, {"names", {"Mana"}}}).is_error);
    AETHER_CHECK(h.Call("attribute_list", {{"entity", hero}}).value["attributes"].size() == 1);
    AETHER_CHECK(h.Call("attribute_remove", {{"entity", hero}, {"names", {"Mana"}}}).is_error); // already gone
    AETHER_CHECK(h.Call("attribute_define", {{"entity", hero}, {"attributes", Json::array()}}).is_error);
    AETHER_CHECK(h.Call("attribute_define", {{"entity", hero}, {"attributes", {{{"name", "Bad"}, {"base", "text"}}}}}).is_error);
    // A whole add-component-and-define is one undo step on a fresh entity.
    std::string other = h.Create({{"Transform", Json::object()}});
    AETHER_CHECK(!h.Call("attribute_define", {{"entity", other}, {"attributes", {{{"name", "Stamina"}, {"base", 10}}}}}).is_error);
    h.Call("undo");
    AETHER_CHECK(!h.Components(other).contains("AttributeSet"));

    // The project's scenes define attributes; kit_check warns about effects that modify unknown ones.
    const stdfs::path dir = stdfs::temp_directory_path() / "aether_mcp_attr_test";
    stdfs::remove_all(dir);
    stdfs::create_directories(dir);
    ProjectPaths paths;
    std::string error;
    AETHER_CHECK(CreateProject(dir, "McpAttrs", &paths, &error));
    auto project = MakeAssetHost();
    RegisterAssetTools(h.server, project);
    RegisterKitTools(h.server, project);
    AETHER_CHECK(!h.Call("project_open", {{"project_file", paths.file.string()}}).is_error);
    const Json scene = {{"$type", "Scene"}, {"$version", 1},
                        {"entities", Json::array({{{"components", {{"AttributeSet", {{"attributes", {{{"name", "Health"}, {"base", 100}, {"min", 0}, {"max", 100}}}}}}}}}})}};
    AETHER_CHECK(!h.Call("content_write", {{"path", "Scenes/start.ascene"}, {"text", scene.dump()}}).is_error);
    Json listed = h.Call("attribute_project").value;
    AETHER_CHECK(listed["attributes"].size() == 1);
    AETHER_CHECK(listed["attributes"][0]["name"] == "Health");
    AETHER_CHECK(listed["attributes"][0]["defined_in"][0] == "Scenes/start.ascene");
    AETHER_CHECK(listed["warnings"].size() == 1); // saved without "current"
    AETHER_CHECK(listed["warnings"][0]["warning"].get<std::string>().find("current") != std::string::npos);

    auto effect = [](const char* name, const char* attribute) {
        return Json{{"name", name}, {"duration_policy", "instant"}, {"modifiers", {{{"attribute", attribute}, {"op", "add"}, {"magnitude", 1}}}}};
    };
    AETHER_CHECK(!h.Call("kit_put", {{"type", "effect"}, {"path", "Fx/Heal"}, {"definition", effect("Heal", "Health")}}).is_error);
    AETHER_CHECK(!h.Call("kit_put", {{"type", "effect"}, {"path", "Fx/Drain"}, {"definition", effect("Drain", "Sanity")}}).is_error);
    Json check = h.Call("kit_check").value;
    AETHER_CHECK(check["ok"] == true); // a warning, not a problem
    AETHER_CHECK(check["warnings"].size() == 1);
    AETHER_CHECK(check["warnings"][0]["warning"].get<std::string>().find("Sanity") != std::string::npos);
    stdfs::remove_all(dir);
}

AETHER_TEST(Mcp_GameAttributesReadAndChangeLiveValues) {
    namespace stdfs = std::filesystem;
    const stdfs::path dir = stdfs::temp_directory_path() / "aether_mcp_game_attr_test";
    stdfs::remove_all(dir);
    stdfs::create_directories(dir);
    const Json manifest = {{"$type", "CookManifest"}, {"$version", 1}, {"project", "McpAttr"}, {"configuration", "Development"},
                           {"startup_scene", "Scenes/start.ascene"}, {"fixed_timestep_hz", 60.0}, {"gravity", {0.0, -9.81, 0.0}},
                           {"layers", {"Default"}}, {"collision_matrix", Json::array()},
                           {"assets", Json::array({{{"guid", assets::ToString(assets::NewAssetGuid())}, {"path", "Scenes/start.ascene"}, {"importer", "Scene"}}})},
                           {"files", Json::array()}};
    const Json scene = {{"$type", "Scene"}, {"$version", 1},
                        {"entities", Json::array({{{"components", {{"AttributeSet", {{"attributes", {{{"name", "Health"}, {"base", 80}, {"current", 80}, {"min", 0}, {"max", 100}}}}}}}}}})}};
    pak::PakWriter writer;
    writer.Add("Manifest.json", manifest.dump());
    writer.Add("Content/Scenes/start.ascene", scene.dump());
    std::string error;
    AETHER_CHECK(writer.Write((dir / "game.apak").string(), &error));

    Harness h;
    RegisterGameTools(h.server);
    AETHER_CHECK(!h.Call("game_load", {{"paks", {(dir / "game.apak").string()}}, {"user_dir", (dir / "user").string()}}).is_error);
    h.Call("game_step", {{"frames", 2}});
    std::string who = h.Call("game_list_entities", {{"component", "AttributeSet"}}).value["entities"][0]["entity"];

    Json before = h.Call("game_attributes", {{"entity", who}}).value["attributes"];
    AETHER_CHECK(before.size() == 1);
    AETHER_CHECK(before[0]["current"] == 80);

    Harness::Result set = h.Call("game_set_attribute", {{"entity", who}, {"name", "Health"}, {"delta", -30}});
    AETHER_CHECK(!set.is_error);
    AETHER_CHECK(set.value["base"] == 50);
    AETHER_CHECK(h.Call("game_set_attribute", {{"entity", who}, {"name", "Health"}, {"base", 500}}).value["base"] == 100); // clamped
    AETHER_CHECK(!h.Call("game_set_attribute", {{"entity", who}, {"name", "Mana"}, {"define", {{"base", 5}, {"min", 0}, {"max", 10}}}}).is_error);
    AETHER_CHECK(h.Call("game_attributes", {{"entity", who}}).value["attributes"].size() == 2);
    AETHER_CHECK(h.Call("game_set_attribute", {{"entity", who}, {"name", "Nope"}, {"base", 1}}).is_error);
    AETHER_CHECK(h.Call("game_set_attribute", {{"entity", who}, {"name", "Health"}}).is_error); // needs one mode
    h.Call("game_unload");
    stdfs::remove_all(dir);
}

#if AETHER_MCP_INTERACTION
AETHER_TEST(Mcp_InteractableToolsEditCheckAndDriveTheGame) {
    namespace stdfs = std::filesystem;
    // --- the editor scene: validated, undoable edits
    Harness h;
    std::string door = h.Create({{"Transform", Json::object()}});
    AETHER_CHECK(h.Call("interactable_list").value.empty());
    AETHER_CHECK(h.Call("interactable_set", {{"entity", door}, {"fields", {{"range", -1}}}}).is_error);
    AETHER_CHECK(h.Call("interactable_set", {{"entity", door}, {"fields", {{"required_tags", {"not a tag"}}}}}).is_error);
    AETHER_CHECK(h.Call("interactable_set", {{"entity", door}, {"fields", {{"colour", "red"}}}}).is_error);
    AETHER_CHECK(h.Call("interactable_set", {{"entity", door}, {"fields", {{"used", true}}}}).is_error);
    AETHER_CHECK(!h.Components(door).contains("Interactable")); // a refused edit leaves nothing behind

    Harness::Result set = h.Call("interactable_set", {{"entity", door}, {"fields", {{"prompt", "Open"}, {"range", 2.5}, {"one_shot", true}, {"required_tags", {"Item.Key"}}}}});
    AETHER_CHECK(!set.is_error);
    AETHER_CHECK(set.value["interactable"]["prompt"] == "Open");
    AETHER_CHECK(set.value["interactable"]["one_shot"] == true);
    AETHER_CHECK(!h.Call("interactable_set", {{"entity", door}, {"fields", {{"cooldown", 3}}}}).is_error); // partial update
    AETHER_CHECK(h.Components(door)["Interactable"]["prompt"] == "Open");
    AETHER_CHECK(h.Components(door)["Interactable"]["cooldown"] == 3);
    h.Call("undo");
    AETHER_CHECK(h.Components(door)["Interactable"]["cooldown"] == 0);
    h.Call("undo");
    AETHER_CHECK(!h.Components(door).contains("Interactable")); // add + set were one step
    h.Call("redo");
    AETHER_CHECK(h.Call("interactable_list").value.size() == 1);
    AETHER_CHECK(!h.Call("interactable_remove", {{"entity", door}}).is_error);
    AETHER_CHECK(h.Call("interactable_remove", {{"entity", door}}).is_error);

    // --- the project: references to effects and abilities are checked
    const stdfs::path dir = stdfs::temp_directory_path() / "aether_mcp_interact_test";
    stdfs::remove_all(dir);
    stdfs::create_directories(dir);
    ProjectPaths paths;
    std::string error;
    AETHER_CHECK(CreateProject(dir, "McpInteract", &paths, &error));
    auto project = MakeAssetHost();
    RegisterAssetTools(h.server, project);
    RegisterKitTools(h.server, project);
    AETHER_CHECK(!h.Call("project_open", {{"project_file", paths.file.string()}}).is_error);
    const Json project_scene = {{"$type", "Scene"}, {"$version", 1},
                                {"entities", Json::array({{{"components", {{"Interactable", {{"prompt", "Drink"}, {"effect", "Heal"}, {"ability", "Missing"}}}}}}})}};
    AETHER_CHECK(!h.Call("content_write", {{"path", "Scenes/start.ascene"}, {"text", project_scene.dump()}}).is_error);
    AETHER_CHECK(h.Call("interaction_project").value["interactables"].size() == 1);
    AETHER_CHECK(!h.Call("kit_put", {{"type", "effect"}, {"path", "Fx/Heal"}, {"definition", {{"name", "Heal"}, {"duration_policy", "instant"}}}}).is_error);
    Json check = h.Call("kit_check").value;
    AETHER_CHECK(check["ok"] == false);
    AETHER_CHECK(check["problems"].size() == 1); // Heal exists, the ability does not
    AETHER_CHECK(check["problems"][0]["problem"].get<std::string>().find("Missing") != std::string::npos);
    stdfs::remove_all(dir);

    // --- the running game
    const stdfs::path gdir = stdfs::temp_directory_path() / "aether_mcp_interact_game_test";
    stdfs::remove_all(gdir);
    stdfs::create_directories(gdir);
    const Json manifest = {{"$type", "CookManifest"}, {"$version", 1}, {"project", "McpInteractGame"}, {"configuration", "Development"},
                           {"startup_scene", "Scenes/start.ascene"}, {"fixed_timestep_hz", 60.0}, {"gravity", {0.0, -9.81, 0.0}},
                           {"layers", {"Default"}}, {"collision_matrix", Json::array()},
                           {"assets", Json::array({{{"guid", assets::ToString(assets::NewAssetGuid())}, {"path", "Scenes/start.ascene"}, {"importer", "Scene"}}})},
                           {"files", Json::array()}};
    auto transform_at = [](double x) { return Json{{"position", {x, 0, 0}}, {"rotation", {0, 0, 0, 1}}, {"scale", {1, 1, 1}}}; };
    const Json scene = {{"$type", "Scene"}, {"$version", 1},
                        {"entities", Json::array({
                            {{"components", {{"EntityName", {{"value", "Player"}}}, {"Transform", transform_at(0)}}}},
                            {{"components", {{"EntityName", {{"value", "Lever"}}}, {"Transform", transform_at(1)}, {"Interactable", {{"prompt", "Pull"}, {"range", 2}, {"one_shot", true}}}}}},
                            {{"components", {{"EntityName", {{"value", "Chest"}}}, {"Transform", transform_at(50)}, {"Interactable", {{"prompt", "Open"}, {"range", 2}}}}}},
                            {{"components", {{"EntityName", {{"value", "Vault"}}}, {"Transform", transform_at(1)}, {"Interactable", {{"prompt", "Unlock"}, {"range", 2}, {"required_tags", {"Item.Key"}}}}}}}})}};
    pak::PakWriter writer;
    writer.Add("Manifest.json", manifest.dump());
    writer.Add("Content/Scenes/start.ascene", scene.dump());
    AETHER_CHECK(writer.Write((gdir / "game.apak").string(), &error));

    RegisterGameTools(h.server);
    AETHER_CHECK(!h.Call("game_load", {{"paks", {(gdir / "game.apak").string()}}, {"user_dir", (gdir / "user").string()}}).is_error);
    h.Call("game_step", {{"frames", 2}});
    std::map<std::string, std::string> who;
    const Harness::Result everyone = h.Call("game_list_entities"); // kept alive: the loop reads into it
    for (const Json& e : everyone.value["entities"]) {
        if (e.contains("name")) who[e["name"]] = e["entity"];
    }
    AETHER_CHECK(who.size() == 4);

    Json rows = h.Call("game_interactables", {{"interactor", who["Player"]}}).value;
    AETHER_CHECK(rows.size() == 3);
    std::map<std::string, std::string> reasons;
    for (const Json& r : rows) reasons[r["name"]] = r["reason"];
    AETHER_CHECK(reasons["Lever"] == "none");
    AETHER_CHECK(reasons["Chest"] == "out_of_range");
    AETHER_CHECK(reasons["Vault"] == "missing_tag");

    AETHER_CHECK(h.Call("game_can_interact", {{"interactor", who["Player"]}, {"target", who["Lever"]}}).value["can_interact"] == true);
    Json focus = h.Call("game_interaction_focus", {{"interactor", who["Player"]}}).value;
    AETHER_CHECK(focus["target"] == who["Lever"]);
    AETHER_CHECK(focus["prompt"] == "Pull");

    Harness::Result pulled = h.Call("game_interact", {{"interactor", who["Player"]}, {"target", who["Lever"]}});
    AETHER_CHECK(pulled.value["interacted"] == true);
    Json again = h.Call("game_interact", {{"interactor", who["Player"]}, {"target", who["Lever"]}}).value;
    AETHER_CHECK(again["interacted"] == false);
    AETHER_CHECK(again["reason"] == "used"); // one shot
    AETHER_CHECK(h.Call("game_interact", {{"interactor", who["Player"]}, {"target", who["Chest"]}}).value["reason"] == "out_of_range");

    AETHER_CHECK(!h.Call("game_set_interactable", {{"target", who["Lever"]}, {"reset", true}}).is_error);
    AETHER_CHECK(h.Call("game_can_interact", {{"interactor", who["Player"]}, {"target", who["Lever"]}}).value["can_interact"] == true);
    AETHER_CHECK(!h.Call("game_set_interactable", {{"target", who["Lever"]}, {"enabled", false}}).is_error);
    AETHER_CHECK(h.Call("game_can_interact", {{"interactor", who["Player"]}, {"target", who["Lever"]}}).value["reason"] == "disabled");
    AETHER_CHECK(h.Call("game_set_interactable", {{"target", who["Player"]}, {"enabled", true}}).is_error); // not an interactable
    AETHER_CHECK(h.Call("game_set_interactable", {{"target", who["Lever"]}}).is_error);
    h.Call("game_unload");
    stdfs::remove_all(gdir);
}
#endif

AETHER_TEST(Mcp_AudioToolsEditCheckPreviewAndDriveTheGame) {
    namespace stdfs = std::filesystem;
    // --- the editor scene: validated, undoable audio components
    Harness h;
    std::string speaker = h.Create({{"Transform", Json::object()}});
    AETHER_CHECK(h.Call("audio_set", {{"entity", speaker}, {"component", "Transform"}, {"fields", Json::object()}}).is_error);
    AETHER_CHECK(h.Call("audio_set", {{"entity", speaker}, {"component", "AudioSource"}, {"fields", {{"pitch", 0}}}}).is_error);
    AETHER_CHECK(h.Call("audio_set", {{"entity", speaker}, {"component", "AudioSource"}, {"fields", {{"playing", true}}}}).is_error);
    AETHER_CHECK(h.Call("audio_set", {{"entity", speaker}, {"component", "ReverbZone"}, {"fields", {{"wet", 2}}}}).is_error);
    AETHER_CHECK(h.Call("audio_set", {{"entity", speaker}, {"component", "ReverbZone"}, {"fields", {{"loudness", 1}}}}).is_error);
    AETHER_CHECK(!h.Components(speaker).contains("AudioSource")); // a refused edit leaves nothing behind

    AETHER_CHECK(!h.Call("audio_set", {{"entity", speaker}, {"component", "AudioSource"}, {"fields", {{"cue", "Audio/blip.acue"}, {"volume_db", -6}, {"auto_play", false}}}}).is_error);
    AETHER_CHECK(h.Components(speaker)["AudioSource"]["cue"] == "Audio/blip.acue");
    AETHER_CHECK(h.Components(speaker)["AudioSource"]["auto_play"] == false);
    AETHER_CHECK(!h.Call("audio_set", {{"entity", speaker}, {"component", "ReverbZone"}, {"fields", {{"radius", 5}, {"wet", 0.5}}}}).is_error);
    AETHER_CHECK(!h.Call("audio_set", {{"entity", speaker}, {"component", "AudioListener"}, {"fields", Json::object()}}).is_error); // just add it
    AETHER_CHECK(h.Call("audio_list").value.size() == 1);
    h.Call("undo");
    AETHER_CHECK(!h.Components(speaker).contains("AudioListener"));
    h.Call("undo");
    h.Call("undo");
    AETHER_CHECK(!h.Components(speaker).contains("AudioSource")); // add + set were one step
    h.Call("redo");
    AETHER_CHECK(!h.Call("audio_remove", {{"entity", speaker}, {"component", "AudioSource"}}).is_error);
    AETHER_CHECK(h.Call("audio_remove", {{"entity", speaker}, {"component", "AudioSource"}}).is_error);

    // --- cues work without a project, too
    const Json random_cue = {{"version", 1}, {"name", "pick"}, {"root", 1},
                             {"output", {{"bus", "SFX"}}},
                             {"nodes", Json::array({{{"id", 1}, {"type", "Random"}, {"children", {2, 3}}},
                                                    {{"id", 2}, {"type", "Wave"}, {"sound", "Audio/a.wav"}},
                                                    {{"id", 3}, {"type", "Wave"}, {"sound", "Audio/b.wav"}}})}};
    auto auto_project = MakeAssetHost();
    RegisterAssetTools(h.server, auto_project);
    RegisterKitTools(h.server, auto_project);
    RegisterAudioTools(h.server, auto_project);
    Json verdict = h.Call("cue_validate", {{"definition", random_cue}}).value;
    AETHER_CHECK(verdict["ok"] == true);
    AETHER_CHECK(verdict["sounds_checked_against_project"] == false);
    Json preview = h.Call("cue_preview", {{"definition", random_cue}, {"plays", 6}, {"seed", 7}}).value;
    AETHER_CHECK(preview["plays"].size() == 6);
    std::set<std::string> picked;
    for (const Json& run : preview["plays"]) {
        AETHER_CHECK(run["items"].size() == 1);
        picked.insert(run["items"][0]["sound"].get<std::string>());
    }
    AETHER_CHECK(picked.size() == 2); // both sounds came up over six plays
    Json again = h.Call("cue_preview", {{"definition", random_cue}, {"plays", 6}, {"seed", 7}}).value;
    AETHER_CHECK(again["plays"] == preview["plays"]); // the seed makes it repeatable
    AETHER_CHECK(h.Call("cue_validate", {{"definition", {{"nodes", Json::array()}}}}).value["ok"] == false); // no output
    AETHER_CHECK(h.Call("cue_preview", {{"definition", random_cue}, {"plays", 0}}).is_error);
    AETHER_CHECK(h.Call("cue_validate").is_error);
    AETHER_CHECK(h.Call("cue_validate", {{"cue", "Audio/x.acue"}}).is_error); // needs a project for that

    // --- a project with a real sound, a cue and a scene that uses them
    const stdfs::path dir = stdfs::temp_directory_path() / "aether_mcp_audio_test";
    stdfs::remove_all(dir);
    stdfs::create_directories(dir);
    ProjectPaths paths;
    std::string error;
    AETHER_CHECK(CreateProject(dir, "McpAudio", &paths, &error));
    {
        // A 0.1 s, 8 kHz, 16-bit mono WAV.
        const stdfs::path wav = dir / "blip.wav";
        std::ofstream out(wav, std::ios::binary);
        const unsigned data_bytes = 800 * 2;
        auto u32le = [&](unsigned v) { const char b[4] = {char(v & 255), char((v >> 8) & 255), char((v >> 16) & 255), char((v >> 24) & 255)}; out.write(b, 4); };
        auto u16le = [&](unsigned v) { const char b[2] = {char(v & 255), char((v >> 8) & 255)}; out.write(b, 2); };
        out.write("RIFF", 4); u32le(36 + data_bytes); out.write("WAVEfmt ", 8);
        u32le(16); u16le(1); u16le(1); u32le(8000); u32le(16000); u16le(2); u16le(16);
        out.write("data", 4); u32le(data_bytes);
        for (int i = 0; i < 800; ++i) u16le(static_cast<unsigned>(static_cast<int>(8000.0 * std::sin(i * 0.3)) & 0xFFFF));
    }
    AETHER_CHECK(!h.Call("project_open", {{"project_file", paths.file.string()}}).is_error);
    AETHER_CHECK(!h.Call("asset_add_files", {{"sources", {(dir / "blip.wav").string()}}, {"folder", "Audio"}}).is_error);

    const Json blip = {{"version", 1}, {"name", "blip"}, {"root", 1}, {"output", {{"bus", "SFX"}, {"volume_db", -6}, {"spatial", false}}},
                       {"nodes", Json::array({{{"id", 1}, {"type", "Wave"}, {"sound", "Audio/blip.wav"}}})}};
    AETHER_CHECK(!h.Call("kit_put", {{"type", "sound_cue"}, {"path", "Audio/blip"}, {"definition", blip}}).is_error);
    AETHER_CHECK(h.Call("kit_schema", {{"type", "sound_cue"}}).value.contains("sample"));
    Json sample = h.Call("kit_schema", {{"type", "sound_cue"}}).value["sample"];
    AETHER_CHECK(h.Call("kit_validate", {{"type", "sound_cue"}, {"definition", sample}}).value["ok"] == true);
    AETHER_CHECK(h.Call("kit_put", {{"type", "sound_cue"}, {"path", "Audio/bad"}, {"definition", {{"nodes", Json::array()}}}}).is_error);

    Json by_asset = h.Call("cue_validate", {{"cue", "Audio/blip.acue"}}).value;
    AETHER_CHECK(by_asset["ok"] == true);
    AETHER_CHECK(by_asset["sounds_checked_against_project"] == true);
    // The preview reads the real length of the project's .wav (0.1 s), where it assumes one for sounds it cannot read.
    Json real = h.Call("cue_preview", {{"cue", "Audio/blip.acue"}, {"plays", 1}}).value;
    AETHER_CHECK(real["assumed_length_sounds"].empty());
    AETHER_CHECK(std::fabs(real["sound_lengths"]["Audio/blip.wav"].get<double>() - 0.1) < 1e-6);
    AETHER_CHECK(std::fabs(real["plays"][0]["duration"].get<double>() - 0.1) < 1e-6);
    Json missing = h.Call("cue_validate", {{"definition", random_cue}}).value; // a.wav and b.wav are not in this project
    AETHER_CHECK(missing["ok"] == false);
    AETHER_CHECK(missing["diagnostics"][0]["code"] == "CU007");

    // kit_check: the Random cue's missing sounds, and a scene naming a cue that is not there.
    AETHER_CHECK(!h.Call("kit_put", {{"type", "sound_cue"}, {"path", "Audio/pick"}, {"definition", random_cue}}).is_error);
    auto scene_with = [](const char* cue) {
        return Json{{"$type", "Scene"}, {"$version", 1},
                    {"entities", Json::array({{{"components", {{"Transform", {{"position", {0, 0, 0}}, {"rotation", {0, 0, 0, 1}}, {"scale", {1, 1, 1}}}},
                                                                  {"AudioListener", {{"active", true}}},
                                                                  {"AudioSource", {{"cue", cue}, {"auto_play", false}}}}}}})}};
    };
    AETHER_CHECK(!h.Call("content_write", {{"path", "Scenes/start.ascene"}, {"text", scene_with("Audio/nothing.acue").dump()}}).is_error);
    Json check = h.Call("kit_check").value;
    AETHER_CHECK(check["ok"] == false);
    unsigned cue_problems = 0, source_problems = 0;
    for (const Json& pr : check["problems"]) {
        const std::string text = pr["problem"];
        if (text.find("CU007") != std::string::npos) ++cue_problems;
        if (text.find("AudioSource's cue") != std::string::npos) ++source_problems;
    }
    AETHER_CHECK(cue_problems == 2);   // pick.acue's a.wav and b.wav
    AETHER_CHECK(source_problems == 1);

    AETHER_CHECK(!h.Call("asset_delete", {{"asset", "Audio/pick.acue"}}).is_error);
    AETHER_CHECK(!h.Call("content_write", {{"path", "Scenes/start.ascene"}, {"text", scene_with("Audio/blip.acue").dump()}}).is_error);
    AETHER_CHECK(h.Call("kit_check").value["ok"] == true);

    // --- cook it and run the audio in the hosted game
    AETHER_CHECK(!h.Call("project_set", {{"startup_scene", "Scenes/start.ascene"}}).is_error);
    Harness::Result cooked = h.Call("asset_cook", {{"output_dir", (dir / "Cooked").string()}, {"always_cook", {"Audio/"}}});
    AETHER_CHECK(!cooked.is_error);
    AETHER_CHECK(cooked.value["ok"] == true);
    RegisterGameTools(h.server);
    AETHER_CHECK(h.Call("game_audio_state").is_error); // no game yet
    AETHER_CHECK(!h.Call("game_load", {{"paks", {cooked.value["pak_file"]}}, {"user_dir", (dir / "user").string()}}).is_error);
    h.Call("game_step", {{"frames", 3}});

    Json state = h.Call("game_audio_state").value;
    AETHER_CHECK(state["buses"].size() >= 2);
    AETHER_CHECK(state["listener"].is_string());
    AETHER_CHECK(state["problems"].empty());

    AETHER_CHECK(h.Call("game_play_sound", {{"cue", "Audio/blip.acue"}}).value["played"] == true);
    AETHER_CHECK(h.Call("game_audio_state").value["cues_playing"].get<int>() >= 1);
    AETHER_CHECK(h.Call("game_play_sound", {{"cue", "Audio/blip.acue"}, {"location", {1, 0, 0}}, {"volume_db", -3}}).value["played"] == true);
    Json nothing = h.Call("game_play_sound", {{"cue", "Audio/ghost.acue"}}).value;
    AETHER_CHECK(nothing["played"] == false);
    AETHER_CHECK(!nothing["problems"].empty()); // the missing cue is reported
    AETHER_CHECK(h.Call("game_play_sound", {{"cue", "Audio/blip.acue"}, {"pitch", 0}}).is_error);
    AETHER_CHECK(h.Call("game_play_sound", {{"cue", "Audio/blip.acue"}, {"location", {1, 2}}}).is_error);

    Json bus = h.Call("game_set_bus", {{"bus", "SFX"}, {"volume_db", -12}}).value;
    AETHER_CHECK(bus["volume_db"] == -12);
    AETHER_CHECK(h.Call("game_set_bus", {{"bus", "SFX"}, {"muted", true}}).value["muted"] == true);
    AETHER_CHECK(h.Call("game_set_bus", {{"bus", "NoSuchBus"}, {"volume_db", 0}}).is_error);
    AETHER_CHECK(h.Call("game_set_bus", {{"bus", "SFX"}}).is_error);
    AETHER_CHECK(!h.Call("game_stop_sounds").is_error);
    AETHER_CHECK(h.Call("game_audio_state").value["cues_playing"] == 0);

    const Harness::Result everyone = h.Call("game_list_entities", {{"component", "AudioSource"}});
    const std::string source = everyone.value["entities"][0]["entity"];
    AETHER_CHECK(!h.Call("game_audio_source", {{"entity", source}, {"action", "play"}}).is_error);
    AETHER_CHECK(!h.Call("game_audio_source", {{"entity", source}, {"action", "set_volume"}, {"value", -9}}).is_error);
    AETHER_CHECK(h.Call("game_audio_source", {{"entity", source}, {"action", "explode"}}).is_error);
    h.Call("game_step", {{"frames", 2}});
    AETHER_CHECK(h.Call("game_audio_source", {{"entity", source}, {"action", "stop"}}).value["volume_db"] == -9);
    h.Call("game_unload");
    stdfs::remove_all(dir);
}

AETHER_TEST(Mcp_VfxToolsEditValidateSimulateAndCheck) {
    namespace stdfs = std::filesystem;
    // --- the editor scene: validated, undoable ParticleSystem edits
    Harness h;
    std::string spark = h.Create({{"Transform", Json::object()}});
    AETHER_CHECK(h.Call("particle_list").value.empty());
    AETHER_CHECK(h.Call("particle_set", {{"entity", spark}, {"fields", {{"time_scale", -1}}}}).is_error);
    AETHER_CHECK(h.Call("particle_set", {{"entity", spark}, {"fields", {{"lod_spawn_scale", 2}}}}).is_error);
    AETHER_CHECK(h.Call("particle_set", {{"entity", spark}, {"fields", {{"active", true}}}}).is_error);
    AETHER_CHECK(h.Call("particle_set", {{"entity", spark}, {"fields", {{"colour", "red"}}}}).is_error);
    AETHER_CHECK(!h.Components(spark).contains("ParticleSystem")); // a refused edit leaves nothing behind
    AETHER_CHECK(!h.Call("particle_set", {{"entity", spark}, {"fields", {{"asset", "VFX/Sparks.avfx"}, {"destroy_when_finished", true}}}}).is_error);
    AETHER_CHECK(!h.Call("particle_set", {{"entity", spark}, {"fields", {{"cull_distance", 80}}}}).is_error);
    AETHER_CHECK(h.Components(spark)["ParticleSystem"]["asset"] == "VFX/Sparks.avfx");
    AETHER_CHECK(h.Components(spark)["ParticleSystem"]["cull_distance"] == 80);
    h.Call("undo");
    AETHER_CHECK(h.Components(spark)["ParticleSystem"]["cull_distance"] == 0);
    h.Call("undo");
    AETHER_CHECK(!h.Components(spark).contains("ParticleSystem")); // add + set were one step
    h.Call("redo");
    AETHER_CHECK(h.Call("particle_list").value.size() == 1);
    AETHER_CHECK(!h.Call("particle_remove", {{"entity", spark}}).is_error);
    AETHER_CHECK(h.Call("particle_remove", {{"entity", spark}}).is_error);

    // --- the format, without a project
    auto project = MakeAssetHost();
    RegisterAssetTools(h.server, project);
    RegisterVfxTools(h.server, project);
    Json schema = h.Call("vfx_schema").value;
    AETHER_CHECK(schema["modules"]["spawn"].size() == 3);
    AETHER_CHECK(schema["modules"]["spawn"].contains("SpawnRate"));
    AETHER_CHECK(schema["modules"]["render"].size() == 4);
    const Json sample = schema["sample"];
    AETHER_CHECK(h.Call("vfx_validate", {{"definition", sample}}).value["ok"] == true);
    Json broken = sample;
    broken["emitters"][0]["max_particles"] = 0;
    Json verdict = h.Call("vfx_validate", {{"definition", broken}}).value;
    AETHER_CHECK(verdict["ok"] == false);
    AETHER_CHECK(verdict["diagnostics"][0]["code"] == "FX003");
    AETHER_CHECK(h.Call("vfx_validate", {{"definition", "{ not json"}}).is_error);
    AETHER_CHECK(h.Call("vfx_validate").is_error);
    AETHER_CHECK(h.Call("vfx_list").is_error); // needs a project

    // --- simulation: deterministic, parameter-driven
    Json run = h.Call("vfx_simulate", {{"definition", sample}, {"seconds", 2}, {"seed", 5}}).value;
    AETHER_CHECK(run["ok"] == true);
    AETHER_CHECK(run["emitters"][0]["born"].get<int>() > 50);
    AETHER_CHECK(run["peak_alive"].get<int>() > 0);
    AETHER_CHECK(run["loops_on"] == true); // a looping emitter does not finish
    AETHER_CHECK(run["timeline"].size() >= 5);
    Json same = h.Call("vfx_simulate", {{"definition", sample}, {"seconds", 2}, {"seed", 5}}).value;
    AETHER_CHECK(same == run); // same effect, seed and steps: same result
    Json other = h.Call("vfx_simulate", {{"definition", sample}, {"seconds", 2}, {"seed", 6}}).value;
    AETHER_CHECK(other["emitters"][0]["sample_particles"] != run["emitters"][0]["sample_particles"]);
    Json hot = h.Call("vfx_simulate", {{"definition", sample}, {"seconds", 2}, {"seed", 5}, {"parameters", {{"Intensity", 300}}}}).value;
    AETHER_CHECK(hot["emitters"][0]["born"].get<int>() > 2 * run["emitters"][0]["born"].get<int>()); // the bound spawn rate followed it
    AETHER_CHECK(h.Call("vfx_simulate", {{"definition", sample}, {"parameters", {{"Nope", 1}}}}).is_error);
    AETHER_CHECK(h.Call("vfx_simulate", {{"definition", sample}, {"parameters", {{"Intensity", "loud"}}}}).is_error);
    AETHER_CHECK(h.Call("vfx_simulate", {{"definition", sample}, {"seconds", 99}}).is_error);
    AETHER_CHECK(h.Call("vfx_simulate", {{"definition", broken}}).value["ok"] == false); // errors are reported, not simulated

    // A one-shot finishes.
    Json one_shot = sample;
    one_shot["emitters"][0]["looping"] = false;
    one_shot["emitters"][0]["duration"] = 0.5;
    Json shot = h.Call("vfx_simulate", {{"definition", one_shot}, {"seconds", 5}, {"dt", 0.02}}).value;
    AETHER_CHECK(shot["loops_on"] == false);
    AETHER_CHECK(shot["finished_at"].get<double>() > 0.4);
    AETHER_CHECK(shot["alive_at_end"] == 0);

    // --- a project: effects live under Content/ and scenes refer to them
    const stdfs::path dir = stdfs::temp_directory_path() / "aether_mcp_vfx_test";
    stdfs::remove_all(dir);
    stdfs::create_directories(dir);
    ProjectPaths paths;
    std::string error;
    AETHER_CHECK(CreateProject(dir, "McpVfx", &paths, &error));
    AETHER_CHECK(!h.Call("project_open", {{"project_file", paths.file.string()}}).is_error);
    AETHER_CHECK(h.Call("vfx_put", {{"path", "VFX/Broken"}, {"definition", broken}}).is_error);
    AETHER_CHECK(!stdfs::exists(paths.content / "VFX" / "Broken.avfx"));
    Harness::Result put = h.Call("vfx_put", {{"path", "VFX/Sparks"}, {"definition", sample}});
    AETHER_CHECK(!put.is_error);
    AETHER_CHECK(put.value["path"] == "VFX/Sparks.avfx");
    AETHER_CHECK(stdfs::exists(paths.content / "VFX" / "Sparks.avfx"));
    AETHER_CHECK(h.Call("vfx_put", {{"path", "VFX/Sparks"}, {"definition", sample}}).is_error); // exists
    AETHER_CHECK(!h.Call("vfx_put", {{"path", "VFX/Sparks"}, {"definition", sample}, {"overwrite", true}}).is_error);
    AETHER_CHECK(h.Call("vfx_put", {{"path", "../Escape"}, {"definition", sample}}).is_error);
    AETHER_CHECK(h.Call("vfx_get", {{"asset", "VFX/Sparks.avfx"}}).value["definition"]["name"] == "Sparks");
    Json listed = h.Call("vfx_list").value;
    AETHER_CHECK(listed.size() == 1);
    AETHER_CHECK(listed[0]["valid"] == true);
    AETHER_CHECK(h.Call("vfx_simulate", {{"asset", "VFX/Sparks.avfx"}, {"seconds", 1}}).value["ok"] == true);
    AETHER_CHECK(h.Call("vfx_simulate", {{"asset", "VFX/Missing.avfx"}}).is_error);

    auto scene_with = [](const char* asset) {
        return Json{{"$type", "Scene"}, {"$version", 1},
                    {"entities", Json::array({{{"components", {{"Transform", {{"position", {0, 0, 0}}, {"rotation", {0, 0, 0, 1}}, {"scale", {1, 1, 1}}}},
                                                                  {"ParticleSystem", {{"asset", asset}}}}}}})}};
    };
    AETHER_CHECK(!h.Call("content_write", {{"path", "Scenes/start.ascene"}, {"text", scene_with("VFX/Gone.avfx").dump()}}).is_error);
    Json check = h.Call("vfx_check").value;
    AETHER_CHECK(check["ok"] == false);
    AETHER_CHECK(check["scene_uses"] == 1);
    AETHER_CHECK(check["problems"][0]["problem"].get<std::string>().find("VFX/Gone.avfx") != std::string::npos);
    AETHER_CHECK(!h.Call("content_write", {{"path", "Scenes/start.ascene"}, {"text", scene_with("VFX/Sparks.avfx").dump()}}).is_error);
    AETHER_CHECK(h.Call("vfx_check").value["ok"] == true);
    // A hand-edited, broken effect shows up as a problem.
    AETHER_CHECK(!h.Call("content_write", {{"path", "VFX/Bad.avfx"}, {"text", "{ not json"}}).is_error);
    AETHER_CHECK(h.Call("vfx_check").value["ok"] == false);
    stdfs::remove_all(dir);
}

AETHER_TEST(Mcp_TerrainToolsSculptPaintPlanAndRoundTrip) {
    namespace stdfs = std::filesystem;
    Harness h;
    RegisterTerrainTools(h.server);
    auto height_at = [&](double x, double z) { return h.Call("terrain_sample", {{"points", {{x, z}}}}).value["points"][0]["height"].get<double>(); };
    auto weight_at = [&](double x, double z, int layer) { return h.Call("terrain_sample", {{"points", {{x, z}}}}).value["points"][0]["layers"][layer]["weight"].get<double>(); };

    AETHER_CHECK(h.Call("terrain_info").is_error); // nothing yet
    AETHER_CHECK(h.Call("terrain_sculpt", {{"tool", "raise"}, {"strokes", {{1, 1}}}}).is_error);
    AETHER_CHECK(h.Call("terrain_create", {{"width", 5}}).is_error);
    AETHER_CHECK(h.Call("terrain_create", {{"kind", "volcanic"}}).is_error);
    AETHER_CHECK(h.Call("terrain_create", {{"chunk_size", 20}}).is_error);
    AETHER_CHECK(h.Call("terrain_create", {{"layers", Json::array({{{"name", "a"}}, {{"name", "b"}}, {{"name", "c"}}, {{"name", "d"}}, {{"name", "e"}}})}}).is_error);

    // A flat 65 x 65 terrain, 2 m cells: 128 m across.
    Json info = h.Call("terrain_create", {{"width", 65}, {"cell_size", 2}}).value;
    AETHER_CHECK(info["world_size"]["width"] == 128);
    AETHER_CHECK(info["heights"]["max"] == 0);
    AETHER_CHECK(info["layers"].size() == 3);
    AETHER_CHECK(info["chunks"]["total"] == 4);

    // Sculpt: one undoable step per call.
    Harness::Result raised = h.Call("terrain_sculpt", {{"tool", "raise"}, {"strokes", {{64, 64}}}, {"radius", 20}, {"strength", 1}, {"amount", 10}});
    AETHER_CHECK(!raised.is_error);
    AETHER_CHECK(raised.value["changed"] == true);
    AETHER_CHECK(height_at(64, 64) > 5.0);
    AETHER_CHECK(std::fabs(height_at(5, 5)) < 1e-6); // outside the brush
    AETHER_CHECK(raised.value["terrain"]["undo_depth"] == 1);
    const double peak = height_at(64, 64);
    AETHER_CHECK(!h.Call("terrain_undo").is_error);
    AETHER_CHECK(std::fabs(height_at(64, 64)) < 1e-6);
    AETHER_CHECK(!h.Call("terrain_redo").is_error);
    AETHER_CHECK(std::fabs(height_at(64, 64) - peak) < 1e-6);
    AETHER_CHECK(h.Call("terrain_redo").is_error);

    // Lowering, smoothing and flattening move the peak the way their names say.
    const double shoulder = height_at(76, 64); // where the bump curves, so averaging neighbours changes it (its flat top it would not)
    h.Call("terrain_sculpt", {{"tool", "smooth"}, {"strokes", {{64, 64}, {64, 64}, {64, 64}}}, {"radius", 20}, {"strength", 1}});
    AETHER_CHECK(std::fabs(height_at(76, 64) - shoulder) > 1e-4);
    const double smoothed = height_at(64, 64);
    h.Call("terrain_sculpt", {{"tool", "lower"}, {"strokes", {{64, 64}}}, {"radius", 20}, {"strength", 1}, {"amount", 10}});
    AETHER_CHECK(height_at(64, 64) < smoothed);
    h.Call("terrain_sculpt", {{"tool", "flatten"}, {"strokes", {{64, 64}, {70, 64}}}, {"radius", 30}, {"strength", 1}});

    // Bad sculpt requests change nothing.
    const double before_bad = height_at(64, 64);
    AETHER_CHECK(h.Call("terrain_sculpt", {{"tool", "dig"}, {"strokes", {{1, 1}}}}).is_error);
    AETHER_CHECK(h.Call("terrain_sculpt", {{"tool", "raise"}, {"strokes", Json::array()}}).is_error);
    AETHER_CHECK(h.Call("terrain_sculpt", {{"tool", "raise"}, {"strokes", {{1}}}}).is_error);
    AETHER_CHECK(h.Call("terrain_sculpt", {{"tool", "paint"}, {"strokes", {{1, 1}}}}).is_error);
    AETHER_CHECK(h.Call("terrain_sculpt", {{"tool", "paint"}, {"strokes", {{1, 1}}}, {"layer", "Lava"}}).is_error);
    AETHER_CHECK(h.Call("terrain_sculpt", {{"tool", "paint"}, {"strokes", {{1, 1}}}, {"layer", 9}}).is_error);
    AETHER_CHECK(height_at(64, 64) == before_bad);

    // Paint a layer by name.
    AETHER_CHECK(weight_at(30, 30, 0) > 0.99); // all Grass to begin with
    AETHER_CHECK(!h.Call("terrain_sculpt", {{"tool", "paint"}, {"strokes", {{30, 30}, {30, 30}}}, {"radius", 15}, {"strength", 1}, {"layer", "Rock"}}).is_error);
    AETHER_CHECK(weight_at(30, 30, 1) > 0.5);
    AETHER_CHECK(weight_at(30, 30, 0) < 0.5);
    AETHER_CHECK(weight_at(120, 120, 0) > 0.99); // far away: still Grass

    // Chunks and LOD for a camera: near sees more detail than far.
    Json near_camera = h.Call("terrain_chunks", {{"camera", {64, 10, 64}}}).value;
    Json far_camera = h.Call("terrain_chunks", {{"camera", {5000, 5000, 5000}}}).value;
    AETHER_CHECK(near_camera["chunks"] == 4);
    AETHER_CHECK(near_camera["total_triangles"].get<int>() > far_camera["total_triangles"].get<int>());
    AETHER_CHECK(near_camera["per_lod"].size() == 4);
    AETHER_CHECK(near_camera["per_lod"][0]["triangles"].get<int>() > near_camera["per_lod"][3]["triangles"].get<int>());
    AETHER_CHECK(h.Call("terrain_chunks", {{"camera", {1, 2}}}).is_error);

    // Foliage: the seed repeats, filters filter, clear clears.
    Json scatter = h.Call("terrain_scatter_foliage", {{"density", 1}, {"spacing", 4}, {"seed", 3}}).value;
    AETHER_CHECK(scatter["instances"].get<int>() > 100);
    AETHER_CHECK(h.Call("terrain_scatter_foliage", {{"density", 1}, {"spacing", 4}, {"seed", 3}}).value["instances"] == scatter["instances"]);
    AETHER_CHECK(h.Call("terrain_scatter_foliage", {{"density", 1}, {"spacing", 4}, {"min_height", 1000}}).value["instances"] == 0);
    AETHER_CHECK(h.Call("terrain_scatter_foliage", {{"density", 0.1}, {"spacing", 4}, {"seed", 3}}).value["instances"].get<int>() < scatter["instances"].get<int>());
    AETHER_CHECK(h.Call("terrain_scatter_foliage", {{"types", {{{"scale_min", 3}, {"scale_max", 1}}}}}).is_error);
    AETHER_CHECK(h.Call("terrain_scatter_foliage", {{"spacing", 0}}).is_error);
    h.Call("terrain_scatter_foliage", {{"density", 1}, {"spacing", 4}});
    AETHER_CHECK(h.Call("terrain_clear_foliage").value["removed"].get<int>() > 0);

    // The picture: an image content item, and a PNG file if asked.
    const stdfs::path dir = stdfs::temp_directory_path() / "aether_mcp_terrain_test";
    stdfs::remove_all(dir);
    stdfs::create_directories(dir);
    Json rpc = h.Rpc("tools/call", {{"name", "terrain_preview"}, {"arguments", {{"mode", "layers"}, {"size", 128}, {"path", (dir / "look.png").string()}}}});
    AETHER_CHECK(rpc["result"]["isError"] == false);
    AETHER_CHECK(rpc["result"]["content"].size() == 2);
    AETHER_CHECK(rpc["result"]["content"][1]["type"] == "image");
    AETHER_CHECK(rpc["result"]["content"][1]["mimeType"] == "image/png");
    {
        std::ifstream png(dir / "look.png", std::ios::binary);
        char magic[4] = {};
        png.read(magic, 4);
        AETHER_CHECK(magic[1] == 'P' && magic[2] == 'N' && magic[3] == 'G');
    }
    for (const char* mode : {"shaded", "slope", "height"}) AETHER_CHECK(!h.Call("terrain_preview", {{"mode", mode}, {"size", 64}}).is_error);
    AETHER_CHECK(h.Call("terrain_preview", {{"mode", "xray"}}).is_error);
    AETHER_CHECK(h.Call("terrain_preview", {{"size", 8}}).is_error);

    // Export, wreck the terrain, import it back.
    const double saved_peak = height_at(64, 64);
    const double saved_rock = weight_at(30, 30, 1);
    AETHER_CHECK(!h.Call("terrain_export", {{"path", (dir / "hills").string()}}).is_error);
    AETHER_CHECK(stdfs::exists(dir / "hills.r16") && stdfs::exists(dir / "hills.splat") && stdfs::exists(dir / "hills.terrain.json"));
    h.Call("terrain_create", {{"width", 17}});
    AETHER_CHECK(h.Call("terrain_info").value["samples"]["width"] == 17);
    Json imported = h.Call("terrain_import", {{"path", (dir / "hills").string()}}).value;
    AETHER_CHECK(imported["samples"]["width"] == 65);
    AETHER_CHECK(imported["world_size"]["width"] == 128);
    AETHER_CHECK(imported["undo_depth"] == 0);
    AETHER_CHECK(std::fabs(height_at(64, 64) - saved_peak) < 0.01 + std::fabs(saved_peak) * 1e-3); // 16-bit storage
    AETHER_CHECK(std::fabs(weight_at(30, 30, 1) - saved_rock) < 0.02);
    AETHER_CHECK(h.Call("terrain_import", {{"path", (dir / "nothing").string()}}).is_error);
    {
        std::ofstream bad(dir / "bad.terrain.json");
        bad << "{\"format\": \"something-else\"}";
    }
    AETHER_CHECK(h.Call("terrain_import", {{"path", (dir / "bad").string()}}).is_error);
    AETHER_CHECK(h.Call("terrain_info").value["samples"]["width"] == 65); // a failed import leaves the terrain alone

    // The engine's procedural generator: hills with a height range, the same every time.
    Json hills = h.Call("terrain_create", {{"width", 65}, {"kind", "procedural"}, {"scale", 8}}).value;
    AETHER_CHECK(hills["heights"]["max"].get<double>() > hills["heights"]["min"].get<double>() + 1.0);
    Json again = h.Call("terrain_create", {{"width", 65}, {"kind", "procedural"}, {"scale", 8}}).value;
    AETHER_CHECK(again["heights"] == hills["heights"]);
    stdfs::remove_all(dir);
}

AETHER_TEST(Mcp_NavToolsBakeQuerySimulateAndEditComponents) {
    namespace stdfs = std::filesystem;
    Harness h;
    RegisterTerrainTools(h.server);
    RegisterNavTools(h.server, h.session);
    auto path_length = [&](Json start, Json end, Json extra = Json::object()) {
        Json args = extra;
        args["queries"] = Json::array({{{"start", start}, {"end", end}}});
        Json r = h.Call("nav_path", args).value["paths"][0];
        return r["status"] == "complete" ? r["length"].get<double>() : -1.0;
    };

    // --- nothing baked yet; bad geometry is refused as a whole
    AETHER_CHECK(h.Call("nav_path", {{"queries", {{{"start", {0, 0, 0}}, {"end", {1, 0, 1}}}}}}).is_error);
    AETHER_CHECK(h.Call("nav_bake").is_error); // no geometry
    AETHER_CHECK(h.Call("nav_geometry_add", {{"planes", {{{"center", {0, 0, 0}}, {"half_x", 5}, {"half_z", 5}}}}, {"boxes", {{{"center", {0, 0, 0}}}}}}).is_error);
    AETHER_CHECK(h.Call("nav_info").value["geometry"]["triangles"] == 0); // the valid plane was not kept either
    AETHER_CHECK(h.Call("nav_geometry_add", {{"volumes", {{{"shape", "cone"}}}}}).is_error);
    AETHER_CHECK(h.Call("nav_geometry_add", {{"planes", {{{"center", {0, 0}}, {"half_x", 1}, {"half_z", 1}}}}}).is_error);
    AETHER_CHECK(h.Call("nav_geometry_add", {{"triangles", {{{"vertices", {{0, 0, 0}, {1, 0, 0}, {0, 0, 1}}}, {"indices", {0, 1, 5}}}}}}).is_error);
    AETHER_CHECK(h.Call("nav_geometry_add", {{"planes", {{{"center", {0, 0, 0}}, {"half_x", 1}, {"half_z", 1}, {"area", 64}}}}}).is_error);

    // --- a floor with a wall across the middle, a pillar, and a band of area 2 across the south
    AETHER_CHECK(!h.Call("nav_geometry_add", {{"planes", {{{"center", {20, 0, 20}}, {"half_x", 20}, {"half_z", 20}}}},
                                              {"boxes", {{{"center", {20, 1.5, 20}}, {"half_extents", {1, 1.5, 12}}}}},
                                              {"volumes", {{{"shape", "cylinder"}, {"center", {8, 0, 8}}, {"radius", 3}, {"height", 2}},
                                                           {{"shape", "box"}, {"center", {32, 0, 6}}, {"half_extents", {6, 1, 6}}, {"area", 2}}}}}).is_error);
    Json baked = h.Call("nav_bake", {{"agent_radius", 0.5}}).value;
    AETHER_CHECK(baked["baked"] == true);
    AETHER_CHECK(baked["bake"]["polygons"].get<int>() > 5);
    AETHER_CHECK(baked["from_geometry"] == true);
    AETHER_CHECK(h.Call("nav_bake", {{"agent_radius", -1}}).is_error);

    // --- paths go round the wall: longer than the straight line, and complete
    const double straight = std::sqrt(30.0 * 30.0 + 0.0);
    const double around = path_length({5, 0, 20}, {35, 0, 20});
    AETHER_CHECK(around > straight + 5.0);
    AETHER_CHECK(path_length({5, 0, 3}, {35, 0, 3}) > 0.0);
    // Area costs steer routes: the area-2 patch (x 26 to 38, z 0 to 12) is on the direct line from (23,0,6) to (39,0,6).
    const double direct = path_length({23, 0, 6}, {39, 0, 6});
    const double costly = path_length({23, 0, 6}, {39, 0, 6}, {{"area_costs", {{"2", 50}}}});
    AETHER_CHECK(direct > 0.0);
    AETHER_CHECK(costly > direct + 1.0); // detours round the costly area
    AETHER_CHECK(h.Call("nav_path", {{"queries", {{{"start", {5, 0, 5}}, {"end", {35, 0, 35}}}}}, {"area_costs", {{"2", 0.5}}}}).is_error);
    AETHER_CHECK(h.Call("nav_path", {{"queries", Json::array()}}).is_error);

    // --- the other questions
    AETHER_CHECK(h.Call("nav_query", {{"kind", "reachable"}, {"a", {5, 0, 5}}, {"b", {35, 0, 35}}}).value["reachable"] == true);
    Json ray = h.Call("nav_query", {{"kind", "raycast"}, {"start", {5, 0, 20}}, {"end", {35, 0, 20}}}).value;
    AETHER_CHECK(ray["blocked"] == true); // the wall is in the way
    AETHER_CHECK(h.Call("nav_query", {{"kind", "raycast"}, {"start", {3, 0, 30}}, {"end", {12, 0, 30}}}).value["blocked"] == false);
    Json nearest = h.Call("nav_query", {{"kind", "nearest"}, {"points", {{20, 0.1, 3}, {500, 0, 500}}}}).value["points"];
    AETHER_CHECK(nearest[0]["on_mesh"] == true);
    AETHER_CHECK(nearest[1]["on_mesh"] == false);
    Json random_a = h.Call("nav_query", {{"kind", "random"}, {"count", 5}, {"seed", 9}}).value;
    Json random_b = h.Call("nav_query", {{"kind", "random"}, {"count", 5}, {"seed", 9}}).value;
    AETHER_CHECK(random_a["points"].size() == 5);
    AETHER_CHECK(random_a == random_b); // the seed repeats
    AETHER_CHECK(h.Call("nav_query", {{"kind", "teleport"}}).is_error);
    AETHER_CHECK(h.Call("nav_query", {{"kind", "random"}, {"count", 0}}).is_error);

    // --- an island the ground does not reach, then a link that does
    AETHER_CHECK(!h.Call("nav_geometry_add", {{"planes", {{{"center", {70, 0, 20}}, {"half_x", 8}, {"half_z", 8}}}}}).is_error);
    AETHER_CHECK(!h.Call("nav_bake", {{"agent_radius", 0.5}}).is_error);
    AETHER_CHECK(h.Call("nav_query", {{"kind", "reachable"}, {"a", {5, 0, 5}}, {"b", {70, 0, 20}}}).value["reachable"] == false);
    AETHER_CHECK(!h.Call("nav_geometry_add", {{"links", {{{"start", {38, 0, 20}}, {"end", {64, 0, 20}}, {"radius", 1.5}}}}}).is_error);
    // Detour joins an off-mesh link only inside a tile or between neighbouring ones, so a long link needs one big tile.
    Json linked = h.Call("nav_bake", {{"agent_radius", 0.5}, {"tile_size", 0}}).value;
    AETHER_CHECK(linked["links_in_mesh"].get<int>() >= 1);
    AETHER_CHECK(h.Call("nav_query", {{"kind", "reachable"}, {"a", {5, 0, 5}}, {"b", {70, 0, 20}}}).value["reachable"] == true);
    Json over = h.Call("nav_path", {{"queries", {{{"start", {5, 0, 5}}, {"end", {70, 0, 20}}}}}}).value["paths"][0];
    AETHER_CHECK(over["status"] == "complete");
    AETHER_CHECK(!over["link_starts_at_points"].empty());

    // --- the picture
    const stdfs::path dir = stdfs::temp_directory_path() / "aether_mcp_nav_test";
    stdfs::remove_all(dir);
    stdfs::create_directories(dir);
    Json rpc = h.Rpc("tools/call", {{"name", "nav_preview"}, {"arguments", {{"size", 128}, {"path", (dir / "nav.png").string()}, {"paths", {{{"start", {5, 0, 5}}, {"end", {35, 0, 35}}}}}, {"points", {{10, 0, 10}}}}}});
    AETHER_CHECK(rpc["result"]["isError"] == false);
    AETHER_CHECK(rpc["result"]["content"].size() == 2);
    AETHER_CHECK(rpc["result"]["content"][1]["mimeType"] == "image/png");
    {
        std::ifstream png(dir / "nav.png", std::ios::binary);
        char magic[4] = {};
        png.read(magic, 4);
        AETHER_CHECK(magic[1] == 'P' && magic[2] == 'N' && magic[3] == 'G');
    }
    AETHER_CHECK(h.Call("nav_preview", {{"size", 5}}).is_error);

    // --- a crowd crossing the floor: arrivals, detours and spacing
    Json crowd = h.Call("nav_simulate", {{"agents", {{{"start", {3, 0, 3}}, {"goal", {37, 0, 37}}}, {{"start", {37, 0, 37}}, {"goal", {3, 0, 3}}}, {{"start", {3, 0, 37}}, {"goal", {37, 0, 3}}}}}, {"seconds", 60}}).value;
    AETHER_CHECK(crowd["arrived"] == 3);
    AETHER_CHECK(crowd["failed"] == 0);
    for (const Json& a : crowd["agents"]) {
        AETHER_CHECK(a["status"] == "arrived");
        AETHER_CHECK(a["arrived_at"].get<double>() > 1.0);
        AETHER_CHECK(a["distance_walked"].get<double>() > 30.0);
        AETHER_CHECK(a["distance_walked"].get<double>() < a["shortest_path_length"].get<double>() * 1.5);
        AETHER_CHECK(!a["samples"].empty());
    }
    AETHER_CHECK(crowd["closest_agent_distance"].get<double>() > 0.1);
    AETHER_CHECK(h.Call("nav_simulate", {{"agents", {{{"start", {900, 0, 900}}, {"goal", {3, 0, 3}}}}}}).is_error); // start off the mesh
    AETHER_CHECK(h.Call("nav_simulate", {{"agents", {{{"start", {3, 0, 3}}}}}}).is_error);
    AETHER_CHECK(h.Call("nav_simulate", {{"agents", Json::array()}}).is_error);

    // --- save and load the mesh
    const std::size_t polygons = h.Call("nav_info").value["polygons"].get<std::size_t>();
    AETHER_CHECK(!h.Call("nav_export", {{"path", (dir / "level").string()}}).is_error);
    AETHER_CHECK(stdfs::exists(dir / "level.anav"));
    AETHER_CHECK(!h.Call("nav_geometry_clear").is_error);
    AETHER_CHECK(!h.Call("nav_geometry_add", {{"planes", {{{"center", {0, 0, 0}}, {"half_x", 3}, {"half_z", 3}}}}}).is_error);
    AETHER_CHECK(!h.Call("nav_bake").is_error);
    AETHER_CHECK(h.Call("nav_info").value["polygons"].get<std::size_t>() != polygons);
    Json imported = h.Call("nav_import", {{"path", (dir / "level.anav").string()}}).value;
    AETHER_CHECK(imported["polygons"].get<std::size_t>() == polygons);
    AETHER_CHECK(imported["from_geometry"] == false);
    AETHER_CHECK(path_length({5, 0, 20}, {35, 0, 20}) > 0.0); // the loaded mesh answers queries
    {
        std::ofstream bad(dir / "bad.anav", std::ios::binary);
        bad << "not a mesh";
    }
    AETHER_CHECK(h.Call("nav_import", {{"path", (dir / "bad.anav").string()}}).is_error);
    AETHER_CHECK(h.Call("nav_import", {{"path", (dir / "none.anav").string()}}).is_error);
    AETHER_CHECK(h.Call("nav_info").value["polygons"].get<std::size_t>() == polygons); // a failed import leaves the mesh alone

    // --- the sculpted terrain as ground
    AETHER_CHECK(h.Call("nav_geometry_add_terrain").is_error); // no terrain yet
    AETHER_CHECK(!h.Call("terrain_create", {{"width", 65}, {"depth", 65}, {"cell_size", 1}}).is_error);
    AETHER_CHECK(!h.Call("terrain_sculpt", {{"tool", "raise"}, {"strokes", {{32, 32}}}, {"radius", 8}, {"strength", 1}, {"amount", 12}}).is_error); // a steep hill in the middle
    AETHER_CHECK(!h.Call("nav_geometry_clear").is_error);
    Json terrain = h.Call("nav_geometry_add_terrain", {{"stride", 1}}).value;
    AETHER_CHECK(terrain["triangles"] == 64 * 64 * 2);
    AETHER_CHECK(h.Call("nav_geometry_add_terrain", {{"stride", 99}}).is_error);
    AETHER_CHECK(!h.Call("nav_bake", {{"agent_radius", 0.5}, {"agent_max_slope", 35}}).is_error);
    const double hill = path_length({5, 0, 32}, {59, 0, 32});
    AETHER_CHECK(hill > 54.0); // over or round the hill, never shorter than straight across
    stdfs::remove_all(dir);

    // --- the editor scene's navigation components
    std::string agent = h.Create({{"Transform", Json::object()}});
    AETHER_CHECK(h.Call("nav_component_list").value.empty());
    AETHER_CHECK(h.Call("nav_component_set", {{"entity", agent}, {"component", "Transform"}, {"fields", Json::object()}}).is_error);
    AETHER_CHECK(h.Call("nav_component_set", {{"entity", agent}, {"component", "NavAgent"}, {"fields", {{"radius", 9}}}}).is_error);
    AETHER_CHECK(h.Call("nav_component_set", {{"entity", agent}, {"component", "NavAgent"}, {"fields", {{"avoidance_quality", 7}}}}).is_error);
    AETHER_CHECK(h.Call("nav_component_set", {{"entity", agent}, {"component", "NavAgent"}, {"fields", {{"status", "Moving"}}}}).is_error);
    AETHER_CHECK(h.Call("nav_component_set", {{"entity", agent}, {"component", "NavAgent"}, {"fields", {{"speed", 3}}}}).is_error);
    AETHER_CHECK(h.Call("nav_component_set", {{"entity", agent}, {"component", "NavObstacle"}, {"fields", {{"shape", "Sphere"}}}}).is_error);
    AETHER_CHECK(h.Call("nav_component_set", {{"entity", agent}, {"component", "NavModifierVolume"}, {"fields", {{"area", 64}}}}).is_error);
    AETHER_CHECK(!h.Components(agent).contains("NavAgent")); // refused edits leave nothing behind

    AETHER_CHECK(!h.Call("nav_component_set", {{"entity", agent}, {"component", "NavAgent"}, {"fields", {{"radius", 0.4}, {"max_speed", 5}}}}).is_error);
    AETHER_CHECK(h.Components(agent)["NavAgent"]["max_speed"] == 5);
    AETHER_CHECK(!h.Call("nav_component_set", {{"entity", agent}, {"component", "NavAgent"}, {"fields", {{"allow_partial", true}}}}).is_error);
    h.Call("undo");
    AETHER_CHECK(h.Components(agent)["NavAgent"]["allow_partial"] == false);
    h.Call("undo");
    AETHER_CHECK(!h.Components(agent).contains("NavAgent")); // add + set were one step
    h.Call("redo");

    std::string pillar = h.Create({{"Transform", {{"position", {10, 0, 10}}, {"rotation", {0, 0, 0, 1}}, {"scale", {1, 1, 1}}}}});
    AETHER_CHECK(!h.Call("nav_component_set", {{"entity", pillar}, {"component", "NavObstacle"}, {"fields", {{"shape", "Cylinder"}, {"radius", 2}}}}).is_error);
    std::string pond = h.Create({{"Transform", {{"position", {20, 0, 20}}, {"rotation", {0, 0, 0, 1}}, {"scale", {1, 1, 1}}}}});
    AETHER_CHECK(!h.Call("nav_component_set", {{"entity", pond}, {"component", "NavModifierVolume"}, {"fields", {{"half_extents", {3, 1, 3}}, {"area", 2}}}}).is_error);
    std::string jump = h.Create({{"Transform", {{"position", {0, 0, 0}}, {"rotation", {0, 0, 0, 1}}, {"scale", {1, 1, 1}}}}});
    AETHER_CHECK(!h.Call("nav_component_set", {{"entity", jump}, {"component", "NavLinkProxy"}, {"fields", {{"start", {1, 0, 0}}, {"end", {1, 0, 5}}}}}).is_error);
    AETHER_CHECK(h.Call("nav_component_list").value.size() == 4);

    AETHER_CHECK(!h.Call("nav_geometry_clear").is_error);
    Json from_scene = h.Call("nav_geometry_add_scene").value;
    AETHER_CHECK(from_scene["added"]["obstacles"] == 1);
    AETHER_CHECK(from_scene["added"]["modifier_volumes"] == 1);
    AETHER_CHECK(from_scene["added"]["links"] == 1);
    AETHER_CHECK(from_scene["volumes"] == 2);
    AETHER_CHECK(from_scene["links"] == 1);
    AETHER_CHECK(!h.Call("nav_component_remove", {{"entity", pillar}, {"component", "NavObstacle"}}).is_error);
    AETHER_CHECK(h.Call("nav_component_remove", {{"entity", pillar}, {"component", "NavObstacle"}}).is_error);
}

AETHER_TEST(Mcp_BlueprintToolsFindValidateCompileAndRun) {
    namespace stdfs = std::filesystem;
    Harness h;
    auto project = MakeAssetHost();
    RegisterAssetTools(h.server, project);
    RegisterKitTools(h.server, project);
    RegisterBlueprintTools(h.server, project);

    // --- the palette and a node's pins
    Json palette = h.Call("bp_node_types", {{"search", "branch"}}).value;
    AETHER_CHECK(palette["total"].get<int>() >= 1);
    bool found_branch = false;
    for (const Json& n : palette["nodes"]) found_branch = found_branch || n["id"] == "Flow.Branch";
    AETHER_CHECK(found_branch);
    AETHER_CHECK(h.Call("bp_node_types", {{"limit", 5}}).value["nodes"].size() == 5);
    Json flow = h.Call("bp_node_types", {{"category", "Flow Control"}, {"limit", 500}}).value;
    for (const Json& n : flow["nodes"]) AETHER_CHECK(n["category"] == "Flow Control");
    AETHER_CHECK(flow["categories"].size() > 10);

    Json branch = h.Call("bp_node_info", {{"type", "Flow.Branch"}}).value;
    AETHER_CHECK(branch["kind"] == "impure");
    AETHER_CHECK(branch["pins"].size() == 4);
    AETHER_CHECK(branch["pins"][1]["name"] == "condition");
    AETHER_CHECK(branch["pins"][1]["type"] == "bool");
    AETHER_CHECK(h.Call("bp_node_info", {{"type", "Flow.Nonsense"}}).is_error);
    AETHER_CHECK(h.Call("bp_node_info", {{"type", "Var.Get:Count"}}).is_error); // no such variable without a Blueprint

    const Json sample = h.Call("kit_schema", {{"type", "blueprint"}}).value["sample"];
    Json with_blueprint = h.Call("bp_node_info", {{"type", "Var.Get:Count"}, {"definition", sample}}).value;
    AETHER_CHECK(with_blueprint["kind"] == "pure");
    AETHER_CHECK(with_blueprint["pins"][0]["type"] == "int");
    Json custom = h.Call("bp_node_info", {{"type", "Event.Custom"}, {"config", {{"name", "Boom"}}}}).value;
    AETHER_CHECK(custom["title"] == "Boom");
    AETHER_CHECK(custom["event_key"] == "Event.Custom:Boom");
    Json own = h.Call("bp_node_types", {{"definition", sample}, {"search", "Count"}}).value;
    bool has_get = false;
    for (const Json& n : own["nodes"]) has_get = has_get || n["id"] == "Var.Get:Count";
    AETHER_CHECK(has_get); // the Blueprint's own variables join the palette

    // --- validate and compile
    Json valid = h.Call("bp_validate", {{"definition", sample}}).value;
    AETHER_CHECK(valid["ok"] == true);
    AETHER_CHECK(valid["errors"] == 0);
    Json broken = sample;
    broken["graphs"][0]["links"].push_back({{"from", {99, "then"}}, {"to", {1, "exec"}}});
    Json bad = h.Call("bp_validate", {{"definition", broken}}).value;
    AETHER_CHECK(bad["ok"] == false);
    AETHER_CHECK(bad["errors"].get<int>() >= 1);
    AETHER_CHECK(bad["diagnostics"][0]["code"].get<std::string>().rfind("BP", 0) == 0);
    AETHER_CHECK(h.Call("bp_validate", {{"definition", "{ not json"}}).is_error);
    AETHER_CHECK(h.Call("bp_validate").is_error);
    AETHER_CHECK(h.Call("bp_validate", {{"definition", {{"graphs", 5}}}}).is_error);

    Json compiled = h.Call("bp_compile", {{"definition", sample}, {"disassemble", true}}).value;
    AETHER_CHECK(compiled["ok"] == true);
    bool begin = false, hit = false;
    for (const Json& e : compiled["events"]) {
        begin = begin || e == "Event.BeginPlay";
        hit = hit || e == "Event.Custom:Hit";
    }
    AETHER_CHECK(begin && hit);
    AETHER_CHECK(compiled["functions"][0]["instructions"].get<int>() > 0);
    AETHER_CHECK(!compiled["functions"][0]["listing"].get<std::string>().empty());
    AETHER_CHECK(h.Call("bp_compile", {{"definition", broken}}).value["ok"] == false);

    // --- run it
    Json ran = h.Call("bp_run", {{"definition", sample}}).value;
    AETHER_CHECK(ran["ok"] == true);
    AETHER_CHECK(ran["prints"] == Json::array({"Hello from BeginPlay"}));
    AETHER_CHECK(ran["variables"]["Count"] == 5);
    Json hits = h.Call("bp_run", {{"definition", sample}, {"events", {{{"event", "Event.Custom:Hit"}}, {{"event", "Event.Custom:Hit"}}}}}).value;
    AETHER_CHECK(hits["variables"]["Count"] == 7);
    AETHER_CHECK(hits["prints"].size() == 3);
    AETHER_CHECK(hits["prints"][2] == "Hit!");
    AETHER_CHECK(h.Call("bp_run", {{"definition", sample}, {"begin_play", false}}).value["variables"]["Count"] == 0);
    Json missing_event = h.Call("bp_run", {{"definition", sample}, {"events", {{{"event", "Event.Custom:Nope"}}}}}).value;
    AETHER_CHECK(missing_event["events"][1]["ran"] == false);
    AETHER_CHECK(h.Call("bp_run", {{"definition", sample}, {"events", {{{"nope", 1}}}}}).is_error);
    AETHER_CHECK(h.Call("bp_run", {{"definition", sample}, {"ticks", 99999}}).is_error);
    AETHER_CHECK(h.Call("bp_run", {{"definition", sample}, {"events", {{{"event", "Event.Custom:Hit"}, {"args", {{{"a", 1}}}}}}}}).is_error);
    Json refused = h.Call("bp_run", {{"definition", broken}}).value;
    AETHER_CHECK(refused["ok"] == false);
    AETHER_CHECK(refused["stage"] == "compile");

    // Ticks: a Blueprint that counts frames.
    const Json ticker = {{"$type", "Blueprint"}, {"$version", 1},
                         {"variables", {{{"name", "Ticks"}, {"type", "int"}, {"default", 0}}}},
                         {"graphs", {{{"name", "EventGraph"}, {"kind", "EventGraph"},
                                      {"nodes", {{{"id", 1}, {"type", "Event.Tick"}, {"pos", {0, 0}}},
                                                 {{"id", 2}, {"type", "Var.Set:Ticks"}, {"pos", {200, 0}}},
                                                 {{"id", 3}, {"type", "Math.Add:int"}, {"pos", {0, 200}}, {"defaults", {{"b", 1}}}},
                                                 {{"id", 4}, {"type", "Var.Get:Ticks"}, {"pos", {-200, 200}}}}},
                                      {"links", {{{"from", {1, "then"}}, {"to", {2, "exec"}}},
                                                 {{"from", {4, "value"}}, {"to", {3, "a"}}},
                                                 {{"from", {3, "result"}}, {"to", {2, "value"}}}}}}}}};
    Json ticked = h.Call("bp_run", {{"definition", ticker}, {"ticks", 30}}).value;
    AETHER_CHECK(ticked["ok"] == true);
    AETHER_CHECK(ticked["variables"]["Ticks"] == 30);
    AETHER_CHECK(std::fabs(ticked["game_time"].get<double>() - 0.5) < 0.01);

    // A runaway loop hits the instruction budget (BP202) instead of hanging.
    const Json runaway = {{"$type", "Blueprint"}, {"$version", 1},
                          {"graphs", {{{"name", "EventGraph"}, {"kind", "EventGraph"},
                                       {"nodes", {{{"id", 1}, {"type", "Event.BeginPlay"}, {"pos", {0, 0}}},
                                                  {{"id", 2}, {"type", "Flow.ForLoop"}, {"pos", {200, 0}}, {"defaults", {{"first", 0}, {"last", 100000000}}}}}},
                                       {"links", {{{"from", {1, "then"}}, {"to", {2, "exec"}}}}}}}}};
    Json budget = h.Call("bp_run", {{"definition", runaway}, {"instruction_budget", 5000}}).value;
    AETHER_CHECK(budget["ok"] == false);
    AETHER_CHECK(budget["errors"][0]["code"] == "BP202");

    // --- Blueprints in a project go through the kit tools, and the Blueprint tools read them by path
    const stdfs::path dir = stdfs::temp_directory_path() / "aether_mcp_bp_test";
    stdfs::remove_all(dir);
    stdfs::create_directories(dir);
    ProjectPaths paths;
    std::string error;
    AETHER_CHECK(CreateProject(dir, "McpBp", &paths, &error));
    AETHER_CHECK(h.Call("bp_validate", {{"asset", "Blueprints/BP_Demo.abp"}}).is_error); // no project yet
    AETHER_CHECK(!h.Call("project_open", {{"project_file", paths.file.string()}}).is_error);
    AETHER_CHECK(h.Call("kit_put", {{"type", "blueprint"}, {"path", "Blueprints/BP_Broken"}, {"definition", broken}}).is_error);
    AETHER_CHECK(!stdfs::exists(paths.content / "Blueprints" / "BP_Broken.abp"));
    Harness::Result put = h.Call("kit_put", {{"type", "blueprint"}, {"path", "Blueprints/BP_Demo"}, {"definition", sample}});
    AETHER_CHECK(!put.is_error);
    AETHER_CHECK(put.value["path"] == "Blueprints/BP_Demo.abp");
    AETHER_CHECK(h.Call("kit_list", {{"type", "blueprint"}}).value[0]["valid"] == true);
    AETHER_CHECK(h.Call("kit_get", {{"asset", "Blueprints/BP_Demo.abp"}}).value["definition"]["variables"][0]["name"] == "Count");
    AETHER_CHECK(h.Call("bp_validate", {{"asset", "Blueprints/BP_Demo.abp"}}).value["ok"] == true);
    AETHER_CHECK(h.Call("bp_run", {{"asset", "Blueprints/BP_Demo.abp"}}).value["variables"]["Count"] == 5);
    AETHER_CHECK(h.Call("bp_validate", {{"asset", "Nothing.abp"}}).is_error);
    AETHER_CHECK(h.Call("kit_check").value["ok"] == true);
    // A hand-edited, broken Blueprint shows up as a problem.
    AETHER_CHECK(!h.Call("content_write", {{"path", "Blueprints/BP_Bad.abp"}, {"text", broken.dump()}}).is_error);
    Json check = h.Call("kit_check").value;
    AETHER_CHECK(check["ok"] == false);
    AETHER_CHECK(check["problems"][0]["problem"].get<std::string>().find("blueprint.") != std::string::npos);
    stdfs::remove_all(dir);
}
