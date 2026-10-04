#include "aether/templates/templates.h"

#include "template_scripts.h"

#include "aether/assets/asset_database.h"
#include "aether/blueprint/graph.h"
#include "aether/blueprint/nodes.h"
#include "aether/blueprint/system.h"
#include "aether/core/log.h"
#include "aether/input/bindings.h"
#include "aether/platform/filesystem.h"
#include "aether/reflection/serialize.h"
#include "aether/scene/components.h"
#include "aether/scene/entity_guid.h"
#include "aether/scene/gameplay.h"
#include "aether/scene/script_component.h"

#include <nlohmann/json.hpp>

#include <cmath>

namespace aether::templates {

using nlohmann::json;
namespace stdfs = std::filesystem;

namespace {

constexpr f32 kPi = 3.14159265358979f;

enum class Kind { Blank, FirstPerson, ThirdPerson, TopDown, Vehicle };

struct Entry {
    ProjectTemplate info;
    Kind kind = Kind::Blank;
};

ProjectTemplate Info(std::string id, std::string name, std::string genre, std::string description,
                     std::vector<std::string> features) {
    ProjectTemplate t;
    t.id = std::move(id);
    t.name = std::move(name);
    t.genre = std::move(genre);
    t.description = std::move(description);
    t.features = std::move(features);
    return t;
}

const std::vector<Entry>& Entries() {
    static const std::vector<Entry> entries = [] {
        std::vector<Entry> e;
        e.push_back({Info("blank", "Blank", "Any",
                      "An empty level with a camera and a spot to start from. Nothing to delete first.",
                      {"A Main scene with a camera and a Player Start", "No scripts, Blueprints or input bindings"}),
                     Kind::Blank});
        e.push_back({Info("first_person", "First Person", "Shooter",
                      "Walk and look around from the player's eyes, with pickups to find.",
                      {"WASD or the left stick to move, the mouse or the right stick to look, Space or A to jump",
                       "A Luau controller with speed, look and jump settings in the Inspector",
                       "BP_Pickup: a Blueprint that spins, placed in a ring around the start"}),
                     Kind::FirstPerson});
        e.push_back({Info("third_person", "Third Person", "Adventure",
                      "A character with a camera that orbits behind it, turning to face where it runs.",
                      {"Movement relative to the camera, the Look axis orbits it", "Jump, camera distance and height settings",
                       "BP_Pickup pickups to run into"}),
                     Kind::ThirdPerson});
        e.push_back({Info("top_down", "Top Down", "Action",
                      "A character moved with WASD, seen from above at an angle, with a camera that follows.",
                      {"World-axis movement, facing the way it goes", "A tilted follow camera with distance and tilt settings",
                       "BP_Pickup pickups scattered around"}),
                     Kind::TopDown});
        e.push_back({Info("vehicle", "Vehicle", "Racing",
                      "An arcade car: throttle, brake and steer, with a chase camera.",
                      {"Throttle and brake on W and S, steering on A and D, steering that needs speed",
                       "Acceleration, braking, drag and turn rate settings", "BP_Pickup markers to drive through"}),
                     Kind::Vehicle});
        ProjectTemplate platformer = Info("platformer_2d", "2D Platformer", "Platformer",
                                          "Run and jump through a side-scrolling level.",
                                          {"Sprites, tilemaps and 2D physics"});
        platformer.available = false;
        platformer.unavailable_reason = "It needs the 2D toolkit (sprites, tilemaps and 2D physics), which isn't built yet";
        e.push_back({platformer, Kind::Blank});
        return e;
    }();
    return entries;
}

// --- Files -----------------------------------------------------------------

bool Fail(std::string* error, std::string message) {
    if (error) *error = std::move(message);
    return false;
}

bool WriteText(const stdfs::path& file, const char* text, std::string* error) {
    std::error_code ec;
    stdfs::create_directories(file.parent_path(), ec);
    const std::string s(text);
    if (!fs::WriteFileBytes(file.string(), s.data(), s.size())) return Fail(error, "Couldn't write " + file.string());
    return true;
}

// --- Input -------------------------------------------------------------------

input::InputModifier Modifier(input::ModifierType type) {
    input::InputModifier m;
    m.type = type;
    return m;
}

input::InputBinding Bind(const char* action, input::Key key, std::vector<input::InputModifier> modifiers = {},
                         std::vector<input::InputTrigger> triggers = {}) {
    input::InputBinding b;
    b.action = action;
    b.key = key;
    b.modifiers = std::move(modifiers);
    b.triggers = std::move(triggers);
    return b;
}

// Move: WASD and the left stick. Look: the mouse and the right stick. Jump:
// Space and the A button. A key is on the X axis; W and S are swizzled onto Y.
input::InputMappingContext GameplayContext(bool look, bool jump) {
    using namespace input;
    const InputModifier swizzle = Modifier(ModifierType::Swizzle);
    const InputModifier negate = Modifier(ModifierType::Negate);
    InputModifier dead = Modifier(ModifierType::DeadZone);
    dead.lower = 0.2f;
    InputMappingContext c;
    c.name = "Gameplay";
    c.bindings = {Bind("Move", Key::W, {swizzle}),        Bind("Move", Key::S, {swizzle, negate}),
                  Bind("Move", Key::D),                   Bind("Move", Key::A, {negate}),
                  Bind("Move", Key::GamepadLeftStickX, {dead}),
                  // A stick's Y is negative pushed forward (the window layer reports GLFW's axes), so
                  // it's negated onto Move's forward; for Look, down is positive on both stick and mouse.
                  Bind("Move", Key::GamepadLeftStickY, {dead, swizzle, negate})};
    if (look) {
        c.bindings.push_back(Bind("Look", Key::MouseX));
        c.bindings.push_back(Bind("Look", Key::MouseY, {swizzle}));
        c.bindings.push_back(Bind("Look", Key::GamepadRightStickX, {dead}));
        c.bindings.push_back(Bind("Look", Key::GamepadRightStickY, {dead, swizzle}));
    }
    if (jump) {
        InputTrigger pressed;
        pressed.type = TriggerType::Pressed;
        c.bindings.push_back(Bind("Jump", Key::Space, {}, {pressed}));
        c.bindings.push_back(Bind("Jump", Key::GamepadA, {}, {pressed}));
    }
    return c;
}

bool WriteInput(const stdfs::path& content, bool look, bool jump, std::string* error) {
    using namespace input;
    const stdfs::path dir = content / "Input";
    std::error_code ec;
    stdfs::create_directories(dir, ec);
    if (!SaveInputAction({"Move", ActionValueType::Axis2D}, dir / "Move.aaction", error)) return false;
    if (look && !SaveInputAction({"Look", ActionValueType::Axis2D}, dir / "Look.aaction", error)) return false;
    if (jump && !SaveInputAction({"Jump", ActionValueType::Bool}, dir / "Jump.aaction", error)) return false;
    return SaveMappingContext(GameplayContext(look, jump), dir / "Gameplay.amapping", error);
}

// --- The pickup Blueprint ------------------------------------------------------

// BP_Pickup: spins about the up axis. Tick: Angle += delta * SpinSpeed, then
// the entity's rotation is Angle degrees about +Y.
bp::Blueprint PickupBlueprint() {
    using namespace bp;
    Blueprint blueprint;
    Variable angle;
    angle.name = "Angle";
    angle.type = PinType::Of(ValueType::Float);
    angle.default_value = 0.0f;
    angle.tooltip = "The current turn, degrees";
    Variable speed;
    speed.name = "SpinSpeed";
    speed.type = PinType::Of(ValueType::Float);
    speed.default_value = 90.0f;
    speed.flags = Var_InstanceEditable;
    speed.tooltip = "Degrees per second";
    blueprint.variables = {angle, speed};
    Graph graph;
    graph.name = "EventGraph";
    GraphBuilder b(graph);
    const NodeId tick = b.Add("Event.Tick", json::object(), 0, 0);
    const NodeId get_speed = b.Add("Var.Get:SpinSpeed", json::object(), 0, 160);
    const NodeId turn = b.Add("Math.Multiply:float", json::object(), 260, 120);
    const NodeId get_angle = b.Add("Var.Get:Angle", json::object(), 260, 260);
    const NodeId sum = b.Add("Math.Add:float", json::object(), 500, 160);
    const NodeId set_angle = b.Add("Var.Set:Angle", json::object(), 740, 0);
    const NodeId rotation = b.Add("Quat.FromAxisAngle", json::object(), 1000, 200);
    const NodeId apply = b.Add("Entity.SetRotation", json::object(), 1260, 0);
    b.Default(rotation, "axis", json::array({0, 1, 0}));
    b.Connect(tick, "delta_seconds", turn, "a")
        .Connect(get_speed, "value", turn, "b")
        .Connect(get_angle, "value", sum, "a")
        .Connect(turn, "result", sum, "b")
        .Connect(tick, "then", set_angle, "exec")
        .Connect(sum, "result", set_angle, "value")
        .Connect(set_angle, "value", rotation, "degrees")
        .Connect(set_angle, "then", apply, "exec")
        .Connect(rotation, "result", apply, "rotation");
    blueprint.graphs.push_back(graph);
    return blueprint;
}

// --- The scene ------------------------------------------------------------------

struct SceneBuilder {
    json entities = json::array();

    // One entity: a transform, tags, and optionally a camera, a script and a Blueprint.
    void Add(Vec3 position, Quaternion rotation, const std::vector<std::string>& tags, bool camera = false,
             const assets::AssetGuid& script = {}, const assets::AssetGuid& blueprint = {}) {
        json components = {{"Transform", reflect::ToJson(Transform{position, rotation})}};
        if (!tags.empty()) {
            Tags t;
            t.names = tags;
            components["Tags"] = reflect::ToJson(t);
        }
        if (camera) components["Camera"] = reflect::ToJson(Camera{});
        if (!script.IsNull()) {
            ScriptComponent s;
            s.script.guid = script;
            components["ScriptComponent"] = reflect::ToJson(s);
        }
        if (!blueprint.IsNull()) {
            bp::BlueprintInstance instance;
            instance.blueprint.guid = blueprint;
            components["BlueprintInstance"] = reflect::ToJson(instance);
        }
        entities.push_back({{"guid", ToString(NewEntityGuid())}, {"components", std::move(components)}});
    }

    // `count` pickups on a circle of `radius` about (cx, y, cz).
    void Ring(const assets::AssetGuid& blueprint, int count, f32 radius, f32 cx, f32 y, f32 cz) {
        for (int i = 0; i < count; ++i) {
            const f32 a = 2.0f * kPi * static_cast<f32>(i) / static_cast<f32>(count);
            Add(Vec3(cx + radius * std::sin(a), y, cz - radius * std::cos(a)), Quaternion::Identity(), {"Pickup"}, false, {},
                blueprint);
        }
    }

    bool Write(const stdfs::path& file, std::string* error) const {
        const json scene = {{"$type", "Scene"}, {"$version", 1}, {"entities", entities}};
        const std::string text = scene.dump(2) + "\n";
        std::error_code ec;
        stdfs::create_directories(file.parent_path(), ec);
        if (!fs::WriteFileBytes(file.string(), text.data(), text.size())) return Fail(error, "Couldn't write " + file.string());
        return true;
    }
};

// Pitch about X, in the engine's right-handed, -Z-forward convention.
Quaternion PitchDown(f32 radians) {
    return Quaternion::FromAxisAngle(Vec3(1, 0, 0), -radians);
}

} // namespace

const std::vector<ProjectTemplate>& ProjectTemplates() {
    static const std::vector<ProjectTemplate> infos = [] {
        std::vector<ProjectTemplate> v;
        for (const Entry& e : Entries()) v.push_back(e.info);
        return v;
    }();
    return infos;
}

const ProjectTemplate* FindProjectTemplate(const std::string& id) {
    for (const ProjectTemplate& t : ProjectTemplates()) {
        if (t.id == id) return &t;
    }
    return nullptr;
}

bool CreateProjectFromTemplate(const stdfs::path& parent_dir, const std::string& name, const std::string& template_id,
                               ProjectPaths* out_paths, std::string* error) {
    const Entry* entry = nullptr;
    for (const Entry& e : Entries()) {
        if (e.info.id == template_id) entry = &e;
    }
    if (!entry) return Fail(error, "There's no project template '" + template_id + "'");
    if (!entry->info.available) {
        return Fail(error, "The " + entry->info.name + " template isn't available: " + entry->info.unavailable_reason);
    }
    ProjectPaths paths;
    if (!CreateProject(parent_dir, name, &paths, error)) return false;
    const stdfs::path content = paths.content;
    const Kind kind = entry->kind;

    // Scripts, the pickup Blueprint and the input bindings.
    const char* script_source = nullptr;
    const char* script_name = nullptr;
    switch (kind) {
        case Kind::FirstPerson: script_source = kFirstPersonScript, script_name = "FirstPersonController"; break;
        case Kind::ThirdPerson: script_source = kThirdPersonScript, script_name = "ThirdPersonController"; break;
        case Kind::TopDown: script_source = kTopDownScript, script_name = "TopDownController"; break;
        case Kind::Vehicle: script_source = kVehicleScript, script_name = "VehicleController"; break;
        case Kind::Blank: break;
    }
    const std::string script_path = script_name ? std::string("Scripts/") + script_name + ".luau" : std::string();
    if (script_source && !WriteText(content / script_path, script_source, error)) return false;
    if (kind != Kind::Blank) {
        std::error_code ec;
        stdfs::create_directories(content / "Blueprints", ec);
        if (!bp::SaveBlueprint(PickupBlueprint(), content / "Blueprints/BP_Pickup.abp", error)) return false;
        const bool look = kind == Kind::FirstPerson || kind == Kind::ThirdPerson;
        if (!WriteInput(content, look, look, error)) return false;
    }

    // The scene refers to them by GUID, which the asset database gives them.
    assets::AssetGuid script, pickup;
    if (kind != Kind::Blank) {
        assets::AssetDatabase database(content);
        database.Scan();
        const assets::AssetRecord* s = database.FindByPath(script_path);
        const assets::AssetRecord* p = database.FindByPath("Blueprints/BP_Pickup.abp");
        if (!s || !p) return Fail(error, "The template's assets weren't picked up by the asset database");
        script = s->guid;
        pickup = p->guid;
    }

    SceneBuilder scene;
    switch (kind) {
        case Kind::Blank:
            scene.Add(Vec3(0, 3, 8), PitchDown(0.3f), {"MainCamera"}, true);
            scene.Add(Vec3(0, 0, 0), Quaternion::Identity(), {"PlayerStart"});
            break;
        case Kind::FirstPerson:
            // The player is the camera, at eye height; the pickups ring it.
            scene.Add(Vec3(0, 1.7f, 0), Quaternion::Identity(), {"Player", "MainCamera"}, true, script);
            scene.Ring(pickup, 8, 6.0f, 0, 1.2f, 0);
            break;
        case Kind::ThirdPerson:
            scene.Add(Vec3(0, 0, 0), Quaternion::Identity(), {"Player"}, false, script);
            scene.Add(Vec3(0, 3, 6), PitchDown(0.3f), {"MainCamera"}, true);
            scene.Ring(pickup, 8, 7.0f, 0, 1.0f, 0);
            break;
        case Kind::TopDown:
            scene.Add(Vec3(0, 0, 0), Quaternion::Identity(), {"Player"}, false, script);
            scene.Add(Vec3(0, 16, 8), PitchDown(1.1f), {"MainCamera"}, true);
            scene.Ring(pickup, 6, 6.0f, 0, 0.8f, 0);
            scene.Ring(pickup, 10, 12.0f, 0, 0.8f, 0);
            break;
        case Kind::Vehicle:
            scene.Add(Vec3(0, 0.5f, 0), Quaternion::Identity(), {"Player"}, false, script);
            scene.Add(Vec3(0, 4, 9), PitchDown(0.35f), {"MainCamera"}, true);
            // A line of markers ahead to drive through.
            for (int i = 1; i <= 8; ++i) {
                scene.Add(Vec3((i % 2 == 0 ? 3.0f : -3.0f), 1.0f, -12.0f * static_cast<f32>(i)), Quaternion::Identity(),
                          {"Pickup"}, false, {}, pickup);
            }
            break;
    }
    if (!scene.Write(content / "Scenes/Main.ascene", error)) return false;

    ProjectSettings settings;
    if (!LoadProject(paths.file, settings, error)) return false;
    settings.startup_scene = "Scenes/Main.ascene";
    if (kind != Kind::Blank) settings.always_cook = {"Input/"}; // nothing in a scene refers to the bindings
    if (!SaveProject(paths.file, settings, error)) return false;

    AETHER_LOG_INFO("Templates", "Created %s from the %s template in %s", name.c_str(), entry->info.name.c_str(),
                    paths.root.string().c_str());
    if (out_paths) *out_paths = paths;
    return true;
}

} // namespace aether::templates
