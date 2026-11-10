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
#include "aether/scene/hierarchy.h"
#include "aether/scene/script_component.h"
#include "aether/sprite2d/atlas.h"
#include "aether/sprite2d/components.h"
#include "aether/sprite2d/physics2d.h"
#include "aether/sprite2d/platformer.h"
#include "aether/sprite2d/tilemap.h"
#if AETHER_TEMPLATE_HAS_PHYSICS
#include "aether/physics/character.h"
#include "aether/physics/components.h"
#endif

#include <nlohmann/json.hpp>

#include <array>
#include <cmath>

namespace aether::templates {

using nlohmann::json;
namespace stdfs = std::filesystem;

namespace {

constexpr f32 kPi = 3.14159265358979f;

enum class Kind { Blank, FirstPerson, ThirdPerson, TopDown, Vehicle, Platformer2D };

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
        e.push_back({Info("platformer_2d", "2D Platformer", "Platformer",
                      "Run and jump through a side-scrolling level made of tiles.",
                      {"A tilemap level with ground, platforms, a pit and a step, on a tileset with an autotile",
                       "A character with 2D physics: A and D or the left stick to run, Space, W or A to jump",
                       "Coyote time, jump buffering, variable jump height; settings in the Inspector",
                       "A camera that follows the player, with pixel-perfect-friendly units (16 pixels a tile)"}),
                     Kind::Platformer2D});
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

// --- Greybox models ---------------------------------------------------------------

// A model a template's scene shows: where it is in the project and the GUID the asset database gave it.
struct GreyboxModel {
    std::string path;
    assets::AssetGuid guid;
};

template <typename T> void AppendBytes(std::vector<u8>& out, const T& value) {
    const u8* bytes = reinterpret_cast<const u8*>(&value);
    out.insert(out.end(), bytes, bytes + sizeof(T));
}

// Models/Cube.bin (a unit cube about the origin: 24 vertices with normals and UVs, 36 indices) and three .gltf files that
// draw it in different colours. Generated, like the rest of a template, so there is no art to ship or to go missing: the
// floor, the player and the pickups are scaled cubes until the game's own models replace them.
bool WriteGreyboxModels(const stdfs::path& content, std::string* error) {
    std::vector<u8> positions, normals, uvs, indices;
    // For each face: its normal and the two axes that span it, so that u x v points along the normal (counter-clockwise).
    const Vec3 faces[6][3] = {{Vec3(1, 0, 0), Vec3(0, 0, -1), Vec3(0, 1, 0)},  {Vec3(-1, 0, 0), Vec3(0, 0, 1), Vec3(0, 1, 0)},
                              {Vec3(0, 1, 0), Vec3(1, 0, 0), Vec3(0, 0, -1)},  {Vec3(0, -1, 0), Vec3(1, 0, 0), Vec3(0, 0, 1)},
                              {Vec3(0, 0, 1), Vec3(1, 0, 0), Vec3(0, 1, 0)},   {Vec3(0, 0, -1), Vec3(-1, 0, 0), Vec3(0, 1, 0)}};
    for (u16 f = 0; f < 6; ++f) {
        const Vec3 n = faces[f][0], u = faces[f][1], v = faces[f][2];
        const f32 corner[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
        for (const auto& c : corner) {
            const Vec3 p = (n + u * c[0] + v * c[1]) * 0.5f;
            for (f32 x : {p.x, p.y, p.z}) AppendBytes(positions, x);
            for (f32 x : {n.x, n.y, n.z}) AppendBytes(normals, x);
            AppendBytes(uvs, (c[0] + 1.0f) * 0.5f);
            AppendBytes(uvs, 1.0f - (c[1] + 1.0f) * 0.5f);
        }
        for (u16 i : {0, 1, 2, 0, 2, 3}) AppendBytes(indices, static_cast<u16>(f * 4 + i));
    }
    std::vector<u8> bin = positions;
    bin.insert(bin.end(), normals.begin(), normals.end());
    bin.insert(bin.end(), uvs.begin(), uvs.end());
    bin.insert(bin.end(), indices.begin(), indices.end());
    std::error_code ec;
    stdfs::create_directories(content / "Models", ec);
    if (!fs::WriteFileBytes((content / "Models/Cube.bin").string(), bin.data(), bin.size())) {
        return Fail(error, "Couldn't write " + (content / "Models/Cube.bin").string());
    }
    struct Look {
        const char* file;
        const char* material;
        f32 r, g, b;
    };
    for (const Look& look : {Look{"Floor", "GreyboxFloor", 0.45f, 0.47f, 0.50f}, Look{"Player", "GreyboxPlayer", 0.16f, 0.45f, 0.85f},
                             Look{"Pickup", "GreyboxPickup", 0.95f, 0.72f, 0.12f}}) {
        const json gltf = {
            {"asset", {{"version", "2.0"}, {"generator", "Aether project template"}}},
            {"buffers", json::array({{{"uri", "Cube.bin"}, {"byteLength", bin.size()}}})},
            {"bufferViews", json::array({{{"buffer", 0}, {"byteOffset", 0}, {"byteLength", positions.size()}},
                                         {{"buffer", 0}, {"byteOffset", positions.size()}, {"byteLength", normals.size()}},
                                         {{"buffer", 0}, {"byteOffset", positions.size() + normals.size()}, {"byteLength", uvs.size()}},
                                         {{"buffer", 0}, {"byteOffset", positions.size() + normals.size() + uvs.size()}, {"byteLength", indices.size()}}})},
            {"accessors", json::array({{{"bufferView", 0}, {"componentType", 5126}, {"count", 24}, {"type", "VEC3"},
                                        {"min", {-0.5, -0.5, -0.5}}, {"max", {0.5, 0.5, 0.5}}},
                                       {{"bufferView", 1}, {"componentType", 5126}, {"count", 24}, {"type", "VEC3"}},
                                       {{"bufferView", 2}, {"componentType", 5126}, {"count", 24}, {"type", "VEC2"}},
                                       {{"bufferView", 3}, {"componentType", 5123}, {"count", 36}, {"type", "SCALAR"}}})},
            {"materials", json::array({{{"name", look.material},
                                        {"pbrMetallicRoughness",
                                         {{"baseColorFactor", {look.r, look.g, look.b, 1.0}}, {"metallicFactor", 0.0}, {"roughnessFactor", 0.8}}}}})},
            {"meshes", json::array({{{"name", look.file},
                                     {"primitives", json::array({{{"attributes", {{"POSITION", 0}, {"NORMAL", 1}, {"TEXCOORD_0", 2}}},
                                                                  {"indices", 3}, {"material", 0}, {"mode", 4}}})}}})},
            {"nodes", json::array({{{"name", look.file}, {"mesh", 0}}})},
            {"scenes", json::array({{{"nodes", json::array({0})}}})},
            {"scene", 0},
        };
        const std::string text = gltf.dump(2) + "\n";
        const stdfs::path file = content / "Models" / (std::string(look.file) + ".gltf");
        if (!fs::WriteFileBytes(file.string(), text.data(), text.size())) return Fail(error, "Couldn't write " + file.string());
    }
    return true;
}

// --- The scene ------------------------------------------------------------------

struct SceneBuilder {
    json entities = json::array();

    // One entity: a transform, tags, and optionally a camera, a script and a Blueprint.
    void Add(Vec3 position, Quaternion rotation, const std::vector<std::string>& tags, bool camera = false,
             const assets::AssetGuid& script = {}, const assets::AssetGuid& blueprint = {}, bool character = false,
             const GreyboxModel* model = nullptr, Vec3 scale = Vec3(1.0f, 1.0f, 1.0f)) {
        json components = {{"Transform", reflect::ToJson(Transform{position, rotation, scale})}};
        if (model != nullptr) {
            // Something to see: without a model an entity is a point in space, and a new project opened on a black window.
            ModelRenderer renderer;
            renderer.model.guid = model->guid;
            SetModelPath(renderer, model->path);
            components["ModelRenderer"] = reflect::ToJson(renderer);
        }
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
#if AETHER_TEMPLATE_HAS_PHYSICS
        if (character) components["CharacterMovement"] = reflect::ToJson(CharacterMovement{});
#else
        (void)character;
#endif
        entities.push_back({{"guid", ToString(NewEntityGuid())}, {"components", std::move(components)}});
    }

#if AETHER_TEMPLATE_HAS_PHYSICS
    void AddGround() {
        BoxCollider collider;
        collider.half_extents = Vec3(40.0f, 0.5f, 40.0f);
        json components = {{"Transform", reflect::ToJson(Transform{Vec3(0.0f, -0.5f, 0.0f), Quaternion::Identity()})},
                           {"BoxCollider", reflect::ToJson(collider)}};
        entities.push_back({{"guid", ToString(NewEntityGuid())}, {"components", std::move(components)}});
    }
#endif

    // A model under the entity added last (parenting is by GUID): the visible body of an entity whose own transform a
    // controller script drives.
    void AddChildOfLast(Vec3 local_position, const std::vector<std::string>& tags, const GreyboxModel& model, Vec3 scale) {
        const std::string parent = entities.back()["guid"].get<std::string>();
        Add(local_position, Quaternion::Identity(), tags, false, {}, {}, false, &model, scale);
        Parent link;
        if (ParseEntityGuid(parent, link.parent)) entities.back()["components"]["Parent"] = reflect::ToJson(link);
    }

    // `count` pickups on a circle of `radius` about (cx, y, cz).
    void Ring(const assets::AssetGuid& blueprint, const GreyboxModel& model, int count, f32 radius, f32 cx, f32 y, f32 cz) {
        for (int i = 0; i < count; ++i) {
            const f32 a = 2.0f * kPi * static_cast<f32>(i) / static_cast<f32>(count);
            Add(Vec3(cx + radius * std::sin(a), y, cz - radius * std::cos(a)), Quaternion::Identity(), {"Pickup"}, false, {},
                blueprint, false, &model, Vec3(0.5f, 0.5f, 0.5f));
        }
    }

    // A slab to stand on, its top at y = 0. Scenery only: collision, where the build has physics, is AddGround's.
    void Floor(const GreyboxModel& model, f32 half_size) {
        Add(Vec3(0.0f, -0.1f, 0.0f), Quaternion::Identity(), {"Ground"}, false, {}, {}, false, &model,
            Vec3(half_size * 2.0f, 0.2f, half_size * 2.0f));
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

// --- The 2D Platformer ---------------------------------------------------------

void RegisterSprite2DTemplateComponents() {
    sprite2d::RegisterSprite2DComponents();
    sprite2d::RegisterPhysics2DComponents();
    sprite2d::RegisterPlatformerComponents();
}

// Move: A and D, the left stick. Jump: Space, W, A button; held (the jump
// is as high as the button is held), so no Pressed trigger.
bool WritePlatformerInput(const stdfs::path& content, std::string* error) {
    using namespace input;
    InputModifier dead = Modifier(ModifierType::DeadZone);
    dead.lower = 0.2f;
    InputMappingContext c;
    c.name = "Gameplay";
    c.bindings = {Bind("Move", Key::D), Bind("Move", Key::A, {Modifier(ModifierType::Negate)}),
                  Bind("Move", Key::Right), Bind("Move", Key::Left, {Modifier(ModifierType::Negate)}),
                  Bind("Move", Key::GamepadLeftStickX, {dead}),
                  Bind("Jump", Key::Space), Bind("Jump", Key::W), Bind("Jump", Key::Up), Bind("Jump", Key::GamepadA)};
    const stdfs::path dir = content / "Input";
    std::error_code ec;
    stdfs::create_directories(dir, ec);
    if (!SaveInputAction({"Move", ActionValueType::Axis2D}, dir / "Move.aaction", error)) return false;
    if (!SaveInputAction({"Jump", ActionValueType::Bool}, dir / "Jump.aaction", error)) return false;
    return SaveMappingContext(c, dir / "Gameplay.amapping", error);
}

// A level over an 8x4-tile sheet (tiles 16-31 are the ground autotile's sixteen
// shapes, all solid), 16 pixels a tile and a world unit a tile.
bool WritePlatformer(const ProjectPaths& paths, std::string* error) {
    using namespace sprite2d;
    const stdfs::path content = paths.content;
    if (!WritePlatformerInput(content, error)) return false;

    Tileset tileset;
    tileset.texture_width = 128;
    tileset.texture_height = 64;
    tileset.tile_width = tileset.tile_height = 16;
    std::array<i32, 16> shapes;
    for (int m = 0; m < 16; ++m) {
        shapes[static_cast<usize>(m)] = 16 + m;
        tileset.SetSolid(16 + m, true);
    }
    const usize ground = tileset.AddAutotile("Ground", shapes);
    if (!SaveTileset(content / "Tiles/Level.atileset", tileset, error)) return false;
    assets::AssetGuid tileset_guid;
    {
        assets::AssetDatabase database(content);
        database.Scan();
        const assets::AssetRecord* t = database.FindByPath("Tiles/Level.atileset");
        if (!t) return Fail(error, "The tileset wasn't picked up by the asset database");
        tileset_guid = t->guid;
    }

    TilemapData map;
    map.tileset = tileset_guid;
    map.Resize(48, 16);
    map.AddLayer("Ground");
    const i32 cell = AutotileCell(ground);
    map.Fill(0, 0, 0, 19, 1, cell);   // the ground, up to a pit
    map.Fill(0, 23, 0, 47, 1, cell);  // and after it
    map.Fill(0, 8, 5, 12, 5, cell);   // a platform
    map.Fill(0, 15, 8, 19, 8, cell);  // a higher one
    map.Fill(0, 26, 6, 31, 6, cell);
    map.Fill(0, 37, 2, 38, 3, cell);  // a step up
    map.Fill(0, 40, 2, 47, 5, cell);  // a tall block to the end
    if (!SaveTilemap(content / "Tiles/Level1.atilemap", map, error)) return false;
    assets::AssetGuid map_guid;
    {
        assets::AssetDatabase database(content);
        database.Scan();
        const assets::AssetRecord* m = database.FindByPath("Tiles/Level1.atilemap");
        if (!m) return Fail(error, "The tilemap wasn't picked up by the asset database");
        map_guid = m->guid;
    }

    SceneBuilder scene;
    const auto add = [&](Vec3 at, const char* tag, json extra) {
        json components = {{"Transform", reflect::ToJson(Transform{at, Quaternion::Identity()})}};
        if (tag) {
            Tags t;
            t.names = {tag};
            components["Tags"] = reflect::ToJson(t);
        }
        for (auto it = extra.begin(); it != extra.end(); ++it) components[it.key()] = it.value();
        scene.entities.push_back({{"guid", ToString(NewEntityGuid())}, {"components", std::move(components)}});
    };
    Camera camera;
    camera.projection = Projection::Orthographic;
    camera.ortho_height = 11.25f; // a 320x180 view at 16 pixels a tile
    camera.near_plane = -50.0f;
    camera.far_plane = 50.0f;
    CameraFollow2D follow;
    follow.use_bounds = true;
    follow.min_bounds = {10.0f, 5.6f};
    follow.max_bounds = {38.0f, 10.4f};
    add(Vec3(10, 6, 10), "MainCamera", {{"Camera", reflect::ToJson(camera)}, {"CameraFollow2D", reflect::ToJson(follow)}});
    TilemapRenderer renderer;
    renderer.tilemap.guid = map_guid;
    renderer.pixels_per_unit = 16.0f;
    add(Vec3(0, 0, 0), "Level", {{"TilemapRenderer", reflect::ToJson(renderer)}});
    // A textureless atlas still renders as a tinted box in the player. This gives a new project a
    // visible, editable character placeholder before the user imports art.
    sprite2d::SpriteAtlas player_atlas;
    player_atlas.width = 16;
    player_atlas.height = 16;
    player_atlas.frames.push_back({"idle", 0, 0, 13, 16, 0.5f, 0.5f});
    std::error_code atlas_ec;
    stdfs::create_directories(content / "Sprites", atlas_ec);
    if (!sprite2d::SaveAtlas(content / "Sprites/Player.aatlas", player_atlas, error)) return false;
    assets::AssetDatabase atlas_database(content);
    atlas_database.Scan();
    const assets::AssetRecord* player_atlas_asset = atlas_database.FindByPath("Sprites/Player.aatlas");
    if (!player_atlas_asset) return Fail(error, "The player sprite atlas wasn't picked up by the asset database");
    sprite2d::Sprite player_sprite;
    player_sprite.atlas.guid = player_atlas_asset->guid;
    sprite2d::SetSpriteFrame(player_sprite, "idle");
    player_sprite.color = {0.16f, 0.45f, 0.85f, 1.0f};
    player_sprite.order = 1;
    Collider2D body;
    body.half_extents = {0.4f, 0.5f};
    body.friction = 0.0f; // the controller sets the speed, so the floor shouldn't fight it
    add(Vec3(3, 3, 0), "Player",
        {{"Sprite", reflect::ToJson(player_sprite)},
         {"Rigidbody2D", reflect::ToJson(Rigidbody2D{})},
         {"Collider2D", reflect::ToJson(body)},
         {"PlatformerController2D", reflect::ToJson(PlatformerController2D{})}});
    if (!scene.Write(content / "Scenes/Main.ascene", error)) return false;

    ProjectSettings settings;
    if (!LoadProject(paths.file, settings, error)) return false;
    settings.startup_scene = "Scenes/Main.ascene";
    settings.always_cook = {"Input/"};
    return SaveProject(paths.file, settings, error);
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
    if (kind == Kind::Platformer2D) {
        RegisterSprite2DTemplateComponents();
        if (!WritePlatformer(paths, error)) return false;
        AETHER_LOG_INFO("Templates", "Created %s from the %s template in %s", name.c_str(), entry->info.name.c_str(),
                        paths.root.string().c_str());
        if (out_paths) *out_paths = paths;
        return true;
    }

    // Scripts, the pickup Blueprint and the input bindings.
    const char* script_source = nullptr;
    const char* script_name = nullptr;
    std::string script_storage;
    switch (kind) {
        case Kind::FirstPerson: script_source = kFirstPersonScript, script_name = "FirstPersonController"; break;
        case Kind::ThirdPerson: script_source = kThirdPersonScript, script_name = "ThirdPersonController"; break;
        case Kind::TopDown: script_source = kTopDownScript, script_name = "TopDownController"; break;
        case Kind::Vehicle: script_source = kVehicleScript, script_name = "VehicleController"; break;
        case Kind::Blank: break;
    }
    if (kind == Kind::ThirdPerson) {
        script_storage = kThirdPersonScript;
        constexpr const char* marker = "@@CHARACTER_MOVEMENT_LOOKUP@@";
#if AETHER_TEMPLATE_HAS_PHYSICS
        constexpr const char* lookup = "self.character = self.entity:Get(\"CharacterMovement\")";
#else
        constexpr const char* lookup = "self.character = nil";
#endif
        if (const usize at = script_storage.find(marker); at != std::string::npos) {
            script_storage.replace(at, std::char_traits<char>::length(marker), lookup);
        }
        script_source = script_storage.c_str();
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

    if (kind != Kind::Blank && !WriteGreyboxModels(content, error)) return false;

    // The scene refers to them by GUID, which the asset database gives them.
    assets::AssetGuid script, pickup;
    GreyboxModel floor_model{"Models/Floor.gltf", {}}, player_model{"Models/Player.gltf", {}}, pickup_model{"Models/Pickup.gltf", {}};
    if (kind != Kind::Blank) {
        assets::AssetDatabase database(content);
        database.Scan();
        for (GreyboxModel* model : {&floor_model, &player_model, &pickup_model}) {
            const assets::AssetRecord* record = database.FindByPath(model->path);
            if (!record) return Fail(error, "The template's models weren't picked up by the asset database");
            model->guid = record->guid;
        }
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
            scene.Floor(floor_model, 20.0f);
            scene.Ring(pickup, pickup_model, 8, 6.0f, 0, 1.2f, 0);
            break;
        case Kind::ThirdPerson:
            // The entity stands at its feet; the body is a child so the box sits on the ground, not half in it.
            scene.Add(Vec3(0, 0, 0), Quaternion::Identity(), {"Player"}, false, script, {}, true);
            scene.AddChildOfLast(Vec3(0, 0.9f, 0), {"PlayerBody"}, player_model, Vec3(0.6f, 1.8f, 0.6f));
            scene.Add(Vec3(0, 3, 6), PitchDown(0.3f), {"MainCamera"}, true);
            scene.Floor(floor_model, 20.0f);
#if AETHER_TEMPLATE_HAS_PHYSICS
            scene.AddGround();
#endif
            scene.Ring(pickup, pickup_model, 8, 7.0f, 0, 1.0f, 0);
            break;
        case Kind::TopDown:
            scene.Add(Vec3(0, 0, 0), Quaternion::Identity(), {"Player"}, false, script);
            scene.AddChildOfLast(Vec3(0, 0.9f, 0), {"PlayerBody"}, player_model, Vec3(0.6f, 1.8f, 0.6f));
            scene.Add(Vec3(0, 16, 8), PitchDown(1.1f), {"MainCamera"}, true);
            scene.Floor(floor_model, 20.0f);
            scene.Ring(pickup, pickup_model, 6, 6.0f, 0, 0.8f, 0);
            scene.Ring(pickup, pickup_model, 10, 12.0f, 0, 0.8f, 0);
            break;
        case Kind::Vehicle:
            scene.Add(Vec3(0, 0.5f, 0), Quaternion::Identity(), {"Player"}, false, script);
            scene.AddChildOfLast(Vec3(0, 0.1f, 0), {"PlayerBody"}, player_model, Vec3(1.8f, 1.0f, 4.0f));
            scene.Add(Vec3(0, 4, 9), PitchDown(0.35f), {"MainCamera"}, true);
            // A road long enough for the markers ahead.
            scene.Add(Vec3(0.0f, -0.1f, -50.0f), Quaternion::Identity(), {"Ground"}, false, {}, {}, false, &floor_model,
                      Vec3(30.0f, 0.2f, 140.0f));
            // A line of markers ahead to drive through.
            for (int i = 1; i <= 8; ++i) {
                scene.Add(Vec3((i % 2 == 0 ? 3.0f : -3.0f), 1.0f, -12.0f * static_cast<f32>(i)), Quaternion::Identity(),
                          {"Pickup"}, false, {}, pickup, false, &pickup_model, Vec3(0.5f, 0.5f, 0.5f));
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
