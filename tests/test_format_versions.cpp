#include "test_framework.h"

#include "aether/animation/anim_graph.h"
#include "aether/animation/blend_space.h"
#include "aether/animation/montage.h"
#include "aether/blueprint/graph.h"
#include "aether/ecs/world.h"
#include "aether/renderer/material.h"
#include "aether/renderer/material_instance.h"
#include "aether/scene/prefab.h"
#include "aether/scene/serialization.h"
#include "aether/sequencer/sequence.h"
#include "aether/sprite2d/atlas.h"
#include "aether/sprite2d/tilemap.h"
#include "aether/ui/theme.h"

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

// Format-version audit (Phase 48 step 2): what every JSON loader does with a file from a newer engine.

using namespace aether;
using nlohmann::json;

namespace {

struct Format {
    const char* name;
    const char* type;
    std::function<bool(const json&, std::string&)> load;
};

std::vector<Format> Formats() {
    return {
        {"material", "Material", [](const json& j, std::string& e) { mat::Material m; return mat::MaterialFromJson(j, m, &e); }},
        {"material instance", "MaterialInstance", [](const json& j, std::string& e) { mat::MaterialInstance m; return mat::InstanceFromJson(j, m, &e); }},
        {"level sequence", "LevelSequence", [](const json& j, std::string& e) { seq::LevelSequence s; return seq::SequenceFromJson(j, s, &e); }},
        {"montage", "Montage", [](const json& j, std::string& e) { anim::Montage m; return anim::MontageFromJson(j, m, &e); }},
        {"animation graph", "AnimGraph", [](const json& j, std::string& e) { anim::AnimGraph g; return anim::AnimGraphFromJson(j, g, &e); }},
        {"blend space", "BlendSpace", [](const json& j, std::string& e) { anim::BlendSpace b; return anim::BlendSpaceFromJson(j, b, &e); }},
        {"prefab", "Prefab", [](const json& j, std::string& e) { PrefabData p; return PrefabFromJson(j, p, &e); }},
        {"blueprint", "Blueprint", [](const json& j, std::string& e) { bp::Blueprint b; return bp::BlueprintFromJson(j, b, &e); }},
        {"sprite atlas", "SpriteAtlas", [](const json& j, std::string& e) { sprite2d::SpriteAtlas a; return sprite2d::AtlasFromJson(j, a, &e); }},
        {"tileset", "Tileset", [](const json& j, std::string& e) { sprite2d::Tileset t; return sprite2d::TilesetFromJson(j, t, &e); }},
        {"tilemap", "Tilemap", [](const json& j, std::string& e) { sprite2d::TilemapData t; return sprite2d::TilemapFromJson(j, t, &e); }},
        {"scene", "Scene", [](const json& j, std::string& e) {
             World w; const std::string text = j.dump();
             (void)e; return LoadSceneJsonFromMemory(w, std::span<const u8>(reinterpret_cast<const u8*>(text.data()), text.size()), "probe"); }},
    };
}

} // namespace

AETHER_TEST(FormatVersions_EveryJsonLoaderRefusesAFileFromANewerEngine) {
    for (const Format& f : Formats()) {
        std::string error;
        const json doc = {{"$type", f.type}, {"$version", 9999}, {"name", "x"}, {"root", json::object()}, {"entities", json::array()}};
        AETHER_CHECK(!f.load(doc, error));
        if (std::string(f.type) != "Scene") AETHER_CHECK(!error.empty()); // the scene loader reports through the log
    }
}

AETHER_TEST(FormatVersions_SavedFilesCarryTheirVersionAndRoundTrip) {
    const auto check = [](const char* what, const json& saved, const std::function<bool(const json&, std::string&)>& load,
                          const std::function<json(const json&)>& resave) {
        std::string error;
        if (!saved.contains("$version") || !saved["$version"].is_number_integer()) {
            std::printf("    %s has no $version\n", what);
            return false;
        }
        if (!load(saved, error)) {
            std::printf("    %s doesn't load its own output: %s\n", what, error.c_str());
            return false;
        }
        const bool same = resave(saved) == saved;
        if (!same) std::printf("    %s doesn't save back to the same JSON\n", what);
        return same;
    };
    // Each format's own default document: loads, and saves back to the same JSON.
    {
        mat::Material m;
        const json saved = mat::MaterialToJson(m);
        AETHER_CHECK(check("material", saved, [](const json& j, std::string& e) { mat::Material x; return mat::MaterialFromJson(j, x, &e); },
                           [](const json& j) { mat::Material x; std::string e; (void)mat::MaterialFromJson(j, x, &e); return mat::MaterialToJson(x); }));
    }
    {
        anim::Montage m;
        const json saved = anim::MontageToJson(m);
        AETHER_CHECK(check("montage", saved, [](const json& j, std::string& e) { anim::Montage x; return anim::MontageFromJson(j, x, &e); },
                           [](const json& j) { anim::Montage x; std::string e; (void)anim::MontageFromJson(j, x, &e); return anim::MontageToJson(x); }));
    }
    {
        anim::BlendSpace b;
        const json saved = anim::BlendSpaceToJson(b);
        AETHER_CHECK(check("blend space", saved, [](const json& j, std::string& e) { anim::BlendSpace x; return anim::BlendSpaceFromJson(j, x, &e); },
                           [](const json& j) { anim::BlendSpace x; std::string e; (void)anim::BlendSpaceFromJson(j, x, &e); return anim::BlendSpaceToJson(x); }));
    }
    {
        bp::Blueprint b;
        const json saved = bp::BlueprintToJson(b);
        AETHER_CHECK(check("blueprint", saved, [](const json& j, std::string& e) { bp::Blueprint x; return bp::BlueprintFromJson(j, x, &e); },
                           [](const json& j) { bp::Blueprint x; std::string e; (void)bp::BlueprintFromJson(j, x, &e); return bp::BlueprintToJson(x); }));
    }
    {
        sprite2d::SpriteAtlas a;
        const json saved = sprite2d::AtlasToJson(a);
        AETHER_CHECK(check("sprite atlas", saved, [](const json& j, std::string& e) { sprite2d::SpriteAtlas x; return sprite2d::AtlasFromJson(j, x, &e); },
                           [](const json& j) { sprite2d::SpriteAtlas x; std::string e; (void)sprite2d::AtlasFromJson(j, x, &e); return sprite2d::AtlasToJson(x); }));
    }
    {
        sprite2d::Tileset t;
        t.tile_width = t.tile_height = 16;
        const json saved = sprite2d::TilesetToJson(t);
        AETHER_CHECK(check("tileset", saved, [](const json& j, std::string& e) { sprite2d::Tileset x; return sprite2d::TilesetFromJson(j, x, &e); },
                           [](const json& j) { sprite2d::Tileset x; std::string e; (void)sprite2d::TilesetFromJson(j, x, &e); return sprite2d::TilesetToJson(x); }));
    }
    {
        sprite2d::TilemapData m;
        m.Resize(2, 2);
        const json saved = sprite2d::TilemapToJson(m);
        AETHER_CHECK(check("tilemap", saved, [](const json& j, std::string& e) { sprite2d::TilemapData x; return sprite2d::TilemapFromJson(j, x, &e); },
                           [](const json& j) { sprite2d::TilemapData x; std::string e; (void)sprite2d::TilemapFromJson(j, x, &e); return sprite2d::TilemapToJson(x); }));
    }
}
