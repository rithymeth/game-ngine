#include "aether/reflection/reflection.h"
#include "test_framework.h"

#include <cstring>
#include <string>

using namespace aether;
using namespace aether::reflect;

namespace game {

enum class Team : u8 { Red = 1, Blue = 2, Spectator = 7 };

struct Health {
    f32 current = 100.0f;
    f32 max = 100.0f;
    bool invulnerable = false;
    Team team = Team::Red;
    Vec3 respawn_point{1.0f, 2.0f, 3.0f};
};

struct Nameplate {
    std::string text = "Player";
    i32 font_size = 16;
};

// Only ever looked up through TypeRegistry::Find in these tests, never via
// Reflect<> first, to prove static registration works on its own.
struct OnlyFoundByName {
    u16 value = 7;
};

struct EmptyTag {};

} // namespace game

AETHER_ENUM(game::Team, 1, AETHER_ENUM_VALUE(Red), AETHER_ENUM_VALUE(Blue), AETHER_ENUM_VALUE(Spectator))

AETHER_REFLECT(game::Health, 2,
    AETHER_FIELD(current, Field_EditAnywhere | Field_Replicated,
                 {.tooltip = "Current HP", .category = "Stats", .range_min = 0, .range_max = 1000}),
    AETHER_FIELD(max, Field_EditAnywhere, {.range_min = 1, .range_max = 1000}),
    AETHER_FIELD(invulnerable),
    AETHER_FIELD(team, Field_EditAnywhere),
    AETHER_FIELD(respawn_point, Field_EditAnywhere, {.units = "m"})
)

AETHER_REFLECT(game::Nameplate, 1, AETHER_FIELD(text), AETHER_FIELD(font_size))

AETHER_REFLECT(game::OnlyFoundByName, 1, AETHER_FIELD(value))

AETHER_REFLECT(game::EmptyTag, 1)

AETHER_TEST(Reflection_FieldsEnumerateInDeclarationOrder) {
    const TypeInfo& info = Reflect<game::Health>();
    AETHER_CHECK(std::string(info.name) == "Health"); // namespace stripped
    AETHER_CHECK(info.kind == TypeKind::Struct);
    AETHER_CHECK(info.version == 2);
    AETHER_CHECK(info.size == sizeof(game::Health));
    AETHER_CHECK(info.alignment == alignof(game::Health));
    AETHER_CHECK(info.fields.size() == 5);

    const char* expected_names[] = {"current", "max", "invulnerable", "team", "respawn_point"};
    const usize expected_offsets[] = {offsetof(game::Health, current), offsetof(game::Health, max),
                                      offsetof(game::Health, invulnerable), offsetof(game::Health, team),
                                      offsetof(game::Health, respawn_point)};
    const TypeInfo* expected_types[] = {&Reflect<f32>(), &Reflect<f32>(), &Reflect<bool>(),
                                        &Reflect<game::Team>(), &Reflect<Vec3>()};
    for (usize i = 0; i < info.fields.size(); ++i) {
        AETHER_CHECK(std::string(info.fields[i].name) == expected_names[i]);
        AETHER_CHECK(info.fields[i].offset == expected_offsets[i]);
        AETHER_CHECK(info.fields[i].type == expected_types[i]);
    }
}

AETHER_TEST(Reflection_FieldPointersReadAndWriteMembers) {
    const TypeInfo& info = Reflect<game::Health>();
    game::Health health;

    const FieldInfo* current = info.FindField("current");
    AETHER_CHECK(current != nullptr);
    AETHER_CHECK(*current->As<f32>(&health) == 100.0f);
    *current->As<f32>(&health) = 42.5f;
    AETHER_CHECK(health.current == 42.5f);

    // Wrong-type access is refused rather than reinterpreting memory.
    AETHER_CHECK(current->As<i32>(&health) == nullptr);

    const FieldInfo* respawn = info.FindField("respawn_point");
    Vec3* point = respawn->As<Vec3>(&health);
    AETHER_CHECK(point != nullptr && point->y == 2.0f);

    // Nested struct fields are reachable through the field's own TypeInfo.
    const FieldInfo* z = respawn->type->FindField("z");
    AETHER_CHECK(z != nullptr);
    *z->As<f32>(point) = 9.0f;
    AETHER_CHECK(health.respawn_point.z == 9.0f);

    const game::Health& const_health = health;
    AETHER_CHECK(*info.FindField("max")->As<f32>(&const_health) == 100.0f);
    AETHER_CHECK(info.FindField("does_not_exist") == nullptr);
}

AETHER_TEST(Reflection_FlagsAndMetaArePreserved) {
    const TypeInfo& info = Reflect<game::Health>();
    const FieldInfo* current = info.FindField("current");
    AETHER_CHECK(current->HasFlag(Field_EditAnywhere));
    AETHER_CHECK(current->HasFlag(Field_Replicated));
    AETHER_CHECK(!current->HasFlag(Field_Transient));
    AETHER_CHECK(std::string(current->meta.tooltip) == "Current HP");
    AETHER_CHECK(std::string(current->meta.category) == "Stats");
    AETHER_CHECK(current->meta.HasRange() && current->meta.range_max == 1000.0);

    const FieldInfo* invulnerable = info.FindField("invulnerable");
    AETHER_CHECK(invulnerable->flags == Field_None);
    AETHER_CHECK(invulnerable->meta.tooltip == nullptr);
    AETHER_CHECK(!invulnerable->meta.HasRange());

    AETHER_CHECK(std::string(info.FindField("respawn_point")->meta.units) == "m");
}

AETHER_TEST(Reflection_RegistryFindsTypesRegisteredStatically) {
    // Never called Reflect<game::OnlyFoundByName>() before this point.
    const TypeInfo* by_name = TypeRegistry::Find("OnlyFoundByName");
    AETHER_CHECK(by_name != nullptr);
    AETHER_CHECK(by_name != nullptr && by_name->FindField("value") != nullptr);
    AETHER_CHECK(by_name == &Reflect<game::OnlyFoundByName>());

    AETHER_CHECK(TypeRegistry::Find(HashTypeName("Health")) == &Reflect<game::Health>());
    AETHER_CHECK(TypeRegistry::Find("f32") == &Reflect<f32>());
    AETHER_CHECK(TypeRegistry::Find("NoSuchType") == nullptr);

    bool saw_vec3 = false;
    for (const TypeInfo* type : TypeRegistry::AllTypes()) {
        saw_vec3 = saw_vec3 || type == &Reflect<Vec3>();
    }
    AETHER_CHECK(saw_vec3);
}

AETHER_TEST(Reflection_TypeIdIsStableHashOfDeclaredName) {
    // FNV-1a 64 of "Health"; pinned so an accidental change to the hash (which
    // would orphan every saved file) fails loudly.
    AETHER_CHECK(HashTypeName("") == 0xcbf29ce484222325ull);
    AETHER_CHECK(HashTypeName("a") == 0xaf63dc4c8601ec8cull);
    AETHER_CHECK(TypeIdOf<game::Health>() == HashTypeName("Health"));
    AETHER_CHECK(TypeIdOf<Vec3>() == HashTypeName("Vec3"));
    AETHER_CHECK(TypeIdOf<game::Health>() != TypeIdOf<game::Nameplate>());
    AETHER_CHECK(std::string(StripNamespace("a::b::Transform")) == "Transform");
    AETHER_CHECK(std::string(StripNamespace("Transform")) == "Transform");
}

AETHER_TEST(Reflection_BuiltinTypesDescribeThemselves) {
    AETHER_CHECK(Reflect<bool>().kind == TypeKind::Bool);
    AETHER_CHECK(Reflect<i32>().kind == TypeKind::Int && Reflect<i32>().size == 4);
    AETHER_CHECK(Reflect<u64>().kind == TypeKind::UInt && Reflect<u64>().size == 8);
    AETHER_CHECK(Reflect<f64>().kind == TypeKind::Float && Reflect<f64>().size == 8);
    AETHER_CHECK(Reflect<std::string>().kind == TypeKind::String);
    AETHER_CHECK(std::string(Reflect<std::string>().name) == "string");
    // const-qualified lookups resolve to the same TypeInfo.
    AETHER_CHECK(&Reflect<const f32>() == &Reflect<f32>());

    const TypeInfo& quat = Reflect<Quaternion>();
    AETHER_CHECK(quat.fields.size() == 4);
    Quaternion q(0.1f, 0.2f, 0.3f, 0.9f);
    AETHER_CHECK(*quat.FindField("w")->As<f32>(&q) == 0.9f);

    const TypeInfo& mat = Reflect<Mat4>();
    AETHER_CHECK(mat.fields.size() == 4);
    Mat4 m = Mat4::Identity();
    const Vec4* col3 = mat.FindField("col3")->As<Vec4>(&m);
    AETHER_CHECK(col3 != nullptr && col3->w == 1.0f && col3->x == 0.0f);
    const Vec4* col1 = mat.FindField("col1")->As<Vec4>(&m);
    AETHER_CHECK(col1 != nullptr && col1->y == 1.0f);

    AETHER_CHECK(Reflect<game::EmptyTag>().fields.empty());
}

AETHER_TEST(Reflection_EnumValuesRoundTripByName) {
    const TypeInfo& team = Reflect<game::Team>();
    AETHER_CHECK(team.kind == TypeKind::Enum);
    AETHER_CHECK(team.underlying == &Reflect<u8>());
    AETHER_CHECK(team.enum_values.size() == 3);
    AETHER_CHECK(std::string(team.EnumName(7)) == "Spectator");
    AETHER_CHECK(team.EnumName(3) == nullptr);

    i64 value = 0;
    AETHER_CHECK(team.EnumValueOf("Blue", value) && value == 2);
    AETHER_CHECK(!team.EnumValueOf("Green", value));
}

AETHER_TEST(Reflection_LifecycleThunksManageNonTrivialTypes) {
    const TypeInfo& info = Reflect<game::Nameplate>();
    alignas(game::Nameplate) unsigned char a[sizeof(game::Nameplate)];
    alignas(game::Nameplate) unsigned char b[sizeof(game::Nameplate)];
    alignas(game::Nameplate) unsigned char c[sizeof(game::Nameplate)];

    info.construct(a);
    AETHER_CHECK(reinterpret_cast<game::Nameplate*>(a)->text == "Player");

    *info.FindField("text")->As<std::string>(a) = "A name long enough to need a heap allocation";
    info.copy_construct(b, a);
    AETHER_CHECK(reinterpret_cast<game::Nameplate*>(b)->text == "A name long enough to need a heap allocation");

    info.move_construct(c, b);
    AETHER_CHECK(reinterpret_cast<game::Nameplate*>(c)->text == "A name long enough to need a heap allocation");

    info.destruct(a);
    info.destruct(b);
    info.destruct(c);
}

namespace game {
struct Label {
    char text[8] = {};
    i32 priority = 0;
};
} // namespace game

AETHER_REFLECT(game::Label, 1, AETHER_FIELD(text), AETHER_FIELD(priority))

#include "aether/reflection/serialize.h"

AETHER_TEST(Reflection_FixedStringArrays) {
    const TypeInfo& info = Reflect<game::Label>();
    const FieldInfo* text = info.FindField("text");
    AETHER_CHECK(text->type->kind == TypeKind::FixedString);
    AETHER_CHECK(std::string(text->type->name) == "char[8]");
    AETHER_CHECK(text->type->size == 8);
    AETHER_CHECK(&Reflect<char[8]>() == text->type);
    AETHER_CHECK(&Reflect<char[16]>() != text->type);

    game::Label label;
    std::memcpy(label.text, "hello", 6);
    Any copy = text->Get(&label); // Any::CopyOf, since arrays can't be passed by value
    AETHER_CHECK(copy.Type() == text->type);
    AETHER_CHECK(std::string(static_cast<const char*>(copy.Data())) == "hello");

    Json json = ToJson(label);
    AETHER_CHECK(json["text"] == "hello");

    game::Label loaded;
    LoadReport report;
    AETHER_CHECK(FromJson(loaded, json, &report) && report.warnings.empty());
    AETHER_CHECK(std::string(loaded.text) == "hello");

    // Too long: truncated to 7 characters plus the terminator, with a warning.
    AETHER_CHECK(FromJson(loaded, Json{{"text", "much too long"}}, &report));
    AETHER_CHECK(std::string(loaded.text) == "much to");
    AETHER_CHECK(report.warnings.size() == 1);
}
