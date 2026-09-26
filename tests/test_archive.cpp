#include "aether/reflection/reflection.h"
#include "aether/reflection/serialize.h"
#include "test_framework.h"

#include <string>

using namespace aether;
using namespace aether::reflect;

namespace save {

enum class Mood : u8 { Calm, Angry, Sleepy };

struct Stats {
    i32 level = 1;
    u64 experience = 0;
    f64 precise = 0.0;
};

struct Everything {
    bool flag = false;
    i8 tiny = 0;
    i16 small = 0;
    i32 medium = 0;
    i64 large = 0;
    u8 byte = 0;
    u16 ushort = 0;
    u32 uint = 0;
    u64 ulong = 0;
    f32 single = 0.0f;
    f64 dbl = 0.0;
    std::string text;
    Mood mood = Mood::Calm;
    Vec3 position;
    Quaternion rotation;
    Mat4 matrix;
    Stats stats;
    i32 cache = 0; // Transient: never saved
};

// "v2" of a type whose v1 stored `hp` (int) instead of `health` (float).
struct Creature {
    f32 health = 10.0f;
    std::string name = "unnamed";
};

// Declared with the same fields as Creature but a lower version, to write
// genuine "v1-shaped" data where needed.
struct Grown {
    i32 legs = 4;
    std::string name = "default";
    f32 added_later = 5.0f;
};

} // namespace save

AETHER_ENUM(save::Mood, 1, AETHER_ENUM_VALUE(Calm), AETHER_ENUM_VALUE(Angry), AETHER_ENUM_VALUE(Sleepy))
AETHER_REFLECT(save::Stats, 1, AETHER_FIELD(level), AETHER_FIELD(experience), AETHER_FIELD(precise))
AETHER_REFLECT(save::Everything, 3,
    AETHER_FIELD(flag), AETHER_FIELD(tiny), AETHER_FIELD(small), AETHER_FIELD(medium), AETHER_FIELD(large),
    AETHER_FIELD(byte), AETHER_FIELD(ushort), AETHER_FIELD(uint), AETHER_FIELD(ulong),
    AETHER_FIELD(single), AETHER_FIELD(dbl), AETHER_FIELD(text), AETHER_FIELD(mood),
    AETHER_FIELD(position), AETHER_FIELD(rotation), AETHER_FIELD(matrix), AETHER_FIELD(stats),
    AETHER_FIELD(cache, Field_Transient)
)
AETHER_REFLECT(save::Creature, 2, AETHER_FIELD(health), AETHER_FIELD(name))
AETHER_REFLECT(save::Grown, 1, AETHER_FIELD(legs), AETHER_FIELD(name), AETHER_FIELD(added_later))

namespace {

save::Everything MakeInteresting() {
    save::Everything e;
    e.flag = true;
    e.tiny = -128;
    e.small = -32000;
    e.medium = -2000000000;
    e.large = -9000000000000000000ll;
    e.byte = 255;
    e.ushort = 65535;
    e.uint = 4000000000u;
    e.ulong = 18000000000000000000ull;
    e.single = 0.1f;
    e.dbl = 3.141592653589793;
    e.text = "quote \" backslash \\ unicode \xC3\xA9 newline \n";
    e.mood = save::Mood::Sleepy;
    e.position = Vec3(1.5f, -2.25f, 1e-7f);
    e.rotation = Quaternion(0.0f, 0.70710677f, 0.0f, 0.70710677f);
    e.matrix = Mat4::Translation(Vec3(4, 5, 6));
    e.stats.level = 42;
    e.stats.experience = 123456789012ull;
    e.stats.precise = 1.0 / 3.0;
    e.cache = 777;
    return e;
}

bool SameEverything(const save::Everything& a, const save::Everything& b) {
    bool same = a.flag == b.flag && a.tiny == b.tiny && a.small == b.small && a.medium == b.medium &&
                a.large == b.large && a.byte == b.byte && a.ushort == b.ushort && a.uint == b.uint &&
                a.ulong == b.ulong && a.single == b.single && a.dbl == b.dbl && a.text == b.text &&
                a.mood == b.mood && a.position.x == b.position.x && a.position.y == b.position.y &&
                a.position.z == b.position.z && a.rotation.x == b.rotation.x && a.rotation.y == b.rotation.y &&
                a.rotation.z == b.rotation.z && a.rotation.w == b.rotation.w && a.stats.level == b.stats.level &&
                a.stats.experience == b.stats.experience && a.stats.precise == b.stats.precise;
    for (int c = 0; c < 4; ++c) {
        same = same && a.matrix.cols[c].x == b.matrix.cols[c].x && a.matrix.cols[c].y == b.matrix.cols[c].y &&
               a.matrix.cols[c].z == b.matrix.cols[c].z && a.matrix.cols[c].w == b.matrix.cols[c].w;
    }
    return same;
}

} // namespace

AETHER_TEST(Archive_JsonRoundTrip_AllKinds) {
    save::Everything original = MakeInteresting();
    std::string text = SaveJsonText(original);

    save::Everything loaded;
    LoadReport report;
    AETHER_CHECK(LoadJsonText(loaded, text, &report));
    AETHER_CHECK(report.warnings.empty());
    AETHER_CHECK(SameEverything(original, loaded));
    AETHER_CHECK(loaded.cache == 0); // transient: not saved, keeps its default
}

AETHER_TEST(Archive_BinaryRoundTrip_AllKinds) {
    save::Everything original = MakeInteresting();
    std::vector<u8> bytes = SaveBinary(original);
    AETHER_CHECK(bytes.size() < SaveJsonText(original).size()); // it's the compact form

    save::Everything loaded;
    LoadReport report;
    AETHER_CHECK(LoadBinary(loaded, bytes, &report));
    AETHER_CHECK(report.warnings.empty());
    AETHER_CHECK(SameEverything(original, loaded));
}

AETHER_TEST(Archive_JsonIsReadableAndDeterministic) {
    save::Everything original = MakeInteresting();
    Json json = ToJson(original);

    AETHER_CHECK(json["$v"] == 3);
    AETHER_CHECK(json["single"].dump() == "0.1"); // shortest f32 form, not 0.10000000149011612
    AETHER_CHECK(json["mood"] == "Sleepy");       // enums by name
    AETHER_CHECK(json["position"].is_array() && json["position"].size() == 3); // math types as arrays
    AETHER_CHECK(json["position"][0] == 1.5);
    AETHER_CHECK(json["matrix"].is_array() && json["matrix"][3][0] == 4.0);
    AETHER_CHECK(json["stats"].is_object() && json["stats"]["$v"] == 1 && json["stats"]["level"] == 42);
    AETHER_CHECK(!json.contains("cache"));

    // Same object -> byte-identical text, with sorted keys.
    AETHER_CHECK(SaveJsonText(original) == SaveJsonText(original));
    std::string text = SaveJsonText(original);
    AETHER_CHECK(text.find("\"byte\"") < text.find("\"dbl\""));
    AETHER_CHECK(text.find("\"dbl\"") < text.find("\"flag\""));

    // Save -> load -> save is stable.
    save::Everything loaded;
    AETHER_CHECK(LoadJsonText(loaded, text));
    AETHER_CHECK(SaveJsonText(loaded) == text);
}

AETHER_TEST(Archive_AddedFieldGetsDefault) {
    // Written before `added_later` existed.
    save::Grown loaded;
    LoadReport report;
    AETHER_CHECK(LoadJsonText(loaded, R"({"$v": 1, "legs": 6, "name": "beetle"})", &report));
    AETHER_CHECK(report.warnings.empty());
    AETHER_CHECK(loaded.legs == 6 && loaded.name == "beetle");
    AETHER_CHECK(loaded.added_later == 5.0f);
}

AETHER_TEST(Archive_RemovedFieldIgnored) {
    save::Grown loaded;
    LoadReport report;
    AETHER_CHECK(LoadJsonText(loaded, R"({"$v": 1, "legs": 2, "wings": 4, "old": {"deep": [1, 2]}})", &report));
    AETHER_CHECK(report.warnings.empty());
    AETHER_CHECK(loaded.legs == 2);
}

AETHER_TEST(Archive_MigrationHookRuns) {
    RegisterMigration<save::Creature>([](u16 from_version, Json& data) {
        if (from_version < 2) {
            // v1 stored an integer "hp"; v2 stores a float "health".
            data["health"] = static_cast<f64>(data["hp"].get<i64>());
            data.erase("hp");
        }
    });

    save::Creature loaded;
    LoadReport report;
    AETHER_CHECK(LoadJsonText(loaded, R"({"$v": 1, "hp": 25, "name": "slime"})", &report));
    AETHER_CHECK(report.warnings.empty());
    AETHER_CHECK(loaded.health == 25.0f && loaded.name == "slime");

    // Also through the binary archive (same document model).
    Json old_data = {{"$v", 1}, {"hp", 7}, {"name", "bat"}};
    save::Creature from_binary;
    AETHER_CHECK(LoadBinary(from_binary, Json::to_msgpack(old_data), &report));
    AETHER_CHECK(from_binary.health == 7.0f);

    // Current-version data isn't migrated.
    save::Creature current;
    AETHER_CHECK(LoadJsonText(current, R"({"$v": 2, "health": 3.5})"));
    AETHER_CHECK(current.health == 3.5f);
}

AETHER_TEST(Archive_NewerVersionLoadsWithWarning) {
    save::Grown loaded;
    LoadReport report;
    AETHER_CHECK(LoadJsonText(loaded, R"({"$v": 9, "legs": 8})", &report));
    AETHER_CHECK(loaded.legs == 8);
    AETHER_CHECK(report.warnings.size() == 1);
}

AETHER_TEST(Archive_BadValuesAreSkippedWithWarnings) {
    save::Everything loaded;
    loaded.medium = 11;
    loaded.byte = 12;
    loaded.text = "keep";
    loaded.mood = save::Mood::Angry;
    loaded.flag = true;

    LoadReport report;
    const char* text = R"({
        "$v": 3,
        "medium": "not a number",
        "byte": 300,
        "uint": -1,
        "tiny": 1000,
        "text": 5,
        "mood": "Furious",
        "flag": 1,
        "position": [9, 8],
        "stats": [1, 2, 3],
        "dbl": 2.5
    })";
    AETHER_CHECK(LoadJsonText(loaded, text, &report)); // structurally fine, so it loads
    AETHER_CHECK(loaded.medium == 11);
    AETHER_CHECK(loaded.byte == 12);
    AETHER_CHECK(loaded.uint == 0);
    AETHER_CHECK(loaded.tiny == 0);
    AETHER_CHECK(loaded.text == "keep");
    AETHER_CHECK(loaded.mood == save::Mood::Angry);
    AETHER_CHECK(loaded.flag == true);
    AETHER_CHECK(loaded.position.x == 9.0f && loaded.position.y == 8.0f && loaded.position.z == 0.0f);
    AETHER_CHECK(loaded.dbl == 2.5); // good values next to bad ones still load
    // medium, byte, uint, tiny, text, mood, flag, position (short array), stats (array for an object)
    AETHER_CHECK(report.warnings.size() == 9);
    bool mentions_path = false;
    for (const std::string& warning : report.warnings) {
        mentions_path = mentions_path || warning.find("Everything.mood") != std::string::npos;
    }
    AETHER_CHECK(mentions_path);

    // Enums also accept their numeric value.
    AETHER_CHECK(LoadJsonText(loaded, R"({"mood": 0})"));
    AETHER_CHECK(loaded.mood == save::Mood::Calm);
}

AETHER_TEST(Archive_UnparsableOrWrongShapeFails) {
    save::Grown loaded;
    LoadReport report;
    AETHER_CHECK(!LoadJsonText(loaded, "{ not json", &report));
    AETHER_CHECK(!LoadJsonText(loaded, "[1, 2, 3]", &report)); // array for a non-array struct
    AETHER_CHECK(!LoadJsonText(loaded, "42", &report));
    const u8 garbage[] = {0xC1, 0xFF, 0x00};
    AETHER_CHECK(!LoadBinary(loaded, garbage, &report));
    AETHER_CHECK(loaded.legs == 4 && loaded.name == "default"); // untouched

    // Scalars round-trip at the top level too.
    f32 value = 0.0f;
    AETHER_CHECK(LoadJsonText(value, SaveJsonText(1.25f)) && value == 1.25f);
    AETHER_CHECK(!LoadJsonText(value, "\"text\"", &report));
}
