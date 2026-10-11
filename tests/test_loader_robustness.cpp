#include "test_framework.h"

#include "aether/blueprint/graph.h"
#include "aether/core/json_util.h"
#include "aether/ecs/world.h"
#include "aether/input/bindings.h"
#include "aether/save/envelope.h"
#include "aether/animation/anim_graph.h"
#include "aether/animation/blend_space.h"
#include "aether/animation/montage.h"
#include "aether/player/game.h"
#include "aether/renderer/material.h"
#include "aether/renderer/material_instance.h"
#include "aether/scene/prefab.h"
#include "aether/scene/serialization.h"
#include "aether/sprite2d/atlas.h"
#include "aether/sprite2d/tilemap.h"
#include "aether/sequencer/sequence.h"

#include <cstdio>
#include <string>
#include <vector>

// Loaders must answer false on a malformed file instead of throwing (Phase 47 step 1: found by the fuzzers).

using namespace aether;

namespace {
std::vector<u8> Bytes(const std::string& s) { return std::vector<u8>(s.begin(), s.end()); }
} // namespace

AETHER_TEST(LoaderRobustness_JsonVersionIsSafe) {
    using nlohmann::json;
    AETHER_CHECK(JsonVersion(json{{"$version", 3}}) == 3);
    AETHER_CHECK(JsonVersion(json{{"$version", "3"}}) == 0 && JsonVersion(json{{"$version", json::array()}}) == 0 && JsonVersion(json{{"$version", 1.5}}) == 0);
    AETHER_CHECK(JsonVersion(json::object()) == 0 && JsonVersion(json::array()) == 0 && JsonVersion(json(5)) == 0 && JsonVersion(json::object(), 7) == 7);
    AETHER_CHECK(JsonVersion(json{{"$version", 9999999999ll}}) == 0); // outside int: not a version
}

AETHER_TEST(LoaderRobustness_SceneAndBlueprintWithWrongTypesDontThrow) {
    World world;
    for (const char* text : {R"({"$type":"Scene","$version":"1","entities":[]})", R"({"$type":"Scene","$version":[1],"entities":[]})", R"({"$type":["Scene"],"$version":1})",
                             R"({"$type":"Scene","$version":1,"entities":[{"guid":5,"components":{}}]})", R"({"$type":"Scene","$version":1,"entities":[{"components":[]}]})"}) {
        const std::vector<u8> bytes = Bytes(text);
        bool threw = false, loaded = true;
        try {
            loaded = LoadSceneJsonFromMemory(world, bytes, "test");
        } catch (...) {
            threw = true;
        }
        AETHER_CHECK(!threw);
        (void)loaded;
    }
    for (const char* text : {R"({"$type":"Blueprint","$version":"BP"})", R"({"$type":"Blueprint","$version":1,"parent":5})", R"({"$type":"Blueprint","$version":1,"variables":[5]})",
                             R"({"$type":"Blueprint","$version":1,"graphs":[{"name":7,"nodes":[]}]})", R"({"$type":"Blueprint","$version":1,"dispatchers":[{"name":3}]})"}) {
        const nlohmann::json j = nlohmann::json::parse(text);
        bp::Blueprint blueprint;
        std::string error;
        bool threw = false;
        try {
            (void)bp::BlueprintFromJson(j, blueprint, &error);
        } catch (...) {
            threw = true;
        }
        AETHER_CHECK(!threw);
    }
    // And the right shape still loads.
    AETHER_CHECK(LoadSceneJsonFromMemory(world, Bytes(R"({"$type":"Scene","$version":1,"entities":[]})"), "ok"));
}

namespace {
// Each document has a value of the wrong type where the loader expects another; none may throw.
const char* const kWrongTypes[] = {
    R"({"$version":"1"})",
    R"({"$version":[1],"name":7,"$type":5})",
    R"({"$version":1,"nodes":5,"entities":"x","tracks":{"a":1},"machines":3,"samples":"s","sections":false})",
    R"({"$version":1,"nodes":[1,"a",null],"entities":[[]],"tracks":[5],"machines":[7],"samples":[{}],"sections":[1]})",
    R"([])",
    R"(5)",
    R"("text")",
    R"(null)",
};
} // namespace

AETHER_TEST(LoaderRobustness_OtherJsonLoadersWithWrongTypesDontThrow) {
    bool threw = false;
    for (const char* text : kWrongTypes) {
        const auto json = nlohmann::json::parse(text);
        std::string error;
        try {
            PrefabData prefab;
            PrefabFromJson(json, prefab, &error);
            mat::Material material;
            mat::MaterialFromJson(json, material, &error);
            mat::MaterialInstance instance;
            mat::InstanceFromJson(json, instance, &error);
            seq::LevelSequence sequence;
            seq::SequenceFromJson(json, sequence, &error);
            anim::AnimGraph graph;
            anim::AnimGraphFromJson(json, graph, &error);
            anim::BlendSpace space;
            anim::BlendSpaceFromJson(json, space, &error);
            anim::Montage montage;
            anim::MontageFromJson(json, montage, &error);
        } catch (...) {
            threw = true;
            std::printf("  loader threw on: %s\n", text);
        }
    }
    AETHER_CHECK(!threw);
}

AETHER_TEST(LoaderRobustness_ManifestWithWrongTypesFailsCleanly) {
    for (const char* text : {R"({"$type":5})", R"({"$type":"CookManifest","$version":1,"fixed_timestep_hz":"x"})",
                             R"({"$type":"CookManifest","$version":1,"gravity":["a",1,2]})",
                             R"({"$type":"CookManifest","$version":1,"window":{"width":"w"}})"}) {
        player::GameManifest manifest;
        std::string error;
        AETHER_CHECK(!player::ParseGameManifest(text, manifest, &error));
        AETHER_CHECK(!error.empty());
    }
}

AETHER_TEST(LoaderRobustness_JsonStringReadsOnlyStrings) {
    using nlohmann::json;
    AETHER_CHECK(JsonString(json{{"$type", "Scene"}}, "$type") == "Scene");
    AETHER_CHECK(JsonString(json{{"$type", 5}}, "$type").empty() && JsonString(json{{"$type", json::array()}}, "$type").empty());
    AETHER_CHECK(JsonString(json::object(), "$type", "none") == "none" && JsonString(json::array(), "$type").empty() && JsonString(json(7), "$type").empty());
}

AETHER_TEST(LoaderRobustness_InputAssetsWithNonStringTypeFailCleanly) {
    input::InputAssetLibrary library;
    std::string error;
    for (const char* text : {R"({"$type":5,"data":{}})", R"({"$type":["InputAction"],"data":{}})", R"({"$type":null})", R"([])"}) {
        AETHER_CHECK(!library.AddFromText("InputAction", "bad.aaction", text, &error));
        AETHER_CHECK(!library.AddFromText("InputMapping", "bad.amapping", text, &error));
    }
}

AETHER_TEST(LoaderRobustness_SpriteAtlasTilesetAndTilemapWithWrongTypesFailCleanly) {
    using nlohmann::json;
    std::string error;
    sprite2d::SpriteAtlas atlas;
    for (const char* text : {R"({"$type":"SpriteAtlas","texture":5})", R"({"$type":"SpriteAtlas","frames":"x"})", R"({"$type":"SpriteAtlas","frames":[{"name":5}]})",
                             R"({"$type":"SpriteAtlas","frames":[{"name":"a","w":8,"h":8,"pivot":["a","b"]}]})"}) {
        AETHER_CHECK(!sprite2d::AtlasFromJson(json::parse(text), atlas, &error));
    }
    sprite2d::Tileset tileset;
    for (const char* text : {R"({"$type":"Tileset","tile_size":[]})", R"({"$type":"Tileset","tile_size":["a","b"]})", R"({"$type":"Tileset","tile_size":[16,16],"solid":["x"]})",
                             R"({"$type":"Tileset","tile_size":[16,16],"solid":[2147483647]})", R"({"$type":"Tileset","tile_size":[16,16],"autotiles":[{"tiles":["a"]}]})"}) {
        AETHER_CHECK(!sprite2d::TilesetFromJson(json::parse(text), tileset, &error));
    }
    AETHER_CHECK(sprite2d::TilesetFromJson(json::parse(R"({"$type":"Tileset","tile_size":[16,16],"solid":[3]})"), tileset, &error));
    sprite2d::TilemapData map;
    for (const char* text : {R"({"$type":"Tilemap","size":["a",1]})", R"({"$type":"Tilemap","size":[2,1],"layers":[{"name":"g","cells":"no"}]})", R"({"$type":"Tilemap","size":[2,1],"layers":[7]})"}) {
        AETHER_CHECK(!sprite2d::TilemapFromJson(json::parse(text), map, &error));
    }
    AETHER_CHECK(sprite2d::TilemapFromJson(json::parse(R"({"$type":"Tilemap","size":[2,1],"layers":[{"name":"g","cells":[0,1]}]})"), map, &error));
}
