#include "test_framework.h"

#include "aether/docs/api_docs.h"
#include "aether/ecs/component.h"
#include "aether/sprite2d/components.h"
#include "aether/sprite2d/lights2d.h"
#include "aether/sprite2d/physics2d.h"
#include "aether/sprite2d/platformer.h"

#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>

// The API reference generated from reflection (Phase 26 step 5, §26.7).

using namespace aether;

#define CHECK(...) AETHER_CHECK((__VA_ARGS__))

namespace docs_test {

enum class DocMode : u8 { Fast, Slow = 5 };

struct DocGadget {
    f32 speed = 2.0f;
    DocMode mode = DocMode::Fast;
    std::vector<f32> samples;
    char label[12] = {};
    bool hidden = false;
    i32 count = 0;
    void Reset() { speed = 0; }
    f32 Scaled(f32 by) const { return speed * by; }
    static i32 Twice(i32 v) { return v * 2; }
};

} // namespace docs_test

AETHER_ENUM(docs_test::DocMode, 1, AETHER_ENUM_VALUE(Fast), AETHER_ENUM_VALUE(Slow))

AETHER_REFLECT(docs_test::DocGadget, 1,
    AETHER_FIELD(speed, Field_EditAnywhere, {.tooltip = "How fast | per second", .range_min = 0.0, .range_max = 10.0, .units = "m/s"}),
    AETHER_FIELD(mode, Field_EditAnywhere),
    AETHER_FIELD(samples, Field_ReadOnly),
    AETHER_FIELD(label, Field_EditAnywhere, {.category = "Text"}),
    AETHER_FIELD(hidden, Field_Transient),
    AETHER_FIELD(count, Field_EditAnywhere | Field_Replicated),
    AETHER_METHOD(Reset, Fn_BlueprintCallable),
    AETHER_METHOD(Scaled, Fn_BlueprintCallable | Fn_Pure),
    AETHER_METHOD(Twice, Fn_BlueprintCallable)
)

AETHER_TEST(ApiDocs_DocumentsAReflectedType) {
    (void)GetComponentId<docs_test::DocGadget>(); // registered with the ECS: a Component
    const std::string md = docs::GenerateApiMarkdown();
    CHECK(md.find("# Aether API reference") == 0);
    CHECK(md.find("### DocGadget") != std::string::npos);
    const usize at = md.find("### DocGadget");
    const std::string section = md.substr(at, md.find("### ", at + 5) - at);
    CHECK(section.find("*Component,") != std::string::npos);
    // The table: types, editing, details.
    CHECK(section.find("| `speed` | `f32` | editable | How fast \\| per second; range 0 to 10; unit m/s |") != std::string::npos);
    CHECK(section.find("| `mode` | [`DocMode`](#docmode) | editable |") != std::string::npos);
    CHECK(section.find("| `samples` | `Array<f32>` | read-only |") != std::string::npos);
    CHECK(section.find("| `label` | `char[12]` | editable | group Text |") != std::string::npos);
    CHECK(section.find("| `hidden` | `bool` | internal, not saved |") != std::string::npos);
    CHECK(section.find("| `count` | `i32` | editable, replicated |") != std::string::npos);
    // The functions, with their flags.
    CHECK(section.find("`Reset()` (Blueprint)") != std::string::npos);
    CHECK(section.find("`Scaled(") != std::string::npos && section.find(") -> f32` (Blueprint, pure, const)") != std::string::npos);
    CHECK(section.find("`Twice(") != std::string::npos && section.find("static") != std::string::npos);
    // The enum has its own entry with its values; the contents list links both.
    const usize e = md.find("### DocMode");
    CHECK(e != std::string::npos);
    CHECK(md.find("| `Slow` | 5 |", e) != std::string::npos && md.find("| `Fast` | 0 |", e) != std::string::npos);
    CHECK(md.find("[DocGadget](#docgadget)") != std::string::npos && md.find("**Enums**") != std::string::npos);
}

AETHER_TEST(ApiDocs_JsonHasTheSameFacts) {
    const nlohmann::json j = docs::GenerateApiJson();
    CHECK(j["title"] == "Aether API reference" && j["types"].is_array());
    const nlohmann::json* gadget = nullptr;
    const nlohmann::json* mode = nullptr;
    for (const nlohmann::json& t : j["types"]) {
        if (t["name"] == "DocGadget") gadget = &t;
        if (t["name"] == "DocMode") mode = &t;
    }
    CHECK(gadget && mode);
    CHECK((*gadget)["fields"].size() == 6);
    const nlohmann::json& speed = (*gadget)["fields"][0];
    CHECK(speed["name"] == "speed" && speed["type"] == "f32" && speed["editing"] == "editable" && speed["units"] == "m/s");
    CHECK(speed["range"][1] == 10.0 && speed["tooltip"] == "How fast | per second");
    CHECK((*gadget)["fields"][2]["type"] == "Array<f32>" && (*gadget)["fields"][2]["editing"] == "read-only");
    CHECK((*gadget)["functions"].size() == 3 && (*gadget)["functions"][1]["flags"] == "Blueprint, pure, const");
    CHECK((*mode)["category"] == "Enum" && (*mode)["values"].size() == 2 && (*mode)["values"][1]["value"] == 5);
    CHECK((*gadget)["category"] == "Component");
}

AETHER_TEST(ApiDocs_SortedLinkedAndOptions) {
    sprite2d::RegisterSprite2DComponents();
    sprite2d::RegisterPhysics2DComponents();
    sprite2d::RegisterPlatformerComponents();
    sprite2d::RegisterLight2DComponents();
    const nlohmann::json j = docs::GenerateApiJson();
    std::string previous;
    std::set<std::string> seen;
    for (const nlohmann::json& t : j["types"]) {
        const std::string name = t["name"];
        CHECK(seen.insert(name).second); // one entry per name
        CHECK(name != "f32" && name != "bool" && name != "std::string"); // plain data isn't an entry
        CHECK(name.rfind("AssetRef<", 0) != 0);
    }
    // Sorted within the groups (components, structs, enums come out in name order).
    const std::string md = docs::GenerateApiMarkdown();
    const usize collider = md.find("### Collider2D"), light = md.find("### Light2D"), sprite = md.find("### Sprite\n");
    CHECK(collider != std::string::npos && light != std::string::npos && sprite != std::string::npos);
    CHECK(collider < light && light < sprite);
    // Types link to their entries: Sprite2D's atlas is a documented AssetRef, written as itself.
    const usize sprite_section = sprite;
    const std::string section = md.substr(sprite_section, md.find("### ", sprite_section + 5) - sprite_section);
    CHECK(section.find("| `atlas` | `AssetRef<SpriteAtlas>` |") != std::string::npos);
    CHECK(section.find("| `color` | [`SpriteColor`](#spritecolor) |") != std::string::npos);
    // The lights' enum and a tooltip with units.
    CHECK(md.find("| `Spot` | 2 |") != std::string::npos);
    CHECK(md.find("Where a point or spot light reaches 0; range 0 to 1000; unit m") != std::string::npos);
    // With scalars on, the numbers get entries too, and the title option changes the heading.
    docs::ApiDocOptions all;
    all.skip_scalars = false;
    all.title = "Everything";
    const std::string with = docs::GenerateApiMarkdown(all);
    CHECK(with.find("# Everything") == 0 && with.find("### f32") != std::string::npos && md.find("### f32") == std::string::npos);
}

AETHER_TEST(ApiDocs_WritesBothFiles) {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "aether_api_docs" / "out";
    std::error_code ec;
    std::filesystem::remove_all(dir.parent_path(), ec);
    std::string error;
    CHECK(docs::WriteApiDocs(dir, {}, &error));
    CHECK(std::filesystem::exists(dir / "API.md") && std::filesystem::exists(dir / "api.json"));
    std::ifstream in(dir / "api.json");
    const nlohmann::json j = nlohmann::json::parse(in, nullptr, false);
    CHECK(!j.is_discarded() && j["types"].size() > 20);
    std::ifstream md(dir / "API.md");
    std::stringstream text;
    text << md.rdbuf();
    CHECK(text.str() == docs::GenerateApiMarkdown()); // deterministic
    // A path that can't be a directory is reported.
    std::ofstream(dir / "file") << "x";
    CHECK(!docs::WriteApiDocs(dir / "file" / "sub", {}, &error) && !error.empty());
}

AETHER_TEST(ApiDocs_NamesTypesTheWayTheCodeDoes) {
    CHECK(docs::DisplayTypeName(reflect::Reflect<f32>()) == "f32");
    CHECK(docs::DisplayTypeName(reflect::Reflect<std::vector<i32>>()) == "Array<i32>");
    CHECK(docs::DisplayTypeName(reflect::Reflect<sprite2d::Sprite>().FindField("atlas")->type[0]) == "AssetRef<SpriteAtlas>");
    CHECK(docs::TypeCategory(reflect::Reflect<docs_test::DocMode>()) == "Enum");
    CHECK(docs::TypeCategory(reflect::Reflect<Vec3>()) == "Struct");
    (void)GetComponentId<sprite2d::Collider2D>();
    CHECK(docs::TypeCategory(reflect::Reflect<sprite2d::Collider2D>()) == "Component");
}
