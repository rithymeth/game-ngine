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
    Json pointed = h.Call("game_input", {{"mouse", {{"x", 100}, {"y", 80}, {"down", true}}}, {"touches", Json::array({Json{{"id", 3}, {"x", 10}, {"y", 20}, {"phase", "down"}}})}}).value;
    AETHER_CHECK(pointed["pointers"].size() == 2 && pointed["pointers"][0]["slot"] == 0 && pointed["pointers"][0]["down"] == true && pointed["pointers"][0]["x"] == 100);
    AETHER_CHECK(pointed["pointers"][1]["slot"] == 1 && pointed["pointers"][1]["y"] == 20);
    Json lifted = h.Call("game_input", {{"touches", Json::array({Json{{"id", 3}, {"x", 10}, {"y", 20}, {"phase", "up"}}})}}).value;
    AETHER_CHECK(lifted["pointers"].size() == 1);
    AETHER_CHECK(h.Call("game_input", {{"touches", Json::array({Json{{"id", 3}, {"x", 1}, {"y", 1}, {"phase", "wiggle"}}})}}).is_error);
    AETHER_CHECK(h.Call("game_input", {{"mouse", {{"x", "far"}}}}).is_error);

    AETHER_CHECK(!h.Call("game_unload").is_error);
    AETHER_CHECK(h.Call("game_state").is_error);
    stdfs::remove_all(dir);
}

#include "aether/player/game.h"

// print() in a game's scripts reaches the log, and log_recent reads it back with filters.
AETHER_TEST(Mcp_LogRecentShowsWhatScriptsPrint) {
    if (!aether::player::Game::HasScripting()) return;
    namespace stdfs = std::filesystem;
    const stdfs::path dir = stdfs::temp_directory_path() / "aether_mcp_log_test";
    stdfs::remove_all(dir);
    stdfs::create_directories(dir);
    const std::string script_guid = assets::ToString(assets::NewAssetGuid());
    const Json manifest = {{"$type", "CookManifest"}, {"$version", 1}, {"project", "McpLog"}, {"configuration", "Development"},
                           {"startup_scene", "Scenes/start.ascene"}, {"fixed_timestep_hz", 60.0}, {"gravity", {0.0, -9.81, 0.0}},
                           {"layers", {"Default"}}, {"collision_matrix", Json::array()},
                           {"assets", Json::array({{{"guid", assets::ToString(assets::NewAssetGuid())}, {"path", "Scenes/start.ascene"}, {"importer", "Scene"}},
                                                   {{"guid", script_guid}, {"path", "Scripts/Talk.luau"}, {"importer", "Script"}}})},
                           {"files", Json::array()}};
    const Json scene = {{"$type", "Scene"}, {"$version", 1},
                        {"entities", Json::array({{{"components", {{"ScriptComponent", {{"script", script_guid}}}}}}})}};
    pak::PakWriter writer;
    writer.Add("Manifest.json", manifest.dump());
    writer.Add("Content/Scenes/start.ascene", scene.dump());
    writer.Add("Content/Scripts/Talk.luau", std::string("local T = {}\nfunction T:OnStart() print('mcp-log-marker', 42) end\nreturn T\n"));
    const stdfs::path file = dir / "game.apak";
    AETHER_CHECK(writer.Write(file.string()));

    Harness h;
    RegisterGameTools(h.server);
    AETHER_CHECK(!h.Call("log_clear").is_error);
    AETHER_CHECK(!h.Call("game_load", {{"paks", {file.string()}}, {"user_dir", (dir / "user").string()}}).is_error);
    AETHER_CHECK(!h.Call("game_step", {{"frames", 2}}).is_error);

    Json found = h.Call("log_recent", {{"category", "Script"}, {"contains", "mcp-log-marker"}}).value;
    AETHER_CHECK(found["count"] == 1 && found["lines"][0]["message"] == "mcp-log-marker\t42" && found["lines"][0]["level"] == "INFO");
    AETHER_CHECK(h.Call("log_recent", {{"category", "Script"}, {"level", "Error"}}).value["count"] == 0);
    const f64 when = found["lines"][0]["time"].get<f64>();
    AETHER_CHECK(h.Call("log_recent", {{"since", when + 1000.0}}).value["count"] == 0);
    AETHER_CHECK(h.Call("log_recent", {{"limit", 1}}).value["lines"].size() == 1);
    AETHER_CHECK(h.Call("log_recent", {{"level", "Loud"}}).is_error);
    AETHER_CHECK(h.Call("log_recent", {{"limit", 0}}).is_error);
    AETHER_CHECK(!h.Call("log_clear").is_error);
    AETHER_CHECK(h.Call("log_recent", {{"contains", "mcp-log-marker"}}).value["count"] == 0);
    h.Call("game_unload");
    stdfs::remove_all(dir);
}

#include "build_tools.h"
#include "subprocess.h"

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
#include "animation_tools.h"
#include "ui_tools.h"
#include "save_tools.h"
#include "streaming_tools.h"
#include "material_tools.h"
#include "prefab_tools.h"
#include "aether/scene/prefab.h"
#include "aether/renderer/material.h"
#include "aether/renderer/material_instance.h"
#include "aether/save/save_system.h"
#include "aether/save/settings.h"
#include "aether/ui/basic.h"
#include "aether/ui/layout_file.h"
#include "aether/ui/panels.h"
#include "aether/ui/theme.h"
#include "aether/ui/animation.h"
#include "aether/ui/controls.h"
#include "aether/animation/anim_graph.h"
#include "script_tools.h"
#include "tilemap_tools.h"
#include "sequence_tools.h"
#include "loc_tools.h"
#include "nav_tools.h"
#include "terrain_tools.h"
#include "vfx_tools.h"
#include "aether/assets/image.h"
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

AETHER_TEST(Mcp_LocToolsFormatEditGatherValidateAndRoundTrip) {
    namespace stdfs = std::filesystem;
    Harness h;
    auto project = MakeAssetHost();
    RegisterAssetTools(h.server, project);
    RegisterLocTools(h.server, project);

    // --- formatting and languages need no project
    AETHER_CHECK(h.Call("loc_format", {{"pattern", "{count} coin{count|s}"}, {"args", {{"count", 1}}}}).value["text"] == "1 coin");
    AETHER_CHECK(h.Call("loc_format", {{"pattern", "{count} coin{count|s}"}, {"args", {{"count", 2}}}}).value["text"] == "2 coins");
    Json forms = h.Call("loc_format", {{"pattern", "{n|one:apple;other:apples}"}, {"args", {{"n", 1}}}}).value;
    AETHER_CHECK(forms["text"] == "apple");
    AETHER_CHECK(forms["plural_categories"]["n"] == "one");
    AETHER_CHECK(h.Call("loc_format", {{"pattern", "{n|one:яблоко;few:яблока;many:яблок;other:яблока}"}, {"language", "ru"}, {"args", {{"n", 5}}}}).value["text"] == "яблок");
    Json hello = h.Call("loc_format", {{"pattern", "Hi {name}!"}, {"args", {{"name", "Ada"}}}}).value;
    AETHER_CHECK(hello["text"] == "Hi Ada!");
    AETHER_CHECK(hello["problems"].empty());
    AETHER_CHECK(!h.Call("loc_format", {{"pattern", "Hi {name}!"}}).value["problems"].empty()); // an argument that was not given
    AETHER_CHECK(!h.Call("loc_format", {{"pattern", "Hi {name"}, {"args", {{"name", "x"}}}}).value["problems"].empty()); // an unclosed brace
    AETHER_CHECK(h.Call("loc_format", {{"pattern", "x"}, {"args", {{"a", Json::array()}}}}).is_error);
    AETHER_CHECK(h.Call("loc_format", {{"pattern", "x"}, {"language", ""}}).is_error);

    Json lang = h.Call("loc_language", {{"language", "pt_br"}}).value;
    AETHER_CHECK(lang["language"] == "pt-BR");
    AETHER_CHECK(lang["fallback_chain"] == Json::array({"pt-BR", "pt", "en"}));
    AETHER_CHECK(lang["right_to_left"] == false);
    AETHER_CHECK(h.Call("loc_language", {{"language", "ar"}}).value["right_to_left"] == true);
    Json ru = h.Call("loc_language", {{"language", "ru"}, {"numbers", {1, 2, 5, 21}}}).value["plural_categories"];
    AETHER_CHECK(ru[0]["category"] == "one" && ru[1]["category"] == "few" && ru[2]["category"] == "many" && ru[3]["category"] == "one");
    AETHER_CHECK(h.Call("loc_language", {{"language", "ja"}, {"numbers", {1, 2}}}).value["plural_categories"][1]["category"] == "other");

    Json pseudo = h.Call("loc_pseudo", {{"texts", {"Hello {name}!", "Quit"}}}).value;
    AETHER_CHECK(pseudo["language"] == "qps-Ploc");
    const std::string first = pseudo["results"][0]["pseudo"];
    AETHER_CHECK(first.front() == '[' && first.back() == ']');
    AETHER_CHECK(first.find("{name}") != std::string::npos); // placeholders survive, so it still formats
    AETHER_CHECK(pseudo["results"][0]["bytes_after"].get<int>() > pseudo["results"][0]["bytes_before"].get<int>());
    AETHER_CHECK(h.Call("loc_pseudo", {{"texts", {"x"}}, {"expansion", 9}}).is_error);
    AETHER_CHECK(h.Call("loc_pseudo").is_error);
    AETHER_CHECK(h.Call("loc_status").is_error); // needs a project

    // --- a project's string table
    const stdfs::path dir = stdfs::temp_directory_path() / "aether_mcp_loc_test";
    stdfs::remove_all(dir);
    stdfs::create_directories(dir);
    ProjectPaths paths;
    std::string error;
    AETHER_CHECK(CreateProject(dir, "McpLoc", &paths, &error));
    AETHER_CHECK(!h.Call("project_open", {{"project_file", paths.file.string()}}).is_error);
    AETHER_CHECK(h.Call("loc_status").value["keys"] == 0);

    AETHER_CHECK(!h.Call("loc_set", {{"key", "menu.play"}, {"translations", {{"en", "Play"}, {"fr", "Jouer"}}}}).is_error);
    AETHER_CHECK(stdfs::exists(paths.content / "Localization" / "Strings.astrings")); // made on first use
    AETHER_CHECK(h.Call("loc_get", {{"key", "menu.play"}}).value["translations"]["fr"] == "Jouer");
    AETHER_CHECK(!h.Call("loc_set", {{"key", "items.count"}, {"translations", {{"en", "{count} item{count|s}"}, {"fr", "{n} objets"}, {"pt_br", "{count} itens"}}}}).is_error);
    AETHER_CHECK(!h.Call("loc_set", {{"key", "bad"}, {"translations", {{"en", "Hello"}, {"de", "Hallo {name"}}}}).is_error);
    AETHER_CHECK(h.Call("loc_get", {{"key", "items.count"}}).value["translations"].contains("pt-BR")); // tags are normalized
    AETHER_CHECK(h.Call("loc_set", {{"key", "menu.play"}, {"translations", {{"", "x"}}}}).is_error);
    AETHER_CHECK(h.Call("loc_set", {{"key", "menu.play"}, {"translations", {{"en", 5}}}}).is_error);
    AETHER_CHECK(h.Call("loc_get", {{"key", "nothing"}}).is_error);

    Json status = h.Call("loc_status", {{"missing_limit", 5}}).value;
    AETHER_CHECK(status["keys"] == 3);
    AETHER_CHECK(status["tables"].size() == 1);
    for (const Json& l : status["languages"]) {
        if (l["language"] == "fr") AETHER_CHECK(l["translated"] == 2 && l["missing"] == 1);
        if (l["language"] == "pt-BR") AETHER_CHECK(l["fallback_chain"] == Json::array({"pt-BR", "pt", "en"}));
    }

    // Validation: placeholders must match the source text, and messages must be well-formed.
    Json check = h.Call("loc_validate").value;
    AETHER_CHECK(check["ok"] == false);
    bool mismatch = false, unclosed = false;
    for (const Json& pr : check["problems"]) {
        if (pr["key"] == "items.count" && pr["language"] == "fr") {
            mismatch = true;
            AETHER_CHECK(pr["missing"] == Json::array({"count"}));
            AETHER_CHECK(pr["extra"] == Json::array({"n"}));
        }
        if (pr["key"] == "bad" && pr["language"] == "de") unclosed = true;
    }
    AETHER_CHECK(mismatch && unclosed);
    AETHER_CHECK(!h.Call("loc_set", {{"key", "items.count"}, {"translations", {{"fr", "{count} objet{count|s}"}}}}).is_error);
    AETHER_CHECK(!h.Call("loc_set", {{"key", "bad"}, {"translations", {{"de", ""}}}}).is_error); // removes the broken one
    AETHER_CHECK(h.Call("loc_validate").value["ok"] == true);

    // --- the content's strings against the table
    const Json scene = {{"$type", "Scene"}, {"$version", 1},
                        {"entities", Json::array({{{"components", {{"Label", {{"text_key", "menu.play"}, {"text", "Play"}}}}}},
                                                  {{"components", {{"Label", {{"text_key", "menu.quit"}, {"text", "Quit"}}}}}}})}};
    AETHER_CHECK(!h.Call("content_write", {{"path", "Scenes/menu.ascene"}, {"text", scene.dump()}}).is_error);
    Json gathered = h.Call("loc_gather").value;
    AETHER_CHECK(gathered["found"] == 2);
    AETHER_CHECK(gathered["new_keys"] == Json::array({"menu.quit"}));
    bool unused_has_items = false;
    for (const Json& k : gathered["unused_keys"]) unused_has_items = unused_has_items || k == "items.count";
    AETHER_CHECK(unused_has_items);

    Json dry = h.Call("loc_sync").value; // reports only
    AETHER_CHECK(dry["ok"] == true);
    AETHER_CHECK(dry["written"] == false);
    AETHER_CHECK(dry["added"] == 1);
    AETHER_CHECK(h.Call("loc_get", {{"key", "menu.quit"}}).is_error); // nothing was written
    const stdfs::path po_dir = dir / "po";
    Json applied = h.Call("loc_sync", {{"write", true}, {"po_out", po_dir.string()}, {"languages", {"fr"}}}).value;
    AETHER_CHECK(applied["written"] == true);
    AETHER_CHECK(applied["po_written"] == 1);
    AETHER_CHECK(h.Call("loc_get", {{"key", "menu.quit"}}).value["translations"]["en"] == "Quit");
    AETHER_CHECK(stdfs::exists(po_dir / "fr.po"));
    AETHER_CHECK(h.Call("loc_sync", {{"write", true}}).value["added"] == 0); // a second run has nothing to add

    // --- .po files for translators
    const stdfs::path po_file = dir / "fr_export.po";
    AETHER_CHECK(!h.Call("loc_export_po", {{"language", "fr"}, {"path", po_file.string()}}).is_error);
    std::string po;
    {
        std::ifstream in(po_file, std::ios::binary);
        po.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    AETHER_CHECK(po.find("msgctxt \"menu.play\"") != std::string::npos);
    AETHER_CHECK(po.find("msgstr \"Jouer\"") != std::string::npos);
    const std::size_t quit = po.find("msgid \"Quit\"");
    AETHER_CHECK(quit != std::string::npos);
    const std::size_t str = po.find("msgstr \"\"", quit);
    AETHER_CHECK(str != std::string::npos);
    po.replace(str, 9, "msgstr \"Quitter\"");
    {
        std::ofstream out(po_file, std::ios::binary | std::ios::trunc);
        out << po;
    }
    Json imported = h.Call("loc_import_po", {{"path", po_file.string()}}).value; // language from the file's own header
    AETHER_CHECK(imported["language"] == "fr");
    AETHER_CHECK(imported["translations_after"].get<int>() == imported["translations_before"].get<int>() + 1);
    AETHER_CHECK(h.Call("loc_get", {{"key", "menu.quit"}}).value["translations"]["fr"] == "Quitter");
    AETHER_CHECK(h.Call("loc_import_po", {{"path", (dir / "none.po").string()}}).is_error);
    AETHER_CHECK(h.Call("loc_export_po", {{"language", "fr"}}).is_error);

    // --- pseudo-localize a key from the table, then remove keys
    Json from_key = h.Call("loc_pseudo", {{"key", "items.count"}}).value;
    AETHER_CHECK(from_key["results"][0]["pseudo"].get<std::string>().find("{count|s}") != std::string::npos);
    AETHER_CHECK(h.Call("loc_pseudo", {{"key", "no.such.key"}}).is_error);
    AETHER_CHECK(!h.Call("loc_remove", {{"key", "menu.quit"}}).is_error);
    AETHER_CHECK(h.Call("loc_remove", {{"key", "menu.quit"}}).is_error);
    AETHER_CHECK(h.Call("loc_status").value["keys"] == 3);

    // Two tables: the tools ask which one.
    AETHER_CHECK(!h.Call("loc_set", {{"key", "ui.back"}, {"translations", {{"en", "Back"}}}, {"strings", "Localization/Extra"}}).is_error);
    AETHER_CHECK(h.Call("loc_status").value["tables"].size() == 2);
    AETHER_CHECK(h.Call("loc_set", {{"key", "ui.next"}, {"translations", {{"en", "Next"}}}}).is_error);
    AETHER_CHECK(h.Call("loc_get", {{"key", "ui.back"}}).value["translations"]["en"] == "Back"); // the merged view sees both
    stdfs::remove_all(dir);
}

AETHER_TEST(Mcp_SequenceToolsValidateEvaluateAndPlay) {
    namespace stdfs = std::filesystem;
    Harness h;
    auto project = MakeAssetHost();
    RegisterAssetTools(h.server, project);
    RegisterKitTools(h.server, project);
    RegisterSequenceTools(h.server, project);

    // --- the sample, validated and read without a project
    const Json sample = h.Call("kit_schema", {{"type", "sequence"}}).value["sample"];
    Json valid = h.Call("seq_validate", {{"definition", sample}}).value;
    AETHER_CHECK(valid["ok"] == true);
    AETHER_CHECK(valid["duration"] == 4);
    AETHER_CHECK(valid["tracks"] == 3);
    AETHER_CHECK(valid["by_type"]["transform"] == 1 && valid["by_type"]["event"] == 1 && valid["by_type"]["fade"] == 1);

    Json no_tracks = {{"$type", "LevelSequence"}, {"$version", 1}, {"name", "Empty"}, {"tracks", Json::array()}};
    Json empty = h.Call("seq_validate", {{"definition", no_tracks}}).value;
    AETHER_CHECK(empty["ok"] == false);
    AETHER_CHECK(empty["diagnostics"][0]["code"] == "SQ001");
    Json duplicate = sample;
    duplicate["tracks"][1]["id"] = duplicate["tracks"][0]["id"];
    bool dup_found = false;
    const Json dup_verdict = h.Call("seq_validate", {{"definition", duplicate}}).value; // kept alive: the loop reads into it
    for (const Json& d : dup_verdict["diagnostics"]) dup_found = dup_found || d["code"] == "SQ005";
    AETHER_CHECK(dup_found);
    AETHER_CHECK(h.Call("seq_validate", {{"definition", "{ not json"}}).is_error);
    AETHER_CHECK(h.Call("seq_validate").is_error);
    AETHER_CHECK(h.Call("seq_validate", {{"definition", sample}, {"scene", "Scenes/x.ascene"}}).is_error); // a scene needs a project

    // A Property track's component and field are checked against reflection.
    auto with_property = [&](const char* component, const char* field) {
        Json s = sample;
        const Json keys = Json::array({Json{{"time", 0}, {"value", 0}}, Json{{"time", 1}, {"value", 1}}});
        const Json channel = {{"name", "value"}, {"keys", keys}};
        const Json track = {{"id", "prop"}, {"name", "Prop"}, {"type", "property"}, {"binding", "00000000-0000-4000-8000-000000000001"},
                            {"component", component}, {"field", field}, {"channels", Json::array({channel})}};
        s["tracks"].push_back(track);
        return s;
    };
    Json bad_component = h.Call("seq_validate", {{"definition", with_property("NoSuchComponent", "x")}}).value;
    AETHER_CHECK(bad_component["ok"] == false);
    AETHER_CHECK(bad_component["problems"][0]["problem"].get<std::string>().find("NoSuchComponent") != std::string::npos);
    Json bad_field = h.Call("seq_validate", {{"definition", with_property("Transform", "wobble")}}).value;
    AETHER_CHECK(bad_field["ok"] == false);
    AETHER_CHECK(h.Call("seq_validate", {{"definition", with_property("Transform", "position")}}).value["problems"].empty());

    // --- sampling: a pure function of time
    Json at = h.Call("seq_evaluate", {{"definition", sample}, {"times", {0, 1, 2, 4}}}).value["samples"];
    AETHER_CHECK(at.size() == 4);
    AETHER_CHECK(std::fabs(at[0]["tracks"]["door-move"]["position"]["y"].get<double>() - 0.0) < 1e-6);
    AETHER_CHECK(std::fabs(at[1]["tracks"]["door-move"]["position"]["y"].get<double>() - 1.5) < 1e-6);
    AETHER_CHECK(std::fabs(at[2]["tracks"]["door-move"]["position"]["y"].get<double>() - 3.0) < 1e-6);
    AETHER_CHECK(std::fabs(at[3]["tracks"]["door-move"]["position"]["y"].get<double>() - 3.0) < 1e-6); // holds the last key
    AETHER_CHECK(!at[0]["tracks"]["door-move"]["position"].contains("x")); // an axis with no keys is left alone
    AETHER_CHECK(std::fabs(at[0]["tracks"]["fade"]["amount"].get<double>() - 1.0) < 1e-6);
    AETHER_CHECK(std::fabs(at[1]["tracks"]["fade"]["amount"].get<double>() - 0.0) < 1e-6);
    AETHER_CHECK(!at[0]["tracks"].contains("creak")); // events are moments, not values
    Json only = h.Call("seq_evaluate", {{"definition", sample}, {"times", {2}}, {"tracks", {"fade"}}}).value["samples"][0]["tracks"];
    AETHER_CHECK(only.size() == 1 && only.contains("fade"));
    Json defaults = h.Call("seq_evaluate", {{"definition", sample}}).value["samples"];
    AETHER_CHECK(defaults.size() == 5);
    AETHER_CHECK(h.Call("seq_evaluate", {{"definition", sample}, {"times", Json::array()}}).is_error);
    AETHER_CHECK(h.Call("seq_evaluate", {{"definition", sample}, {"times", {"soon"}}}).is_error);
    Json muted = sample;
    muted["tracks"][0]["mute"] = true;
    AETHER_CHECK(!h.Call("seq_evaluate", {{"definition", muted}, {"times", {1}}}).value["samples"][0]["tracks"].contains("door-move"));

    // --- what it fires, in time order
    Json moments = h.Call("seq_events", {{"definition", sample}}).value["moments"];
    AETHER_CHECK(moments.size() == 3); // the creak and two fade keys
    for (std::size_t i = 1; i < moments.size(); ++i) AETHER_CHECK(moments[i - 1]["time"].get<double>() <= moments[i]["time"].get<double>());
    bool creak = false;
    for (const Json& m : moments) creak = creak || (m["kind"] == "event" && m["name"] == "door_creak" && m["payload"] == "loud" && m["time"] == 1);
    AETHER_CHECK(creak);
    AETHER_CHECK(h.Call("seq_events", {{"definition", sample}, {"from", 0.5}, {"to", 1.5}}).value["moments"].size() == 2); // the creak and the fade's end
    Json rich = sample;
    rich["tracks"].push_back({{"id", "snd"}, {"name", "Sound"}, {"type", "audio"}, {"binding", "00000000-0000-4000-8000-000000000001"},
                              {"audio", {{{"time", 0.5}, {"cue", "Audio/boom.acue"}, {"action", "play"}}}}});
    rich["tracks"].push_back({{"id", "extra"}, {"name", "Extra"}, {"type", "subsequence"}, {"subs", {{{"time", 1}, {"duration", 2}, {"sequence", "Sequences/Other.asequence"}}}}});
    Json all = h.Call("seq_events", {{"definition", rich}}).value["moments"];
    bool saw_audio = false, saw_start = false, saw_end = false;
    for (const Json& m : all) {
        saw_audio = saw_audio || (m["kind"] == "audio" && m["cue"] == "Audio/boom.acue" && m["time"] == 0.5);
        saw_start = saw_start || (m["kind"] == "subsequence_start" && m["time"] == 1);
        saw_end = saw_end || (m["kind"] == "subsequence_end" && m["time"] == 3);
    }
    AETHER_CHECK(saw_audio && saw_start && saw_end);

    // --- in a project: files, references, loops
    const stdfs::path dir = stdfs::temp_directory_path() / "aether_mcp_seq_test";
    stdfs::remove_all(dir);
    stdfs::create_directories(dir);
    ProjectPaths paths;
    std::string error;
    AETHER_CHECK(CreateProject(dir, "McpSeq", &paths, &error));
    AETHER_CHECK(h.Call("seq_validate", {{"asset", "Sequences/Door.asequence"}}).is_error); // no project yet
    AETHER_CHECK(!h.Call("project_open", {{"project_file", paths.file.string()}}).is_error);
    AETHER_CHECK(h.Call("kit_put", {{"type", "sequence"}, {"path", "Sequences/Broken"}, {"definition", no_tracks}}).is_error);
    AETHER_CHECK(!stdfs::exists(paths.content / "Sequences" / "Broken.asequence"));
    Harness::Result put = h.Call("kit_put", {{"type", "sequence"}, {"path", "Sequences/Door"}, {"definition", sample}});
    AETHER_CHECK(!put.is_error);
    AETHER_CHECK(put.value["path"] == "Sequences/Door.asequence");
    AETHER_CHECK(h.Call("kit_list", {{"type", "sequence"}}).value[0]["valid"] == true);
    AETHER_CHECK(h.Call("kit_get", {{"asset", "Sequences/Door.asequence"}}).value["definition"]["name"] == "Door opens");
    AETHER_CHECK(h.Call("seq_validate", {{"asset", "Sequences/Door.asequence"}}).value["ok"] == true);
    AETHER_CHECK(h.Call("seq_evaluate", {{"asset", "Sequences/Door.asequence"}, {"times", {1}}}).value["samples"][0]["tracks"]["door-move"]["position"]["y"] == 1.5);
    AETHER_CHECK(h.Call("seq_events", {{"asset", "Sequences/Missing.asequence"}}).is_error);

    // Names are checked against the project: cues, prefabs, sub-sequences.
    Json refs = sample;
    refs["tracks"].push_back({{"id", "snd"}, {"name", "Sound"}, {"type", "audio"}, {"binding", "00000000-0000-4000-8000-000000000001"},
                              {"audio", {{{"time", 0.5}, {"cue", "Audio/boom.acue"}, {"action", "play"}}}}});
    refs["tracks"].push_back({{"id", "pre"}, {"name", "Prefab"}, {"type", "spawn"}, {"binding", "00000000-0000-4000-8000-000000000001"},
                              {"spawns", {{{"time", 0.5}, {"prefab", "Prefabs/Gone.aprefab"}}}}});
    refs["tracks"].push_back({{"id", "sub"}, {"name", "Sub"}, {"type", "subsequence"}, {"subs", {{{"time", 1}, {"duration", 2}, {"sequence", "Sequences/Nowhere.asequence"}}}}});
    Json ref_check = h.Call("seq_validate", {{"definition", refs}}).value;
    AETHER_CHECK(ref_check["ok"] == false);
    AETHER_CHECK(ref_check["problems"].size() == 3);

    // Two sequences that play each other never finish.
    auto plays = [&](const char* other) {
        Json s = sample;
        s["tracks"].push_back({{"id", "sub"}, {"name", "Sub"}, {"type", "subsequence"}, {"subs", {{{"time", 0}, {"duration", 2}, {"sequence", other}}}}});
        return s;
    };
    AETHER_CHECK(!h.Call("kit_put", {{"type", "sequence"}, {"path", "Sequences/Ping"}, {"definition", plays("Sequences/Pong.asequence")}}).is_error);
    AETHER_CHECK(!h.Call("kit_put", {{"type", "sequence"}, {"path", "Sequences/Pong"}, {"definition", plays("Sequences/Ping.asequence")}}).is_error);
    Json loop = h.Call("seq_validate", {{"asset", "Sequences/Ping.asequence"}}).value;
    AETHER_CHECK(loop["ok"] == false);
    bool says_loop = false;
    for (const Json& pr : loop["problems"]) says_loop = says_loop || pr["problem"].get<std::string>().find("plays itself") != std::string::npos;
    AETHER_CHECK(says_loop);
    AETHER_CHECK(h.Call("seq_validate", {{"asset", "Sequences/Door.asequence"}}).value["ok"] == true);

    // Bindings are checked against a scene's entities.
    auto scene_with = [](const char* guid) {
        return Json{{"$type", "Scene"}, {"$version", 1},
                    {"entities", Json::array({{{"guid", guid}, {"components", {{"Transform", {{"position", {0, 0, 0}}, {"rotation", {0, 0, 0, 1}}, {"scale", {1, 1, 1}}}}}}}})}};
    };
    AETHER_CHECK(!h.Call("content_write", {{"path", "Scenes/start.ascene"}, {"text", scene_with("00000000-0000-4000-8000-000000000002").dump()}}).is_error);
    Json unbound = h.Call("seq_validate", {{"asset", "Sequences/Door.asequence"}, {"scene", "Scenes/start.ascene"}}).value;
    AETHER_CHECK(unbound["ok"] == false);
    AETHER_CHECK(unbound["problems"][0]["problem"].get<std::string>().find("not in Scenes/start.ascene") != std::string::npos);
    AETHER_CHECK(!h.Call("content_write", {{"path", "Scenes/start.ascene"}, {"text", scene_with("00000000-0000-4000-8000-000000000001").dump()}}).is_error);
    AETHER_CHECK(h.Call("seq_validate", {{"asset", "Sequences/Door.asequence"}, {"scene", "Scenes/start.ascene"}}).value["ok"] == true);
    AETHER_CHECK(h.Call("seq_validate", {{"asset", "Sequences/Door.asequence"}, {"scene", "Sequences/Door.asequence"}}).is_error); // not a scene

    // --- cook it and play it in the hosted game
    AETHER_CHECK(!h.Call("project_set", {{"startup_scene", "Scenes/start.ascene"}}).is_error);
    Harness::Result cooked = h.Call("asset_cook", {{"output_dir", (dir / "Cooked").string()}, {"always_cook", {"Sequences/Door.asequence"}}});
    AETHER_CHECK(!cooked.is_error);
    AETHER_CHECK(cooked.value["ok"] == true);
    RegisterGameTools(h.server);
    AETHER_CHECK(h.Call("game_sequences").is_error); // no game yet
    AETHER_CHECK(!h.Call("game_load", {{"paks", {cooked.value["pak_file"]}}, {"user_dir", (dir / "user").string()}}).is_error);
    h.Call("game_step", {{"frames", 2}});
    AETHER_CHECK(h.Call("game_sequences").value["has_sequence_system"] == true);
    AETHER_CHECK(h.Call("game_sequences").value["sequences"].empty());
    AETHER_CHECK(h.Call("game_play_sequence", {{"sequence", "Sequences/Nowhere.asequence"}}).is_error);
    Harness::Result started = h.Call("game_play_sequence", {{"sequence", "Sequences/Door.asequence"}});
    AETHER_CHECK(!started.is_error);
    const std::string player = started.value["entity"];
    h.Call("game_step", {{"frames", 60}}); // a second: the door is half way up
    const std::string door = "00000000-0000-4000-8000-000000000001";
    const double halfway = h.Call("game_get_entity", {{"entity", door}}).value["components"]["Transform"]["position"][1].get<double>();
    AETHER_CHECK(halfway > 1.0 && halfway < 2.0);
    Json state = h.Call("game_sequences").value;
    AETHER_CHECK(state["sequences"].size() == 1);
    AETHER_CHECK(state["sequences"][0]["playing"] == true);
    AETHER_CHECK(!h.Call("game_sequence_control", {{"entity", player}, {"action", "set_rate"}, {"value", 2}}).is_error);
    AETHER_CHECK(h.Call("game_sequence_control", {{"entity", player}, {"action", "explode"}}).is_error);
    AETHER_CHECK(h.Call("game_sequence_control", {{"entity", door}, {"action", "play"}}).is_error); // no SequenceComponent there
    AETHER_CHECK(!h.Call("game_stop_sequences", {{"skip", true}}).is_error);
    h.Call("game_step", {{"frames", 3}});
    AETHER_CHECK(std::fabs(h.Call("game_get_entity", {{"entity", door}}).value["components"]["Transform"]["position"][1].get<double>() - 3.0) < 0.01); // skipped to the final pose
    h.Call("game_unload");
    stdfs::remove_all(dir);

    // --- the editor scene's SequenceComponent
    Harness editor;
    std::string cutscene = editor.Create({{"Transform", Json::object()}});
    AETHER_CHECK(editor.Call("sequence_component_list").value.empty());
    AETHER_CHECK(editor.Call("sequence_component_set", {{"entity", cutscene}, {"fields", {{"rate", 1000}}}}).is_error);
    AETHER_CHECK(editor.Call("sequence_component_set", {{"entity", cutscene}, {"fields", {{"playing", true}}}}).is_error);
    AETHER_CHECK(editor.Call("sequence_component_set", {{"entity", cutscene}, {"fields", {{"time", 3}}}}).is_error);
    AETHER_CHECK(editor.Call("sequence_component_set", {{"entity", cutscene}, {"fields", {{"speed", 1}}}}).is_error);
    AETHER_CHECK(editor.Call("sequence_component_set", {{"entity", cutscene}, {"fields", {{"loop", "yes"}}}}).is_error);
    AETHER_CHECK(!editor.Components(cutscene).contains("SequenceComponent")); // refused edits leave nothing behind
    AETHER_CHECK(!editor.Call("sequence_component_set", {{"entity", cutscene}, {"fields", {{"sequence", "Sequences/Door.asequence"}, {"auto_play", true}}}}).is_error);
    AETHER_CHECK(!editor.Call("sequence_component_set", {{"entity", cutscene}, {"fields", {{"rate", -1}}}}).is_error);
    AETHER_CHECK(editor.Components(cutscene)["SequenceComponent"]["rate"] == -1);
    editor.Call("undo");
    AETHER_CHECK(editor.Components(cutscene)["SequenceComponent"]["rate"] == 1);
    editor.Call("undo");
    AETHER_CHECK(!editor.Components(cutscene).contains("SequenceComponent")); // add + set were one step
    editor.Call("redo");
    AETHER_CHECK(editor.Call("sequence_component_list").value.size() == 1);
    AETHER_CHECK(!editor.Call("sequence_component_remove", {{"entity", cutscene}}).is_error);
    AETHER_CHECK(editor.Call("sequence_component_remove", {{"entity", cutscene}}).is_error);
}

AETHER_TEST(Mcp_TilemapToolsBuildPaintReadAndPreviewALevel) {
    namespace stdfs = std::filesystem;
    Harness h;
    auto project = MakeAssetHost();
    RegisterAssetTools(h.server, project);
    RegisterKitTools(h.server, project);
    RegisterTilemapTools(h.server, project);

    AETHER_CHECK(h.Call("tilemap_info", {{"asset", "Levels/x.atilemap"}}).is_error); // needs a project
    const stdfs::path dir = stdfs::temp_directory_path() / "aether_mcp_tile_test";
    stdfs::remove_all(dir);
    stdfs::create_directories(dir);
    ProjectPaths paths;
    std::string error;
    AETHER_CHECK(CreateProject(dir, "McpTiles", &paths, &error));
    {
        // A 64 x 64 sheet of 4 x 4 tiles of 16 px: tile n is a flat colour (n * 16 + 8, 255 - n * 16, 100).
        std::ofstream out(dir / "tiles.tga", std::ios::binary);
        const unsigned char header[18] = {0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 64, 0, 64, 0, 32, 0x28}; // top-left origin
        out.write(reinterpret_cast<const char*>(header), sizeof(header));
        for (int y = 0; y < 64; ++y) {
            for (int x = 0; x < 64; ++x) {
                const int n = (y / 16) * 4 + (x / 16);
                const unsigned char bgra[4] = {100, static_cast<unsigned char>(255 - n * 16), static_cast<unsigned char>(n * 16 + 8), 255};
                out.write(reinterpret_cast<const char*>(bgra), 4);
            }
        }
    }
    AETHER_CHECK(!h.Call("project_open", {{"project_file", paths.file.string()}}).is_error);
    AETHER_CHECK(!h.Call("asset_add_files", {{"sources", {(dir / "tiles.tga").string()}}, {"folder", "Textures"}}).is_error);
    AETHER_CHECK(!h.Call("content_write", {{"path", "Notes/readme.json"}, {"text", "{}"}}).is_error);

    // --- the tileset
    AETHER_CHECK(h.Call("tileset_create", {{"path", "Tilesets/Ground"}, {"texture", "Textures/tiles.tga"}, {"tile_width", 100}, {"tile_height", 100}}).is_error); // does not fit
    AETHER_CHECK(h.Call("tileset_create", {{"path", "Tilesets/Ground"}, {"texture", "Textures/nothing.tga"}, {"tile_width", 16}, {"tile_height", 16}}).is_error);
    AETHER_CHECK(h.Call("tileset_create", {{"path", "Tilesets/Ground"}, {"texture", "Textures/tiles.tga"}, {"tile_width", 0}, {"tile_height", 16}}).is_error);
    AETHER_CHECK(h.Call("tileset_create", {{"path", "Tilesets/Ground"}, {"texture", "Textures/tiles.tga"}, {"tile_width", 16}, {"tile_height", 16}, {"solid_tiles", {99}}}).is_error);
    Json made = h.Call("tileset_create", {{"path", "Tilesets/Ground"}, {"texture", "Textures/tiles.tga"}, {"tile_width", 16}, {"tile_height", 16}, {"solid_tiles", {1, 2}}}).value;
    AETHER_CHECK(made["path"] == "Tilesets/Ground.atileset");
    AETHER_CHECK(made["columns"] == 4 && made["rows"] == 4 && made["tile_count"] == 16);
    AETHER_CHECK(made["texture_size"]["width"] == 64);
    AETHER_CHECK(h.Call("tileset_create", {{"path", "Tilesets/Ground"}, {"texture", "Textures/tiles.tga"}, {"tile_width", 16}, {"tile_height", 16}}).is_error); // exists
    Json info = h.Call("tileset_info", {{"asset", "Tilesets/Ground.atileset"}}).value;
    AETHER_CHECK(info["solid_tiles"] == Json::array({1, 2}));
    AETHER_CHECK(info["texture"] == "Textures/tiles.tga");
    AETHER_CHECK(h.Call("tileset_info", {{"asset", "Textures/tiles.tga"}}).is_error); // not a tileset

    AETHER_CHECK(!h.Call("tileset_set", {{"asset", "Tilesets/Ground.atileset"}, {"remove_solid", {2}}, {"add_solid", {3}}}).is_error);
    AETHER_CHECK(h.Call("tileset_info", {{"asset", "Tilesets/Ground.atileset"}}).value["solid_tiles"] == Json::array({1, 3}));
    AETHER_CHECK(h.Call("tileset_set", {{"asset", "Tilesets/Ground.atileset"}, {"add_solid", {16}}}).is_error);
    AETHER_CHECK(h.Call("tileset_set", {{"asset", "Tilesets/Ground.atileset"}, {"autotiles", {{{"name", "Ground"}, {"tiles", {1, 2}}}}}}).is_error); // needs 16 tiles
    const Json sixteen = Json::array({9, 9, 10, 11, 9, 9, 12, 8, 9, 9, 10, 11, 9, 9, 12, 8});
    AETHER_CHECK(!h.Call("tileset_set", {{"asset", "Tilesets/Ground.atileset"}, {"autotiles", {{{"name", "Ground"}, {"tiles", sixteen}}}}}).is_error);
    AETHER_CHECK(h.Call("tileset_info", {{"asset", "Tilesets/Ground.atileset"}}).value["autotiles"][0]["tiles"] == sixteen);

    // --- the map, laid out as text
    AETHER_CHECK(h.Call("tilemap_create", {{"path", "Levels/Cave"}, {"tileset", "Tilesets/Ground.atileset"}, {"width", 0}, {"height", 4}}).is_error);
    AETHER_CHECK(h.Call("tilemap_create", {{"path", "Levels/Cave"}, {"tileset", "Textures/tiles.tga"}, {"width", 8}, {"height", 4}}).is_error);
    Json created = h.Call("tilemap_create", {{"path", "Levels/Cave"}, {"tileset", "Tilesets/Ground.atileset"}, {"width", 8}, {"height", 4}, {"layers", {"Ground", "Props"}}}).value;
    AETHER_CHECK(created["path"] == "Levels/Cave.atilemap");
    AETHER_CHECK(h.Call("tilemap_info", {{"asset", "Levels/Cave.atilemap"}}).value["ok"] == true);

    const Json legend = {{"a", 5}, {"#", 1}, {"@", "autotile:0"}};
    Json painted = h.Call("tilemap_paint", {{"asset", "Levels/Cave.atilemap"}, {"layer", "Ground"},
                                            {"ops", {{{"ascii", {"........", "..aa....", "#......#", "@@@@@@@@"}}, {"legend", legend}}}}}).value;
    AETHER_CHECK(painted["cells_changed"] == 12);
    AETHER_CHECK(painted["warnings"].empty());
    Json rows = h.Call("tilemap_ascii", {{"asset", "Levels/Cave.atilemap"}, {"layer", "Ground"}, {"legend", legend}}).value;
    AETHER_CHECK(rows["rows"] == Json::array({"........", "..aa....", "#......#", "@@@@@@@@"}));
    Json automatic = h.Call("tilemap_ascii", {{"asset", "Levels/Cave.atilemap"}, {"solid", true}}).value; // symbols chosen in order of first use
    AETHER_CHECK(automatic["rows"] == Json::array({"........", "..00....", "1......1", "~~~~~~~~"}));
    AETHER_CHECK(automatic["legend"]["0"] == 5 && automatic["legend"]["1"] == 1 && automatic["legend"]["~"] == "autotile:0");
    AETHER_CHECK(automatic["solid_rows"] == Json::array({"........", "........", "#......#", "........"})); // tile 1 blocks, 5 and the autotile do not
    Json usage = h.Call("tilemap_info", {{"asset", "Levels/Cave.atilemap"}}).value;
    AETHER_CHECK(usage["layers"][0]["usage"]["5"] == 2 && usage["layers"][0]["usage"]["1"] == 2 && usage["layers"][0]["usage"]["autotile:0"] == 8);
    AETHER_CHECK(usage["layers"][0]["cells_used"] == 12);
    AETHER_CHECK(usage["layers"][1]["cells_used"] == 0);
    AETHER_CHECK(usage["tileset"] == "Tilesets/Ground.atileset" && usage["tileset_tiles"] == 16);

    // A failed operation saves nothing.
    AETHER_CHECK(h.Call("tilemap_paint", {{"asset", "Levels/Cave.atilemap"}, {"ops", {{{"fill", {0, 0, 7, 3}}, {"cell", 7}}, {{"set", {50, 50}}, {"cell", 1}}}}}).is_error);
    AETHER_CHECK(h.Call("tilemap_ascii", {{"asset", "Levels/Cave.atilemap"}, {"legend", legend}}).value["rows"] == rows["rows"]);
    AETHER_CHECK(h.Call("tilemap_paint", {{"asset", "Levels/Cave.atilemap"}, {"ops", {{{"ascii", {"zz"}}}}}}).is_error); // a symbol not in the legend
    AETHER_CHECK(h.Call("tilemap_paint", {{"asset", "Levels/Cave.atilemap"}, {"ops", {{{"ascii", {"##"}}, {"legend", {{"#", 1}}}, {"at", {7, 0}}}}}}).is_error); // runs off the map
    AETHER_CHECK(h.Call("tilemap_paint", {{"asset", "Levels/Cave.atilemap"}, {"ops", {{{"dance", true}}}}}).is_error);
    AETHER_CHECK(h.Call("tilemap_paint", {{"asset", "Levels/Cave.atilemap"}, {"layer", "Nothing"}, {"ops", {{{"clear", true}}}}}).is_error);
    AETHER_CHECK(h.Call("tilemap_paint", {{"asset", "Levels/Cave.atilemap"}, {"ops", {{{"set", {1, 1}}, {"cell", "lava"}}}}}).is_error);
    AETHER_CHECK(h.Call("tilemap_paint", {{"asset", "Levels/Cave.atilemap"}, {"ops", Json::array()}}).is_error);

    // The other operations: fill, line, replace, clear, layers, resize.
    AETHER_CHECK(!h.Call("tilemap_paint", {{"asset", "Levels/Cave.atilemap"}, {"layer", "Props"}, {"ops", {{{"fill", {1, 1, 2, 2}}, {"cell", 6}}}}}).is_error);
    AETHER_CHECK(h.Call("tilemap_info", {{"asset", "Levels/Cave.atilemap"}}).value["layers"][1]["cells_used"] == 4);
    Json line = h.Call("tilemap_paint", {{"asset", "Levels/Cave.atilemap"}, {"layer", "Props"}, {"ops", {{{"line", {0, 0, 7, 3}}, {"cell", 7}}}}}).value;
    AETHER_CHECK(line["cells_changed"].get<int>() >= 8); // a diagonal across the map
    AETHER_CHECK(!h.Call("tilemap_paint", {{"asset", "Levels/Cave.atilemap"}, {"layer", "Props"}, {"ops", {{{"replace", {{"from", 7}, {"to", -1}}}}}}}).is_error);
    Json after_replace = h.Call("tilemap_info", {{"asset", "Levels/Cave.atilemap"}}).value;
    AETHER_CHECK(!after_replace["layers"][1]["usage"].contains("7"));
    AETHER_CHECK(!h.Call("tilemap_paint", {{"asset", "Levels/Cave.atilemap"}, {"layer", "Props"}, {"ops", {{{"clear", true}}}}}).is_error);
    AETHER_CHECK(h.Call("tilemap_info", {{"asset", "Levels/Cave.atilemap"}}).value["layers"][1]["cells_used"] == 0);
    Json resized = h.Call("tilemap_paint", {{"asset", "Levels/Cave.atilemap"}, {"ops", {{{"resize", {10, 5}}}, {{"add_layer", "Extra"}}, {{"set", {9, 4}}, {"cell", 3}}}}}).value;
    AETHER_CHECK(resized["resized"] == true && resized["width"] == 10 && resized["height"] == 5 && resized["layers_added"] == 1);
    Json grown = h.Call("tilemap_ascii", {{"asset", "Levels/Cave.atilemap"}, {"legend", legend}}).value;
    AETHER_CHECK(grown["rows"][4] == "@@@@@@@@.."); // the cells that fit were kept, and the new top row is empty
    AETHER_CHECK(grown["rows"][0] == ".........0"); // only the tile 3 set at (9, 4); the legend has no symbol for it, so one is assigned
    AETHER_CHECK(h.Call("tilemap_paint", {{"asset", "Levels/Cave.atilemap"}, {"ops", {{{"resize", {0, 5}}}}}}).is_error);

    // A tile the tileset does not have is reported.
    Json bad = h.Call("tilemap_paint", {{"asset", "Levels/Cave.atilemap"}, {"ops", {{{"set", {0, 0}}, {"cell", 99}}, {{"set", {1, 0}}, {"cell", "autotile:3"}}}}}).value;
    AETHER_CHECK(bad["warnings"].size() == 2);
    Json broken = h.Call("tilemap_info", {{"asset", "Levels/Cave.atilemap"}}).value;
    AETHER_CHECK(broken["ok"] == false && broken["problems"].size() == 2);
    const Json fix_ops = Json::array({Json{{"replace", Json{{"from", 99}, {"to", 5}}}}, Json{{"replace", Json{{"from", "autotile:3"}, {"to", -1}}}}});
    AETHER_CHECK(!h.Call("tilemap_paint", {{"asset", "Levels/Cave.atilemap"}, {"ops", fix_ops}}).is_error);
    AETHER_CHECK(h.Call("tilemap_info", {{"asset", "Levels/Cave.atilemap"}}).value["ok"] == true);

    // --- the picture, drawn from the real tile pictures
    AETHER_CHECK(!h.Call("tilemap_paint", {{"asset", "Levels/Cave.atilemap"}, {"ops", {{{"clear", true}}, {{"ascii", {"..aa..", "#....#", "@@@@@@"}}, {"legend", legend}}}}}).is_error);
    AETHER_CHECK(!h.Call("tilemap_paint", {{"asset", "Levels/Cave.atilemap"}, {"ops", {{{"resize", {6, 3}}}}}}).is_error);
    const stdfs::path picture = dir / "level.png";
    Json rpc = h.Rpc("tools/call", {{"name", "tilemap_preview"}, {"arguments", {{"asset", "Levels/Cave.atilemap"}, {"scale", 2}, {"path", picture.string()}, {"show_solid", true}}}});
    AETHER_CHECK(rpc["result"]["isError"] == false);
    AETHER_CHECK(rpc["result"]["content"].size() == 2 && rpc["result"]["content"][1]["mimeType"] == "image/png");
    Json meta = Json::parse(rpc["result"]["content"][0]["text"].get<std::string>());
    AETHER_CHECK(meta["width"] == 6 * 16 * 2 && meta["height"] == 3 * 16 * 2 && meta["textured"] == true);
    assets::ImageData img;
    AETHER_CHECK(assets::DecodeImageFile(picture.string(), img));
    AETHER_CHECK(img.width == 192 && img.height == 96);
    auto pixel = [&](int x, int y) { return &img.pixels[(static_cast<std::size_t>(y) * img.width + static_cast<std::size_t>(x)) * 4]; };
    // Cell (2, 2) is tile 5 (the top row of the 3-row map): its flat colour (88, 175, 100).
    const unsigned char* tile5 = pixel((2 * 16 + 8) * 2, (3 - 1 - 2) * 16 * 2 + 16);
    AETHER_CHECK(std::abs(tile5[0] - 88) <= 2 && std::abs(tile5[1] - 175) <= 2 && std::abs(tile5[2] - 100) <= 2);
    // The autotile's cell on the bottom row resolves to a tile of the rules (9 here), not to the autotile itself.
    const unsigned char* ground = pixel((2 * 16 + 8) * 2, (3 - 1 - 0) * 16 * 2 + 16);
    AETHER_CHECK(std::abs(ground[0] - (9 * 16 + 8)) <= 2 || std::abs(ground[0] - (10 * 16 + 8)) <= 2 || std::abs(ground[0] - (11 * 16 + 8)) <= 2 || std::abs(ground[0] - (12 * 16 + 8)) <= 2 || std::abs(ground[0] - (8 * 16 + 8)) <= 2);
    // Tile 1 (a solid cell, with show_solid) is tinted red: more red than tile 1's own red channel would be.
    const unsigned char* solid = pixel((0 * 16 + 8) * 2, (3 - 1 - 1) * 16 * 2 + 16);
    AETHER_CHECK(solid[0] > 100 && solid[1] < 200);
    // A layer filter draws only that layer: the Ground cell is background.
    Json only_props = Json::parse(h.Rpc("tools/call", {{"name", "tilemap_preview"}, {"arguments", {{"asset", "Levels/Cave.atilemap"}, {"layers", {"Props"}}, {"scale", 1}}}})["result"]["content"][0]["text"].get<std::string>());
    AETHER_CHECK(only_props["layers_drawn"] == Json::array({"Props"}));
    AETHER_CHECK(h.Call("tilemap_preview", {{"asset", "Levels/Cave.atilemap"}, {"scale", 99}}).is_error);

    // --- kit_* tools know both types
    for (const char* type : {"tileset", "tilemap"}) {
        Json schema = h.Call("kit_schema", {{"type", type}}).value;
        AETHER_CHECK(schema.contains("sample") && schema.contains("notes"));
        AETHER_CHECK(h.Call("kit_validate", {{"type", type}, {"definition", schema["sample"]}}).value["ok"] == true);
    }
    AETHER_CHECK(h.Call("kit_list", {{"type", "tilemap"}}).value[0]["valid"] == true);
    AETHER_CHECK(h.Call("kit_list", {{"type", "tileset"}}).value[0]["valid"] == true);
    AETHER_CHECK(h.Call("kit_validate", {{"type", "tilemap"}, {"definition", {{"$type", "Nonsense"}}}}).value["ok"] == false);
    stdfs::remove_all(dir);
}

AETHER_TEST(Mcp_AnimationToolsValidateBlendAndSimulateAGraph) {
    namespace an = aether::anim;
    Harness h;
    auto project = MakeAssetHost();
    RegisterAnimationTools(h.server, project);

    // A blend space: Idle at 0, Walk at 1, Run at 3.
    an::BlendSpace space;
    space.x = {"Speed", 0.0f, 3.0f};
    space.samples = {{"Idle", 0, 0, 1}, {"Walk", 1, 0, 1}, {"Run", 3, 0, 1}};
    const Json space_json = an::BlendSpaceToJson(space);
    AETHER_CHECK(h.Call("anim_validate", {{"type", "blend_space"}, {"definition", space_json}}).value["ok"] == true);
    Json bad_space = space_json;
    bad_space["samples"].push_back(bad_space["samples"][0]);
    Json bad = h.Call("anim_validate", {{"type", "blend_space"}, {"definition", bad_space}}).value;
    AETHER_CHECK(bad["ok"] == false);
    AETHER_CHECK(bad["diagnostics"][0]["code"] == "BS002");

    Json w = h.Call("anim_blend_weights", {{"definition", space_json}, {"points", Json::array({Json{{"x", 0.5}}, Json{{"x", 9}}})}}).value;
    const Json half = w["points"][0]["weights"];
    AETHER_CHECK(half.size() == 2);
    AETHER_CHECK(std::abs(half[0]["weight"].get<f32>() - 0.5f) < 0.01f);
    AETHER_CHECK(w["points"][1]["weights"][0]["clip"] == "Run"); // clamped to the axis end
    AETHER_CHECK(h.Call("anim_blend_weights", {{"definition", space_json}, {"grid", 4}}).value["points"].size() == 4);

    // A graph: Idle <-> Move on a bool, Attack by trigger once Move/Idle, back when finished.
    an::AnimGraph g;
    g.variables = {{"moving", an::VarType::Bool, 0.0f}, {"hit", an::VarType::Trigger, 0.0f}};
    an::AnimNode idle, run, hit, sm;
    idle.id = 1; idle.kind = an::AnimNodeKind::Clip; idle.asset = "Idle";
    run.id = 2; run.kind = an::AnimNodeKind::Clip; run.asset = "Run";
    hit.id = 3; hit.kind = an::AnimNodeKind::Clip; hit.asset = "Hit"; hit.loop = false;
    sm.id = 4; sm.kind = an::AnimNodeKind::StateMachine; sm.machine = "Locomotion";
    g.nodes = {idle, run, hit, sm};
    g.output = 4;
    an::StateMachine m;
    m.name = "Locomotion";
    m.states = {{"Idle", 1}, {"Run", 2}, {"Hit", 3}};
    an::Transition to_run, to_idle, to_hit, from_hit;
    to_run.from = 0; to_run.to = 1; to_run.blend_time = 0.1f; to_run.conditions = {{"moving", an::CompareOp::IsTrue, 0}};
    to_idle.from = 1; to_idle.to = 0; to_idle.blend_time = 0.1f; to_idle.conditions = {{"moving", an::CompareOp::IsFalse, 0}};
    to_hit.from = an::kAnyState; to_hit.to = 2; to_hit.blend_time = 0.0f; to_hit.priority = 5; to_hit.conditions = {{"hit", an::CompareOp::Triggered, 0}};
    from_hit.from = 2; from_hit.to = 0; from_hit.when_finished = true; from_hit.blend_time = 0.0f;
    m.transitions = {to_run, to_idle, to_hit, from_hit};
    g.machines = {m};
    const Json graph_json = an::AnimGraphToJson(g);

    Json ok = h.Call("anim_validate", {{"type", "graph"}, {"definition", graph_json}}).value;
    AETHER_CHECK(ok["ok"] == true);
    Json missing = h.Call("anim_validate", {{"type", "graph"}, {"definition", graph_json}, {"clips", Json::array({"Idle", "Run"})}}).value;
    AETHER_CHECK(missing["ok"] == false);
    AETHER_CHECK(missing["diagnostics"][0]["code"] == "AG010");
    Json broken = graph_json;
    broken["output"] = 99;
    const auto broken_result = h.Call("anim_validate", {{"type", "graph"}, {"definition", broken}});
    AETHER_CHECK(broken_result.is_error || broken_result.value["ok"] == false);

    Json sim = h.Call("anim_graph_simulate",
                      {{"definition", graph_json},
                       {"clips", {{"Hit", 0.5}}},
                       {"steps", Json::array({Json{{"dt", 0.1}},
                                              Json{{"set", {{"moving", true}}}, {"dt", 0.1}, {"repeat", 5}},
                                              Json{{"trigger", Json::array({"hit"})}, {"dt", 0.1}},
                                              Json{{"set", {{"moving", false}}}, {"dt", 0.1}, {"repeat", 8}}})}})
                   .value;
    const Json steps = sim["steps"];
    AETHER_CHECK(steps.size() == 4);
    AETHER_CHECK(steps[0]["states"]["Locomotion"]["state"] == "Idle");
    AETHER_CHECK(steps[1]["states"]["Locomotion"]["state"] == "Run");
    AETHER_CHECK(steps[1]["state_changes"][0]["to"] == "Run");
    AETHER_CHECK(steps[2]["states"]["Locomotion"]["state"] == "Hit");
    AETHER_CHECK(steps[3]["states"]["Locomotion"]["state"] == "Idle"); // the hit clip finished after 0.5s

    // A bad variable and a bad step are reported, not ignored.
    AETHER_CHECK(h.Call("anim_graph_simulate", {{"definition", graph_json}, {"steps", Json::array({Json{{"set", {{"nope", 1}}}}})}}).is_error);
    AETHER_CHECK(h.Call("anim_graph_simulate", {{"definition", graph_json}}).is_error);

    // Montages: validate, then play one in a slot over a graph and see its sections go by.
    an::Montage mt;
    mt.name = "Swing";
    mt.clip = "Swing";
    mt.sections = {{"Windup", 0.0f, "Hit"}, {"Hit", 0.4f, ""}};
    const Json mt_json = an::MontageToJson(mt);
    AETHER_CHECK(h.Call("anim_validate", {{"type", "montage"}, {"definition", mt_json}}).value["ok"] == true);
    Json bad_mt = mt_json;
    bad_mt["sections"][0]["next"] = "Nowhere";
    AETHER_CHECK(h.Call("anim_validate", {{"type", "montage"}, {"definition", bad_mt}}).value["diagnostics"][0]["code"] == "MN002");
    Json mont = h.Call("anim_graph_simulate",
                       {{"definition", graph_json},
                        {"clips", {{"Swing", 1.0}}},
                        {"steps", Json::array({Json{{"play_montage", mt_json}, {"dt", 0.1}}, Json{{"dt", 0.1}, {"repeat", 5}}})}})
                    .value;
    AETHER_CHECK(mont["steps"][0]["events"][0]["type"] == "MontageStarted");
}

AETHER_TEST(Mcp_UiToolsListCheckAndPreviewALayout) {
    namespace ui = aether::ui;
    Harness h;
    auto project = MakeAssetHost();
    RegisterUiTools(h.server, project);

    Json types = h.Call("ui_widget_types").value;
    AETHER_CHECK(types["count"].get<int>() > 5);
    AETHER_CHECK(h.Call("ui_widget_types", {{"type", "Text"}}).value["widgets"][0]["default"]["type"] == "Text");
    AETHER_CHECK(h.Call("ui_widget_types", {{"type", "Nope"}}).is_error);

    auto root = std::make_unique<ui::Canvas>();
    root->name = "Menu";
    auto* panel = root->Add<ui::Image>(ui::Brush::Solid({1.0f, 0.0f, 0.0f, 1.0f}));
    panel->name = "Panel";
    panel->desired_size = {200, 100};
    panel->slot.anchors = ui::Anchors::Point(0.0f, 0.0f);
    panel->slot.auto_size = true;
    auto* title = root->Add<ui::Text>("HELLO");
    title->name = "Title";
    title->slot.anchors = ui::Anchors::Point(0.0f, 0.0f);
    title->slot.position = {300, 20};
    auto* twin = root->Add<ui::Text>("again");
    twin->name = "Title";
    twin->slot.position = {1800, 20}; // runs off a 1920 wide screen with the default 100 wide slot? placed to overflow
    twin->slot.size = {400, 30};
    twin->slot.anchors = ui::Anchors::Point(0.0f, 0.0f);
    const std::vector<ui::Binding> bindings = {{"Title", "text", "game.title"}, {"Ghost", "text", "game.x"}, {"Title", "color", "game.title"}};
    const Json layout = Json::parse(ui::SaveLayout(*root, bindings));

    Json checked = h.Call("ui_check", {{"definition", layout}}).value;
    AETHER_CHECK(checked["ok"] == false);
    AETHER_CHECK(checked["widgets"] == 4);
    std::string all = checked["problems"].dump();
    AETHER_CHECK(all.find("used by 2 widgets") != std::string::npos);
    AETHER_CHECK(all.find("Ghost") != std::string::npos);
    AETHER_CHECK(all.find("can't be bound") != std::string::npos);
    AETHER_CHECK(all.find("past the screen") != std::string::npos);
    AETHER_CHECK(checked["tree"]["children"].size() == 3);
    AETHER_CHECK(checked["tree"]["children"][0]["rect"][2] == 200);
    AETHER_CHECK(checked["bindings"].size() == 3);

    // A clean layout passes, and a bad one says where it is wrong.
    Json clean = layout;
    clean["bindings"] = Json::array();
    clean["root"]["children"].erase(clean["root"]["children"].size() - 1);
    AETHER_CHECK(h.Call("ui_check", {{"definition", clean}}).value["ok"] == true);
    Json broken = layout;
    broken["root"]["children"][0]["type"] = "Imge";
    const auto bad = h.Call("ui_check", {{"definition", broken}});
    AETHER_CHECK(bad.is_error && bad.text.find("Imge") != std::string::npos);

    // The preview: the red panel's pixels are red, and the picture comes back as image content.
    const auto preview = h.Call("ui_preview", {{"definition", clean}, {"width", 960}, {"height", 540}});
    AETHER_CHECK(!preview.is_error);
    AETHER_CHECK(preview.value["width"] == 960 && preview.value["quads"].get<int>() >= 2);
    AETHER_CHECK(h.Call("ui_preview", {{"definition", clean}, {"width", 5000}}).is_error);
}

AETHER_TEST(Mcp_SaveToolsListInspectDeleteAndEditSettings) {
    namespace stdfs = std::filesystem;
    namespace sv = aether::save;
    const stdfs::path dir = stdfs::temp_directory_path() / "aether_mcp_saves";
    stdfs::remove_all(dir);
    stdfs::create_directories(dir);
    Harness h;
    RegisterSaveTools(h.server);

    AETHER_CHECK(h.Call("save_list", {{"directory", (dir / "nope").string()}}).is_error);
    AETHER_CHECK(h.Call("save_list", {{"directory", dir.string()}}).value["count"] == 0);

    {
        sv::SaveSystem saves(dir);
        sv::GameSettings gs;
        gs.language = "fr";
        AETHER_CHECK(saves.Save("slot1", gs).ok);
        gs.language = "de";
        AETHER_CHECK(saves.Save("slot1", gs).ok); // the first save becomes slot1.asav.bak
    }
    Json list = h.Call("save_list", {{"directory", dir.string()}}).value;
    AETHER_CHECK(list["count"] == 1 && list["slots"][0]["slot"] == "slot1" && list["slots"][0]["valid"] == true && list["slots"][0]["has_backup"] == true);

    Json info = h.Call("save_inspect", {{"directory", dir.string()}, {"slot", "slot1"}}).value;
    AETHER_CHECK(info["kind"] == "aether.save" && info["checksum_ok"] == true && info["data"]["language"] == "de");
    AETHER_CHECK(h.Call("save_inspect", {{"directory", dir.string()}, {"slot", "slot1"}, {"backup", true}}).value["data"]["language"] == "fr");
    AETHER_CHECK(h.Call("save_inspect", {{"directory", dir.string()}, {"slot", "bad/slot"}}).is_error);
    AETHER_CHECK(h.Call("save_inspect", {{"directory", dir.string()}, {"slot", "ghost"}}).is_error);
    AETHER_CHECK(h.Call("save_inspect", {{"directory", dir.string()}, {"slot", "slot1"}, {"max_data", 10}}).value["data_truncated"] == true);

    // Tamper with the data: the checksum no longer holds.
    {
        std::ifstream in(dir / "slot1.asav", std::ios::binary);
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        in.close();
        const usize at = text.find("\"de\"");
        AETHER_CHECK(at != std::string::npos);
        text.replace(at, 4, "\"xx\"");
        std::ofstream(dir / "slot1.asav", std::ios::binary) << text;
    }
    AETHER_CHECK(h.Call("save_inspect", {{"directory", dir.string()}, {"slot", "slot1"}}).value["checksum_ok"] == false);

    // Settings: defaults when missing, clamped and written when set, reset back.
    Json settings = h.Call("save_settings", {{"directory", dir.string()}}).value;
    AETHER_CHECK(settings["settings"]["language"] == "en" && !settings.contains("written"));
    Json set = h.Call("save_settings", {{"directory", dir.string()}, {"set", {{"music", 3.5}, {"width", 1920}, {"language", "ja"}}}}).value;
    AETHER_CHECK(set["written"] == true && set["settings"]["music"] == 1.0 && set["settings"]["width"] == 1920 && set["warnings"].size() >= 1);
    AETHER_CHECK(h.Call("save_settings", {{"directory", dir.string()}}).value["settings"]["language"] == "ja");
    AETHER_CHECK(h.Call("save_list", {{"directory", dir.string()}}).value["has_settings"] == true);
    AETHER_CHECK(h.Call("save_settings", {{"directory", dir.string()}, {"set", {{"nonsense", 1}}}}).is_error);
    AETHER_CHECK(h.Call("save_inspect", {{"file", (dir / "settings.asettings").string()}}).value["kind"] == "aether.settings");
    AETHER_CHECK(h.Call("save_settings", {{"directory", dir.string()}, {"reset", true}}).value["settings"]["language"] == "en");

    AETHER_CHECK(!h.Call("save_delete", {{"directory", dir.string()}, {"slot", "slot1"}}).is_error);
    AETHER_CHECK(!stdfs::exists(dir / "slot1.asav") && !stdfs::exists(dir / "slot1.asav.bak"));
    AETHER_CHECK(h.Call("save_delete", {{"directory", dir.string()}, {"slot", "slot1"}}).is_error);
    stdfs::remove_all(dir);
}

AETHER_TEST(Mcp_UiCheckFlagsSmallTouchTargetsAndBadControlKeys) {
    namespace ui = aether::ui;
    Harness h;
    auto project = MakeAssetHost();
    RegisterUiTools(h.server, project);
    auto root = std::make_unique<ui::Canvas>();
    root->name = "Hud";
    auto* tiny = root->Add<ui::Button>();
    tiny->name = "Tiny";
    tiny->hold_key = "Spacebar"; // not a key name
    tiny->padding = ui::Margin::All(0);
    tiny->slot.anchors = ui::Anchors::Point(0, 0);
    tiny->slot.position = {10, 10};
    tiny->slot.size = {30, 30};
    auto* big = root->Add<ui::Button>();
    big->name = "Big";
    big->hold_key = "GamepadA";
    big->slot.anchors = ui::Anchors::Point(0, 0);
    big->slot.position = {100, 10};
    big->slot.size = {120, 80};
    auto* stick = root->Add<ui::VirtualStick>();
    stick->name = "Stick";
    stick->key_x = "GamepadLeftStickX";
    stick->key_y = "LeftStickWhy";
    stick->slot.anchors = ui::Anchors::Point(0, 0);
    stick->slot.position = {300, 10};
    stick->slot.size = {200, 200};
    const Json layout = Json::parse(ui::SaveLayout(*root));

    const std::string plain = h.Call("ui_check", {{"definition", layout}}).value["problems"].dump();
    AETHER_CHECK(plain.find("Spacebar") != std::string::npos && plain.find("LeftStickWhy") != std::string::npos);
    AETHER_CHECK(plain.find("too small") == std::string::npos); // only when asked
    const Json strict = h.Call("ui_check", {{"definition", layout}, {"touch_target", 44}}).value;
    const std::string problems = strict["problems"].dump();
    AETHER_CHECK(strict["ok"] == false && problems.find("too small to tap") != std::string::npos && problems.find("Tiny") != std::string::npos);
    AETHER_CHECK(problems.find("30 x 30") != std::string::npos);
    AETHER_CHECK(problems.find("Canvas/Big") == std::string::npos && problems.find("Canvas/Stick") == std::string::npos || problems.find("too small to tap") == problems.rfind("too small to tap"));
}

AETHER_TEST(Mcp_UiThemeAndAnimationTools) {
    namespace ui = aether::ui;
    Harness h;
    auto project = MakeAssetHost();
    RegisterUiTools(h.server, project);

    ui::Theme theme;
    theme.name = "Dark";
    theme.colors["accent"] = {1.0f, 0.5f, 0.0f, 1.0f};
    theme.styles["Button"] = ui::ControlStyle{};
    theme.styles["Button.Primary"] = ui::ControlStyle{};
    theme.text["Title"] = ui::TextStyle{};
    const Json theme_json = Json::parse(ui::SaveTheme(theme));

    auto root = std::make_unique<ui::Canvas>();
    root->name = "Menu";
    auto* good = root->Add<ui::Button>();
    good->name = "Go";
    good->style_class = "Primary";
    auto* odd = root->Add<ui::Button>();
    odd->name = "Odd";
    odd->style_class = "Fancy";
    auto* title = root->Add<ui::Text>("Hi");
    title->name = "Heading";
    title->style_class = "Title";
    auto* lost = root->Add<ui::Text>("Hi");
    lost->name = "Lost";
    lost->style_class = "Subtitle";
    auto* panel = root->Add<ui::Image>(ui::Brush::Solid({1.0f, 1.0f, 1.0f, 1.0f}));
    panel->name = "Panel";

    ui::UIAnimation pop;
    pop.name = "Pop";
    pop.tracks.push_back({"Panel", "opacity", {{0.0f, 0.0f, ui::Ease::Linear}, {2.0f, 1.0f, ui::Ease::Linear}}});
    pop.tracks.push_back({"Ghost", "opacity", {{0.0f, 0.0f, ui::Ease::Linear}}});
    const Json layout = Json::parse(ui::SaveLayout(*root, {}, {pop}));

    Json t = h.Call("ui_theme_check", {{"definition", theme_json}}).value;
    AETHER_CHECK(t["ok"] == true && t["name"] == "Dark");
    AETHER_CHECK(t["styles"].size() == 2 && t["text_styles"] == Json::array({"Title"}) && t.contains("colors") && t["colors"].contains("accent"));
    Json against = h.Call("ui_theme_check", {{"definition", theme_json}, {"layout", layout}}).value;
    AETHER_CHECK(against["ok"] == false && against["layout_classed_widgets"] == 4);
    const std::string problems = against["problems"].dump();
    AETHER_CHECK(problems.find("Button.Fancy") != std::string::npos && problems.find("Subtitle") != std::string::npos);
    AETHER_CHECK(problems.find("Primary") == std::string::npos && problems.find("Title") == std::string::npos);
    AETHER_CHECK(h.Call("ui_theme_check", {{"definition", {{"colors", {{"x", "@nope"}}}}}}).is_error);

    // A theme restyles the preview.
    const auto themed = h.Call("ui_preview", {{"definition", layout}, {"theme", theme_json}, {"width", 400}, {"height", 300}});
    AETHER_CHECK(!themed.is_error && themed.value["quads"].get<int>() > 0);

    Json anim = h.Call("ui_animation_sample", {{"definition", layout}, {"times", Json::array({0, 1, 2})}}).value;
    AETHER_CHECK(anim["count"] == 1 && anim["ok"] == false);
    const Json samples = anim["animations"][0]["tracks"][0]["samples"];
    AETHER_CHECK(samples.size() == 3 && samples[1]["value"] == 0.5 && samples[2]["value"] == 1.0);
    const std::string anim_problems = anim["problems"].dump();
    AETHER_CHECK(anim_problems.find("Ghost") != std::string::npos);
    Json sparkle = layout;
    sparkle["animations"][0]["tracks"][0]["property"] = "sparkle";
    const auto rejected = h.Call("ui_animation_sample", {{"definition", sparkle}});
    AETHER_CHECK(rejected.is_error && rejected.text.find("sparkle") != std::string::npos); // the loader says so already
    AETHER_CHECK(h.Call("ui_animation_sample", {{"definition", layout}, {"animation", "Nope"}}).is_error);
    AETHER_CHECK(h.Call("ui_animation_sample", {{"definition", layout}}).value["animations"][0]["tracks"][0]["samples"].size() == 5);
}

AETHER_TEST(Mcp_StreamingToolsPartitionInspectAndStreamAWorld) {
    namespace stdfs = std::filesystem;
    const stdfs::path dir = stdfs::temp_directory_path() / "aether_mcp_world";
    stdfs::remove_all(dir);
    Harness h;
    RegisterStreamingTools(h.server, h.session);
    h.Create({{"Transform", {{"position", {10, 0, 10}}}}});
    h.Create({{"Transform", {{"position", {20, 0, 30}}}}});
    h.Create({{"Transform", {{"position", {250, 0, 10}}}}});
    h.Create({{"Transform", {{"position", {-50, 0, 340}}}}});
    h.Create(); // no Transform: stays in the persistent scene

    Json plan = h.Call("world_partition", {{"cell_size", 100}}).value;
    AETHER_CHECK(plan["written"] == false && plan["cell_count"] == 3 && plan["persistent_entities"] == 1 && plan["total_entities"] == 5);
    AETHER_CHECK(plan["largest_cell_entities"] == 2 && plan["cells"][0]["name"] == "-1_3");
    AETHER_CHECK(!stdfs::exists(dir));
    AETHER_CHECK(h.Call("world_partition", {{"cell_size", 0.5}}).is_error);

    AETHER_CHECK(h.Call("world_partition", {{"cell_size", 100}, {"directory", dir.string()}}).value["written"] == true);
    AETHER_CHECK(stdfs::exists(dir / "world.aworld") && stdfs::exists(dir / "cells" / "0_0.aesc"));
    Json info = h.Call("world_partition_info", {{"directory", dir.string()}}).value;
    AETHER_CHECK(info["ok"] == true && info["cell_count"] == 3 && info["cells"][0].contains("bytes"));
    AETHER_CHECK(h.Call("world_partition_info", {{"directory", (dir / "nope").string()}}).is_error);

    // The player walks from the first cluster far away: the first cells load, then unload.
    Json walk = h.Call("world_stream_simulate", {{"directory", dir.string()},
                                                 {"path", Json::array({Json::array({10, 0, 10}), Json::array({10, 0, 1000})})},
                                                 {"radius", 150}})
                    .value;
    AETHER_CHECK(walk["steps"].size() == 2);
    AETHER_CHECK(walk["steps"][0]["loaded_cells"] == Json::array({Json::array({0, 0})}));
    AETHER_CHECK(walk["steps"][0]["loaded"][0]["entities"] == 2);
    AETHER_CHECK(walk["steps"][1]["loaded_cells"].empty() && walk["steps"][1]["unloaded"].size() == 1);
    AETHER_CHECK(walk["steps"][1]["entities"] == 1); // just the streaming source
    AETHER_CHECK(h.Call("world_stream_simulate", {{"directory", dir.string()}, {"path", Json::array()}}).is_error);
    AETHER_CHECK(h.Call("world_stream_simulate", {{"directory", dir.string()}, {"path", Json::array({Json::array({1, 2})})}}).is_error);

    stdfs::remove(dir / "cells" / "0_0.aesc");
    Json broken = h.Call("world_partition_info", {{"directory", dir.string()}}).value;
    AETHER_CHECK(broken["ok"] == false && broken["problems"].dump().find("0_0") != std::string::npos);
    stdfs::remove_all(dir);
}

AETHER_TEST(Mcp_MaterialToolsValidateCompileAndResolve) {
    namespace mt = aether::mat;
    Harness h;
    auto project = MakeAssetHost();
    RegisterMaterialTools(h.server, project);

    mt::Material m;
    m.parameters.push_back({"Albedo", mt::PinType::Texture, {}, "albedo-guid", ""});
    m.parameters.push_back({"Tint", mt::PinType::Float3, {1, 0.8f, 0.6f, 0}, "", ""});
    m.parameters.push_back({"Roughness", mt::PinType::Float, {0.4f, 0, 0, 0}, "", ""});
    const mt::NodeId out = m.Add("Material.Output");
    const mt::NodeId sample = m.Add("Texture.Sample"), mul = m.Add("Math.Multiply");
    m.Connect(m.Add("Param.Texture:Albedo"), "value", sample, "texture").Connect(sample, "rgb", mul, "a");
    m.Connect(m.Add("Param.Vector:Tint"), "value", mul, "b").Connect(mul, "result", out, "BaseColor");
    m.Connect(m.Add("Param.Scalar:Roughness"), "value", out, "Roughness");
    const Json good = mt::MaterialToJson(m);

    // The node library, with the material's own parameter nodes when given one.
    Json all = h.Call("mat_node_types").value;
    AETHER_CHECK(all["count"].get<int>() > 20 && all["categories"].size() > 2);
    Json found = h.Call("mat_node_types", {{"search", "multiply"}}).value;
    AETHER_CHECK(found["count"].get<int>() >= 1 && found["nodes"][0]["id"] == "Math.Multiply" && found["nodes"][0]["inputs"].size() == 2);
    AETHER_CHECK(h.Call("mat_node_types", {{"search", "Tint"}, {"definition", good}}).value["nodes"][0]["id"] == "Param.Vector:Tint");

    Json ok = h.Call("mat_validate", {{"definition", good}}).value;
    AETHER_CHECK(ok["ok"] == true && ok["summary"]["parameters"].size() == 3 && ok["types"].size() > 3);

    // A float into a texture pin is a type error, and a second Material Output is MT001.
    mt::Material bad = m;
    bad.Connect(bad.Add("Const.Float", {{"value", 1}}), "value", sample, "texture");
    Json bad_result = h.Call("mat_validate", {{"definition", mt::MaterialToJson(bad)}}).value;
    AETHER_CHECK(bad_result["ok"] == false && bad_result["errors"].get<int>() >= 1 && bad_result["diagnostics"][0]["code"].get<std::string>().rfind("MT", 0) == 0);

    Json compiled = h.Call("mat_compile", {{"definition", good}, {"hlsl", false}}).value;
    AETHER_CHECK(compiled["ok"] == true && !compiled.contains("hlsl") && compiled["hlsl_bytes"].get<int>() > 100);
    AETHER_CHECK(compiled["textures"].size() == 1 && compiled["textures"][0]["name"] == "Albedo");
    AETHER_CHECK(compiled["parameter_buffer"]["slots"].size() == 2 && compiled["parameter_buffer"]["bytes"].get<int>() % 16 == 0);
    AETHER_CHECK(h.Call("mat_compile", {{"definition", good}}).value["hlsl"].get<std::string>().find("EvaluateMaterial") != std::string::npos);
    Json not_compiled = h.Call("mat_compile", {{"definition", mt::MaterialToJson(bad)}}).value;
    AETHER_CHECK(not_compiled["ok"] == false && not_compiled["diagnostics"].size() >= 1);
    AETHER_CHECK(h.Call("mat_validate", {{"definition", {{"nonsense", 1}}}}).is_error);

    // An instance chain: the parent overrides Tint, the child overrides Roughness and Tint again, and a stale name is reported.
    mt::MaterialInstance parent, child;
    parent.parent = "M_Base.amat";
    parent.SetValue("Tint", Vec4(1, 0, 0, 0));
    child.parent = "MI_Parent.amati";
    child.SetValue("Roughness", Vec4(0.9f, 0, 0, 0));
    child.SetValue("Tint", Vec4(0, 1, 0, 0));
    child.SetValue("Gone", Vec4(1, 0, 0, 0));
    Json resolved = h.Call("mat_instance_resolve",
                           {{"material", good}, {"instances", Json::array({mt::InstanceToJson(parent), mt::InstanceToJson(child)})}})
                        .value;
    Json by_name = Json::object();
    for (const Json& p : resolved["parameters"]) by_name[p["name"].get<std::string>()] = p;
    AETHER_CHECK(by_name["Tint"]["value"][1] == 1.0 && by_name["Tint"]["source"] == 2);
    AETHER_CHECK(by_name["Roughness"]["source"] == 2 && by_name["Albedo"]["source"] == 0 && by_name["Albedo"]["texture"] == "albedo-guid");
    AETHER_CHECK(resolved["diagnostics"].size() == 1 && resolved["diagnostics"][0]["code"] == "MI001");
    AETHER_CHECK(resolved["buffer_bytes"].get<int>() > 0);
    AETHER_CHECK(h.Call("mat_instance_resolve", {{"material", good}, {"instances", Json::array({Json{{"parameters", 5}}})}}).is_error);
}

AETHER_TEST(Mcp_PrefabToolsMakeInstantiateOverrideAndApply) {
    namespace stdfs = std::filesystem;
    Harness h;
    auto project = MakeAssetHost();
    RegisterAssetTools(h.server, project);
    RegisterPrefabTools(h.server, project, h.session);

    AETHER_CHECK(h.Call("prefab_info", {{"prefab", "Prefabs/x.aprefab"}}).is_error); // needs a project
    const stdfs::path dir = stdfs::temp_directory_path() / "aether_mcp_prefab_test";
    stdfs::remove_all(dir);
    stdfs::create_directories(dir);
    ProjectPaths paths;
    std::string error;
    AETHER_CHECK(CreateProject(dir, "McpPrefabs", &paths, &error));
    AETHER_CHECK(!h.Call("project_open", {{"project_file", paths.file.string()}}).is_error);

    // A crate with a lid: two entities.
    const std::string crate = h.Create({{"Transform", {{"position", {0, 0, 0}}}}});
    const std::string lid = h.Create({{"Transform", {{"position", {1, 0, 0}}}}});
    AETHER_CHECK(!h.Call("reparent", {{"entity", lid}, {"parent", crate}}).is_error);
    Json made = h.Call("prefab_make", {{"entity", crate}, {"path", "Prefabs/Crate.aprefab"}}).value;
    AETHER_CHECK(made["entities"] == 2 && made.contains("guid"));
    AETHER_CHECK(h.Call("prefab_make", {{"entity", crate}, {"path", "Prefabs/Crate.aprefab"}}).is_error); // exists
    AETHER_CHECK(!h.Call("prefab_make", {{"entity", crate}, {"path", "Prefabs/Crate.aprefab"}, {"overwrite", true}}).is_error);
    AETHER_CHECK(h.Call("prefab_make", {{"entity", crate}, {"path", "Prefabs/Crate.txt"}}).is_error);

    Json info = h.Call("prefab_info", {{"prefab", "Prefabs/Crate.aprefab"}}).value;
    AETHER_CHECK(info["entities"].size() == 2 && info["entities"][0]["parent"] == 0 && info["entities"][1]["parent"] == info["entities"][0]["id"]);
    AETHER_CHECK(info["flattened"]["ok"] == true && info["flattened"]["entities"] == 2 && info["built_on_it"].empty());
    AETHER_CHECK(h.Call("prefab_info", {{"prefab", "Prefabs/Crate.aprefab"}, {"scenes", true}}).value.contains("scenes"));
    AETHER_CHECK(h.Call("prefab_info", {{"prefab", "Prefabs/Missing.aprefab"}}).is_error);

    // Place an instance, give its lid an override, then revert it.
    const std::size_t before = h.session.world.EntityCount();
    Json placed = h.Call("prefab_instantiate", {{"prefab", "Prefabs/Crate.aprefab"}, {"position", {10, 0, 0}}}).value;
    AETHER_CHECK(placed["report"]["ok"] == true && h.session.world.EntityCount() == before + 2);
    const std::string instance = placed["entity"];
    AETHER_CHECK(h.Call("prefab_overrides", {{"entity", instance}}).value["overrides"].empty());
    AETHER_CHECK(h.Call("prefab_overrides", {{"entity", crate}}).is_error); // not an instance

    EntityGuid instance_guid;
    AETHER_CHECK(ParseEntityGuid(instance, instance_guid));
    aether::Entity root = h.session.guids.Find(h.session.world, instance_guid);
    auto* inst = h.session.world.GetComponent<aether::PrefabInstance>(root);
    AETHER_CHECK(inst != nullptr);
    const aether::PrefabLocalId lid_id = info["entities"][1]["id"].get<aether::PrefabLocalId>();
    aether::SetOverride(*inst, lid_id, "Transform", "position", Json::array({5, 0, 0}));
    Json overrides = h.Call("prefab_overrides", {{"entity", instance}}).value;
    AETHER_CHECK(overrides["overrides"].size() == 1 && overrides["overrides"][0]["field_path"] == "position" && overrides["overrides"][0]["value"][0] == 5);
    Json reverted = h.Call("prefab_revert", {{"entity", instance}, {"component", "Transform"}}).value;
    AETHER_CHECK(reverted["removed"] == 1 && reverted["remaining"] == 0 && reverted["report"]["ok"] == true);

    // An override applied to the prefab changes the asset.
    inst = h.session.world.GetComponent<aether::PrefabInstance>(root);
    aether::SetOverride(*inst, lid_id, "Transform", "position", Json::array({7, 0, 0}));
    Json applied = h.Call("prefab_apply", {{"entity", instance}}).value;
    AETHER_CHECK(applied["applied"] == 1 && applied["remaining"] == 0 && applied["instances_resolved"].get<int>() >= 1 && applied["failed"] == 0);
    const std::string text = h.Call("content_read", {{"path", "Prefabs/Crate.aprefab"}}).value["text"];
    AETHER_CHECK(text.find("7") != std::string::npos);
    AETHER_CHECK(h.Call("prefab_apply", {{"entity", instance}}).value["applied"] == 0);
    stdfs::remove_all(dir);
}

#if AETHER_MCP_TEMPLATES
AETHER_TEST(Mcp_ProjectCreateMakesBlankAndTemplateProjects) {
    namespace stdfs = std::filesystem;
    Harness h;
    auto project = MakeAssetHost();
    RegisterAssetTools(h.server, project);
    const stdfs::path dir = stdfs::temp_directory_path() / "aether_mcp_new_projects";
    stdfs::remove_all(dir);

    Json templates = h.Call("project_templates").value;
    AETHER_CHECK(templates["count"].get<int>() >= 6);
    bool has_platformer = false;
    for (const Json& t : templates["templates"]) has_platformer = has_platformer || t["id"] == "platformer_2d";
    AETHER_CHECK(has_platformer);

    Json blank = h.Call("project_create", {{"parent_dir", dir.string()}, {"name", "Empty"}}).value;
    AETHER_CHECK(blank["name"] == "Empty" && stdfs::exists(dir / "Empty" / "Empty.aproject") && stdfs::is_directory(dir / "Empty" / "Content"));
    AETHER_CHECK(h.Call("project_info").value["name"] == "Empty"); // it is open

    Json third = h.Call("project_create", {{"parent_dir", dir.string()}, {"name", "Runner"}, {"template", "third_person"}}).value;
    AETHER_CHECK(third["template"] == "third_person" && third["startup_scene"] == "Scenes/Main.ascene");
    AETHER_CHECK(stdfs::exists(dir / "Runner" / "Content" / "Scenes" / "Main.ascene"));
    AETHER_CHECK(h.Call("project_info").value["name"] == "Runner"); // and now this one
    AETHER_CHECK(h.Call("asset_info", {{"asset", "Scenes/Main.ascene"}}).value["importer"] == "Scene");

    AETHER_CHECK(h.Call("project_create", {{"parent_dir", dir.string()}, {"name", "Runner"}}).is_error);                         // exists
    AETHER_CHECK(h.Call("project_create", {{"parent_dir", dir.string()}, {"name", "bad/name"}}).is_error);                       // not a folder name
    AETHER_CHECK(h.Call("project_create", {{"parent_dir", dir.string()}, {"name", "Other"}, {"template", "nope"}}).is_error);   // no such template
    AETHER_CHECK(h.Call("project_info").value["name"] == "Runner");                                                              // failures leave it open
    stdfs::remove_all(dir);
}
#endif

#if AETHER_MCP_SCRIPTING
AETHER_TEST(Mcp_ScriptToolsCheckCompleteRunSimulateAndCheckTheProject) {
    namespace stdfs = std::filesystem;
    Harness h;
    auto project = MakeAssetHost();
    RegisterAssetTools(h.server, project);
    RegisterScriptTools(h.server, project);

    const std::string spin =
        "local Spin = {}\n"
        "--@range 0 720\n"
        "--@tooltip degrees per second\n"
        "Spin.speed = 180\n"
        "Spin.label = 'turner'\n"
        "function Spin:OnStart() print('started', self.speed) end\n"
        "function Spin:OnUpdate(dt)\n"
        "  local t = self.entity:Get('Transform')\n"
        "  t.position = vector.create(t.position.x + dt, 0, 0)\n"
        "end\n"
        "return Spin\n";

    // --- check: what a script is, and where it breaks
    Json checked = h.Call("script_check", {{"source", spin}}).value;
    AETHER_CHECK(checked["ok"] == true);
    AETHER_CHECK(checked["class"]["callbacks"] == Json::array({"OnStart", "OnUpdate"}));
    Json vars = checked["class"]["variables"];
    AETHER_CHECK(vars.size() == 2);
    for (const Json& v : vars) {
        if (v["name"] == "speed") {
            AETHER_CHECK(v["kind"] == "number" && v["default"] == 180 && v["range"] == Json::array({0, 720}) && v["tooltip"] == "degrees per second");
        } else {
            AETHER_CHECK(v["name"] == "label" && v["kind"] == "string" && v["default"] == "turner");
        }
    }
    Json broken = h.Call("script_check", {{"source", "local a = 1\nlocal b = = 2\nreturn {}"}}).value;
    AETHER_CHECK(broken["ok"] == false);
    AETHER_CHECK(broken["error"]["line"] == 2);
    AETHER_CHECK(!broken["error"]["message"].get<std::string>().empty());
    AETHER_CHECK(h.Call("script_check").is_error);
    AETHER_CHECK(h.Call("script_check", {{"asset", "Scripts/x.luau"}}).is_error); // no project yet
    AETHER_CHECK(h.Call("script_check", {{"source", 5}}).is_error);

    // --- completion
    auto labels = [&](const Json& items) {
        std::set<std::string> out;
        for (const Json& i : items) out.insert(i["label"].get<std::string>());
        return out;
    };
    Json after_world = h.Call("script_complete", {{"source", "local x = world:"}}).value;
    AETHER_CHECK(labels(after_world["items"]).count("Spawn") == 1);
    AETHER_CHECK(labels(after_world["items"]).count("EntitiesWith") == 1);
    Json callbacks = h.Call("script_complete", {{"source", "local Spin = {}\nfunction Spin:"}}).value;
    AETHER_CHECK(labels(callbacks["items"]).count("OnUpdate") == 1);
    Json components = h.Call("script_complete", {{"source", "local t = e:Get(\""}, {"limit", 500}}).value;
    AETHER_CHECK(labels(components["items"]).count("Transform") == 1);
    Json by_position = h.Call("script_complete", {{"source", "local a = 1\nlocal b = world:"}, {"line", 2}, {"column", 17}}).value;
    AETHER_CHECK(labels(by_position["items"]).count("Spawn") == 1);
    AETHER_CHECK(h.Call("script_complete", {{"source", "x"}, {"cursor", 99}}).is_error);
    AETHER_CHECK(h.Call("script_complete", {{"source", "x\ny"}, {"line", 9}, {"column", 1}}).is_error);
    AETHER_CHECK(h.Call("script_complete", {{"source", "x"}, {"cursor", -1}}).is_error);

    // --- run in the sandbox
    Json ran = h.Call("script_run", {{"source", "print('hi', 2)\nreturn 1, 'a', true"}}).value;
    AETHER_CHECK(ran["ok"] == true);
    AETHER_CHECK(ran["prints"] == Json::array({"hi\t2"}));
    AETHER_CHECK(ran["returned"] == Json::array({1, "a", true}));
    Json failed = h.Call("script_run", {{"source", "local x = 1\nerror('boom')"}}).value;
    AETHER_CHECK(failed["ok"] == false);
    AETHER_CHECK(failed["error"]["message"].get<std::string>().find("boom") != std::string::npos);
    AETHER_CHECK(failed["error"]["line"] == 2);
    Json called = h.Call("script_run", {{"source", "function add(a, b) return a + b end"}, {"call", {{"function", "add"}, {"args", {2, 3}}}}}).value;
    AETHER_CHECK(called["ok"] == true);
    AETHER_CHECK(called["call"]["returned"] == Json::array({5}));
    Json missing = h.Call("script_run", {{"source", "x = 1"}, {"call", {{"function", "nothing"}}}}).value;
    AETHER_CHECK(missing["ok"] == false && missing["call"]["ok"] == false);
    AETHER_CHECK(h.Call("script_run", {{"source", "x = 1"}, {"call", {{"args", {1}}}}}).is_error);
    AETHER_CHECK(h.Call("script_run", {{"source", "x = 1"}, {"call", {{"function", "f"}, {"args", {Json::array()}}}}}).is_error);
    Json runaway = h.Call("script_run", {{"source", "while true do end"}, {"instruction_budget", 5000}}).value;
    AETHER_CHECK(runaway["ok"] == false); // stopped by the budget instead of hanging
    Json sandbox = h.Call("script_run", {{"source", "return os.execute == nil, io == nil, os.clock() >= 0"}}).value;
    AETHER_CHECK(sandbox["returned"] == Json::array({true, true, true}));
    AETHER_CHECK(h.Call("script_run", {{"source", "local e = world:Spawn()\nreturn e ~= nil"}}).value["entities_in_world"] == 1);
    AETHER_CHECK(h.Call("script_run", {{"source", "return 1"}, {"instruction_budget", -1}}).is_error);

    // --- simulate a class on an entity
    Json sim = h.Call("script_simulate", {{"source", spin}, {"ticks", 60}}).value;
    AETHER_CHECK(sim["ok"] == true);
    AETHER_CHECK(sim["prints"] == Json::array({"started\t180"}));
    AETHER_CHECK(std::fabs(sim["transform"]["position"][0].get<double>() - 1.0) < 0.01); // one unit in a second
    AETHER_CHECK(sim["fields"]["speed"] == 180);
    AETHER_CHECK(sim["instances"] == 1);
    Json overridden = h.Call("script_simulate", {{"source", spin}, {"ticks", 1}, {"variables", {{"speed", 90}}}}).value;
    AETHER_CHECK(overridden["prints"] == Json::array({"started\t90"}));
    AETHER_CHECK(overridden["fields"]["speed"] == 90);
    AETHER_CHECK(h.Call("script_simulate", {{"source", spin}, {"variables", {{"nope", 1}}}}).is_error);
    AETHER_CHECK(h.Call("script_simulate", {{"source", spin}, {"variables", {{"speed", "fast"}}}}).is_error);
    Json compile_fail = h.Call("script_simulate", {{"source", "local = 1"}}).value;
    AETHER_CHECK(compile_fail["ok"] == false && compile_fail["stage"] == "compile");
    AETHER_CHECK(h.Call("script_simulate", {{"source", spin}, {"ticks", 99999}}).is_error);
    AETHER_CHECK(h.Call("script_simulate", {{"source", spin}, {"components", {{"NoSuchComponent", Json::object()}}}}).is_error);

    const std::string crashing = "local C = {}\nfunction C:OnUpdate(dt) error('oops') end\nreturn C\n";
    Json crashed = h.Call("script_simulate", {{"source", crashing}, {"ticks", 3}}).value;
    AETHER_CHECK(crashed["ok"] == false);
    AETHER_CHECK(!crashed["errors"].empty());
    AETHER_CHECK(crashed["errors"][0]["message"].get<std::string>().find("oops") != std::string::npos);

    const std::string timers = "local T = {}\nfunction T:OnStart() Timer.After(0.5, function() print('ding') end) end\nreturn T\n";
    Json timed = h.Call("script_simulate", {{"source", timers}, {"ticks", 60}}).value;
    AETHER_CHECK(timed["prints"] == Json::array({"ding"}));
    AETHER_CHECK(timed["timers_pending"] == 0);
    AETHER_CHECK(h.Call("script_simulate", {{"source", timers}, {"ticks", 10}}).value["prints"].empty()); // 10 ticks is 0.17 s: not yet

    const std::string finder = "local F = {}\nfunction F:OnStart() print(#world:EntitiesWith('Tags')) end\nreturn F\n";
    const Json hero = Json{{"Tags", Json{{"names", Json::array({"Hero"})}}}};
    const Json rat = Json{{"components", Json{{"Tags", Json{{"names", Json::array({"Rat"})}}}}}};
    Json found = h.Call("script_simulate", {{"source", finder}, {"ticks", 1}, {"components", hero}, {"entities", Json::array({rat, rat})}}).value;
    AETHER_CHECK(found["prints"] == Json::array({"3"})); // the scripted entity and two more
    AETHER_CHECK(found["entities"] == 3);

    // --- the project: scripts compile and scenes use them correctly
    const stdfs::path dir = stdfs::temp_directory_path() / "aether_mcp_script_test";
    stdfs::remove_all(dir);
    stdfs::create_directories(dir);
    ProjectPaths paths;
    std::string error;
    AETHER_CHECK(CreateProject(dir, "McpScripts", &paths, &error));
    AETHER_CHECK(!h.Call("project_open", {{"project_file", paths.file.string()}}).is_error);
    Json good = h.Call("content_write", {{"path", "Scripts/Spin.luau"}, {"text", spin}}).value;
    AETHER_CHECK(good["asset"]["importer"] == "Script");
    const std::string good_guid = good["asset"]["guid"];
    AETHER_CHECK(!h.Call("content_write", {{"path", "Scripts/Bad.luau"}, {"text", "local x = = 1"}}).is_error);
    AETHER_CHECK(h.Call("script_check", {{"asset", "Scripts/Spin.luau"}}).value["ok"] == true);
    AETHER_CHECK(h.Call("script_check", {{"asset", "Scripts/Bad.luau"}}).value["ok"] == false);
    AETHER_CHECK(h.Call("script_check", {{"asset", "Scripts/Nothing.luau"}}).is_error);
    AETHER_CHECK(h.Call("script_simulate", {{"asset", "Scripts/Spin.luau"}, {"ticks", 30}}).value["ok"] == true);
    AETHER_CHECK(h.Call("script_run", {{"asset", "Scripts/Spin.luau"}}).value["ok"] == true);

    auto scene_with = [&](Json components) {
        return Json{{"$type", "Scene"}, {"$version", 1}, {"entities", Json::array({Json{{"components", Json{{"ScriptComponent", components}}}}})}};
    };
    auto write_scene = [&](const Json& scene) { AETHER_CHECK(!h.Call("content_write", {{"path", "Scenes/start.ascene"}, {"text", scene.dump()}}).is_error); };
    write_scene(scene_with({{"script", good_guid}, {"properties", {{{"name", "speed"}, {"value", "5"}}}}}));
    Json project_check = h.Call("script_check_project").value;
    AETHER_CHECK(project_check["script_components"] == 1);
    AETHER_CHECK(project_check["ok"] == false);
    AETHER_CHECK(project_check["problems"].size() == 1); // only Bad.luau
    AETHER_CHECK(project_check["problems"][0]["path"] == "Scripts/Bad.luau");
    AETHER_CHECK(!h.Call("asset_delete", {{"asset", "Scripts/Bad.luau"}}).is_error);
    AETHER_CHECK(h.Call("script_check_project").value["ok"] == true);
    write_scene(scene_with({{"script", good_guid}, {"properties", {{{"name", "colour"}, {"value", "\"red\""}}}}}));
    Json unknown = h.Call("script_check_project").value;
    AETHER_CHECK(unknown["ok"] == false);
    AETHER_CHECK(unknown["problems"][0]["problem"].get<std::string>().find("does not expose") != std::string::npos);
    write_scene(scene_with({{"script", good_guid}, {"properties", {{{"name", "speed"}, {"value", "\"fast\""}}}}}));
    AETHER_CHECK(h.Call("script_check_project").value["problems"][0]["problem"].get<std::string>().find("does not fit") != std::string::npos);
    write_scene(scene_with({{"script", "00000000-0000-4000-8000-0000000000aa"}}));
    AETHER_CHECK(h.Call("script_check_project").value["problems"][0]["problem"].get<std::string>().find("not a script in the project") != std::string::npos);
    write_scene(scene_with({{"script", ""}}));
    AETHER_CHECK(h.Call("script_check_project").value["problems"][0]["problem"].get<std::string>().find("has no script") != std::string::npos);
    stdfs::remove_all(dir);
}
#endif
AETHER_TEST(Mcp_SceneToolsStayInsideTheSceneFolder) {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "aether_mcp_sandbox";
    fs::remove_all(root);
    fs::create_directories(root / "scenes");
    const fs::path outside = fs::temp_directory_path() / "aether_mcp_outside.json";
    fs::remove(outside);

    Harness h;
    h.session.scene_root = root;
    h.Create({{"Transform", {{"position", {1, 2, 3}}}}});
    AETHER_CHECK(!h.Call("save_scene", {{"path", "scenes/a.json"}}).is_error); // relative: inside
    AETHER_CHECK(fs::exists(root / "scenes/a.json"));
    AETHER_CHECK(!h.Call("save_scene", {{"path", (root / "scenes/b.aesc").string()}}).is_error); // absolute but inside
    AETHER_CHECK(h.Call("save_scene", {{"path", outside.string()}}).is_error);                    // absolute, outside
    AETHER_CHECK(h.Call("save_scene", {{"path", "../aether_mcp_outside.json"}}).is_error);        // dot-dot escape
    AETHER_CHECK(h.Call("save_scene", {{"path", "scenes/../../aether_mcp_outside.json"}}).is_error);
    AETHER_CHECK(!fs::exists(outside));
    AETHER_CHECK(h.Call("save_scene", {{"path", "scenes/notes.txt"}}).is_error); // not a scene file
    AETHER_CHECK(h.Call("load_scene", {{"path", "/etc/passwd.json"}}).is_error);
    AETHER_CHECK(!h.Call("load_scene", {{"path", "scenes/a.json"}}).is_error);
    fs::remove_all(root);
}

namespace {

PermissionPolicy PolicyFrom(const std::string& text, std::string* error = nullptr) {
    PermissionPolicy policy;
    const bool ok = PermissionPolicy::FromBytes(std::span<const unsigned char>(reinterpret_cast<const unsigned char*>(text.data()), text.size()), policy, error);
    return ok ? policy : PermissionPolicy();
}

} // namespace

AETHER_TEST(Mcp_PermissionFileParsesStrictly) {
    std::string error;
    PermissionPolicy none; // built-in rule: destructive asks, the rest runs
    AETHER_CHECK(none.For("destroy_entity", true) == Permission::Confirm && none.For("list_entities", false) == Permission::Allow);

    const PermissionPolicy policy = PolicyFrom(R"({"$type":"AgentPermissions","default":"allow","tools":{"destroy_entity":"deny","save_scene":"confirm"}})", &error);
    AETHER_CHECK(policy.RuleCount() == 2 && policy.For("destroy_entity", true) == Permission::Deny && policy.For("save_scene", false) == Permission::Confirm);
    AETHER_CHECK(policy.For("remove_component", true) == Permission::Allow); // the file's default beats the built-in rule

    for (const char* bad : {R"({"$type":"AgentPermissions","default":"maybe"})", R"({"$type":"AgentPermissions","tools":[]})", R"({"$type":"AgentPermissions","tools":{"x":5}})",
                            R"({"$type":5})", R"({"tools":{}})", "[]", "not json", ""}) {
        PermissionPolicy out;
        const std::string text = bad;
        AETHER_CHECK(!PermissionPolicy::FromBytes(std::span<const unsigned char>(reinterpret_cast<const unsigned char*>(text.data()), text.size()), out, &error) && !error.empty());
    }
    const std::string huge(kMaxPermissionFileBytes + 1, ' ');
    PermissionPolicy out;
    AETHER_CHECK(!PermissionPolicy::FromBytes(std::span<const unsigned char>(reinterpret_cast<const unsigned char*>(huge.data()), huge.size()), out, &error));
}

AETHER_TEST(Mcp_DeniedToolsDoNotRunAndDestructiveOnesAskFirst) {
    Harness h;
    const std::string guid = h.Create();
    AETHER_CHECK(!guid.empty());
    h.server.SetPolicy(PolicyFrom(R"({"$type":"AgentPermissions","tools":{"undo":"deny"}})"));

    auto undo = h.Call("undo");
    AETHER_CHECK(undo.is_error && undo.text.find("denied") != std::string::npos);

    // destroy_entity is destructive: the built-in rule asks first and nothing happens yet.
    auto ask = h.Call("destroy_entity", {{"entity", guid}});
    AETHER_CHECK(!ask.is_error && ask.value["status"] == "confirmation_required" && ask.value["token"].is_string());
    AETHER_CHECK(!h.Call("get_entity", {{"entity", guid}}).is_error); // still there
    const std::string token = ask.value["token"];

    // A token is for one call: a different entity, a wrong token and a repeat are all refused.
    AETHER_CHECK(h.Call("destroy_entity", {{"entity", "someone-else"}, {"confirm_token", token}}).is_error);
    AETHER_CHECK(h.Call("destroy_entity", {{"entity", guid}, {"confirm_token", "0000000000000000"}}).is_error);
    AETHER_CHECK(!h.Call("get_entity", {{"entity", guid}}).is_error);

    auto done = h.Call("destroy_entity", {{"entity", guid}, {"confirm_token", token}});
    AETHER_CHECK(!done.is_error);
    AETHER_CHECK(h.Call("get_entity", {{"entity", guid}}).is_error);
    AETHER_CHECK(h.Call("destroy_entity", {{"entity", guid}, {"confirm_token", token}}).is_error); // spent

    // Reading and ordinary edits are not asked about.
    AETHER_CHECK(!h.Call("list_entities").is_error && !h.Call("editor_state").is_error);
}

AETHER_TEST(Mcp_ConfirmationsExpireAndAreBounded) {
    Harness h;
    auto now = std::chrono::steady_clock::now();
    h.server.SetClock([&now] { return now; });
    h.server.SetPolicy(PermissionPolicy());
    const std::string guid = h.Create();

    const std::string token = h.Call("destroy_entity", {{"entity", guid}}).value["token"];
    now += McpServer::kConfirmationLifetime + std::chrono::seconds(1);
    AETHER_CHECK(h.Call("destroy_entity", {{"entity", guid}, {"confirm_token", token}}).is_error); // expired
    AETHER_CHECK(!h.Call("get_entity", {{"entity", guid}}).is_error);

    for (std::size_t i = 0; i < McpServer::kMaxPendingConfirmations; ++i) {
        AETHER_CHECK(h.Call("destroy_entity", {{"entity", guid + std::to_string(i)}}).value["status"] == "confirmation_required");
    }
    auto full = h.Call("destroy_entity", {{"entity", guid}});
    AETHER_CHECK(full.is_error && full.text.find("Too many") != std::string::npos);
    now += McpServer::kConfirmationLifetime + std::chrono::seconds(1);
    AETHER_CHECK(h.Call("destroy_entity", {{"entity", guid}}).value["status"] == "confirmation_required"); // space again
}

AETHER_TEST(Mcp_SessionLogRecordsEachCallWithoutItsArguments) {
    Harness h;
    std::vector<Json> records;
    h.server.SetAuditSink([&records](const Json& record) { records.push_back(record); });
    h.server.SetPolicy(PolicyFrom(R"({"$type":"AgentPermissions","tools":{"undo":"deny"}})"));
    const std::string guid = h.Create({{"Transform", {{"position", {4, 5, 6}}}}});

    (void)h.Call("list_entities");
    (void)h.Call("undo");
    const std::string token = h.Call("destroy_entity", {{"entity", guid}}).value["token"];
    (void)h.Call("destroy_entity", {{"entity", guid}, {"confirm_token", "bad"}});
    (void)h.Call("destroy_entity", {{"entity", guid}, {"confirm_token", token}});
    (void)h.Call("get_entity", {{"entity", "missing"}});

    std::vector<std::string> outcomes;
    for (const Json& r : records) outcomes.push_back(r["tool"].get<std::string>() + ":" + r["outcome"].get<std::string>());
    const std::vector<std::string> expected = {"create_entity:ok", "list_entities:ok", "undo:denied", "destroy_entity:confirmation_required",
                                               "destroy_entity:confirmation_invalid", "destroy_entity:ok", "get_entity:error"};
    AETHER_CHECK(outcomes == expected);
    for (const Json& r : records) {
        AETHER_CHECK(r["args_hash"].is_string() && r["args_hash"].get<std::string>().size() == 16 && r["time_ms"].is_number());
        AETHER_CHECK(r.dump().find("4") == std::string::npos || r.dump().find("position") == std::string::npos); // no argument content
    }
    // The same arguments hash the same way; the confirm token is not part of them.
    AETHER_CHECK(records[3]["args_hash"] == records[5]["args_hash"]);
}

// game_ui shows the running game's UI and game_ui_click presses a widget the way a player would.
AETHER_TEST(Mcp_GameUiShowsWidgetsAndClicksThem) {
    if (!aether::player::Game::HasScripting()) return;
    namespace stdfs = std::filesystem;
    const stdfs::path dir = stdfs::temp_directory_path() / "aether_mcp_game_ui_test";
    stdfs::remove_all(dir);
    stdfs::create_directories(dir);
    auto root = std::make_unique<ui::Canvas>();
    root->name = "Hud";
    auto* play = root->Add<ui::Button>();
    play->name = "Play";
    play->slot.anchors = ui::Anchors::Point(0, 0);
    play->slot.position = {100, 100};
    play->slot.size = {200, 80};
    play->Add<ui::Text>("Play");
    auto* status = root->Add<ui::Text>("idle");
    status->name = "Status";
    auto* bar = root->Add<ui::ProgressBar>();
    bar->name = "Health";
    bar->percent = 0.5f;
    const std::string layout = ui::SaveLayout(*root);
    const std::string script_guid = assets::ToString(assets::NewAssetGuid());
    const std::string script =
        "local Hud = {}\n"
        "function Hud:OnUpdate(dt)\n"
        "  if UI.WasClicked('Play') then UI.SetText('Status', 'clicked') end\n"
        "end\n"
        "return Hud\n";
    const Json scene = {{"$type", "Scene"}, {"$version", 1},
        {"entities", Json::array({{{"components", {{"WidgetComponent", {{"layout", "UI/Hud.aui"}, {"z", 0}, {"visible", true}}}}}},
                                  {{"components", {{"ScriptComponent", {{"script", script_guid}}}}}}})}};
    const Json manifest = {{"$type", "CookManifest"}, {"$version", 1}, {"project", "McpUi"}, {"configuration", "Development"},
                           {"startup_scene", "Scenes/start.ascene"}, {"fixed_timestep_hz", 60.0}, {"gravity", {0.0, -9.81, 0.0}},
                           {"layers", {"Default"}}, {"collision_matrix", Json::array()},
                           {"assets", Json::array({{{"guid", assets::ToString(assets::NewAssetGuid())}, {"path", "Scenes/start.ascene"}, {"importer", "Scene"}},
                                                   {{"guid", script_guid}, {"path", "Scripts/Hud.luau"}, {"importer", "Script"}},
                                                   {{"guid", assets::ToString(assets::NewAssetGuid())}, {"path", "UI/Hud.aui"}, {"importer", "Test"}}})},
                           {"files", Json::array()}};
    pak::PakWriter writer;
    writer.Add("Manifest.json", manifest.dump());
    writer.Add("Content/Scenes/start.ascene", scene.dump());
    writer.Add("Content/Scripts/Hud.luau", script);
    writer.Add("Content/UI/Hud.aui", layout);
    const stdfs::path file = dir / "game.apak";
    AETHER_CHECK(writer.Write(file.string()));

    Harness h;
    RegisterGameTools(h.server);
    AETHER_CHECK(h.Call("game_ui").is_error); // no game yet
    AETHER_CHECK(!h.Call("game_load", {{"paks", {file.string()}}, {"user_dir", (dir / "user").string()}}).is_error);
    AETHER_CHECK(!h.Call("game_step", {{"frames", 6}}).is_error); // the layout appears on the first frames

    Json all = h.Call("game_ui").value;
    AETHER_CHECK(all["layers"].size() == 1 && all["layers"][0]["name"] == "Hud");
    Json status_before = h.Call("game_ui", {{"widget", "Status"}}).value["widget"];
    AETHER_CHECK(status_before["type"] == "Text" && status_before["text"] == "idle");
    AETHER_CHECK(h.Call("game_ui", {{"widget", "Health"}}).value["widget"]["percent"] == 0.5);
    Json button = h.Call("game_ui", {{"widget", "Play"}}).value["widget"];
    AETHER_CHECK(button["interactive"] == true && button["enabled"] == true && button["rect"][2] == 200);
    AETHER_CHECK(h.Call("game_ui", {{"widget", "Nothing"}}).is_error);

    Harness::Result clicked = h.Call("game_ui_click", {{"widget", "Play"}});
    AETHER_CHECK(!clicked.is_error && clicked.value["widget"] == "Play");
    AETHER_CHECK(h.Call("game_ui", {{"widget", "Status"}}).value["widget"]["text"] == "clicked");
    AETHER_CHECK(h.Call("game_ui_click", {{"widget", "Nothing"}}).is_error);
    AETHER_CHECK(h.Call("game_ui_click", {}).is_error);
    h.Call("game_unload");
    stdfs::remove_all(dir);
}

// game_spawn_entity, game_add_component and game_remove_component change the running world.
AETHER_TEST(Mcp_GameSpawnsEntitiesAndEditsTheirComponents) {
    namespace stdfs = std::filesystem;
    const stdfs::path dir = stdfs::temp_directory_path() / "aether_mcp_game_spawn_test";
    stdfs::remove_all(dir);
    stdfs::create_directories(dir);
    const Json manifest = {{"$type", "CookManifest"}, {"$version", 1}, {"project", "McpSpawn"}, {"configuration", "Development"},
                           {"startup_scene", "Scenes/start.ascene"}, {"fixed_timestep_hz", 60.0}, {"gravity", {0.0, -9.81, 0.0}},
                           {"layers", {"Default"}}, {"collision_matrix", Json::array()},
                           {"assets", Json::array({{{"guid", assets::ToString(assets::NewAssetGuid())}, {"path", "Scenes/start.ascene"}, {"importer", "Scene"}}})},
                           {"files", Json::array()}};
    const Json scene = {{"$type", "Scene"}, {"$version", 1}, {"entities", Json::array()}};
    pak::PakWriter writer;
    writer.Add("Manifest.json", manifest.dump());
    writer.Add("Content/Scenes/start.ascene", scene.dump());
    const stdfs::path file = dir / "game.apak";
    AETHER_CHECK(writer.Write(file.string()));

    Harness h;
    RegisterGameTools(h.server);
    AETHER_CHECK(h.Call("game_spawn_entity", {{"components", Json::object()}}).is_error); // no game yet
    AETHER_CHECK(!h.Call("game_load", {{"paks", {file.string()}}, {"user_dir", (dir / "user").string()}}).is_error);
    AETHER_CHECK(h.Call("game_state").value["entities"] == 0);

    Harness::Result spawned = h.Call("game_spawn_entity", {{"components", {{"Transform", {{"position", {4, 5, 6}}}}}}, {"tags", {"Crate", "Loot"}}});
    AETHER_CHECK(!spawned.is_error);
    const std::string key = spawned.value["entity"];
    AETHER_CHECK(spawned.value["components"]["Transform"]["position"] == Json::array({4, 5, 6}));
    AETHER_CHECK(spawned.value["components"]["Tags"]["names"] == Json::array({"Crate", "Loot"}));
    AETHER_CHECK(h.Call("game_state").value["entities"] == 1);
    AETHER_CHECK(h.Call("game_list_entities", {{"tag", "Loot"}}).value["total"] == 1);
    AETHER_CHECK(!h.Call("game_step", {{"frames", 3}}).is_error);

    AETHER_CHECK(h.Call("game_spawn_entity", {{"components", {{"NoSuchComponent", Json::object()}}}}).is_error);
    AETHER_CHECK(h.Call("game_spawn_entity", {{"components", {{"Transform", 5}}}}).is_error);
    AETHER_CHECK(h.Call("game_state").value["entities"] == 1); // the failed spawn left nothing behind
    AETHER_CHECK(h.Call("game_spawn_entity", {{"components", "Transform"}}).is_error);

    Harness::Result added = h.Call("game_add_component", {{"entity", key}, {"component", "PointLight"}, {"values", {{"range", 25.0}}}});
    AETHER_CHECK(!added.is_error);
    AETHER_CHECK(added.value["added"] == true && added.value["components"]["PointLight"]["range"] == 25.0);
    Harness::Result merged = h.Call("game_add_component", {{"entity", key}, {"component", "PointLight"}, {"values", {{"intensity", 3.0}}}});
    AETHER_CHECK(merged.value["added"] == false && merged.value["components"]["PointLight"]["range"] == 25.0 && merged.value["components"]["PointLight"]["intensity"] == 3.0);
    AETHER_CHECK(!h.Call("game_remove_component", {{"entity", key}, {"component", "PointLight"}}).is_error);
    AETHER_CHECK(h.Call("game_remove_component", {{"entity", key}, {"component", "PointLight"}}).is_error); // already gone
    AETHER_CHECK(h.Call("game_remove_component", {{"entity", key}, {"component", "Transform"}}).is_error);
    AETHER_CHECK(!h.Call("game_destroy_entity", {{"entity", key}}).is_error);
    h.Call("game_unload");
    stdfs::remove_all(dir);
}
