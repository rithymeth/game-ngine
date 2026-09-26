#include "aether/reflection/reflection.h"
#include "test_framework.h"

#include <string>

using namespace aether;
using namespace aether::reflect;

namespace game {

enum class DamageKind : i32 { Physical, Fire };

struct Fighter {
    f32 health = 100.0f;
    std::string title = "Squire";
    DamageKind resistant_to = DamageKind::Fire;

    void ApplyDamage(f32 amount, DamageKind kind) {
        if (kind != resistant_to) {
            health -= amount;
        }
    }
    bool IsAlive() const { return health > 0.0f; }
    std::string Describe(const std::string& prefix, i32 level) const {
        return prefix + " " + title + " L" + std::to_string(level);
    }
    void Heal(f32& inout_amount) { // writes back how much was actually used
        f32 used = inout_amount < 100.0f - health ? inout_amount : 100.0f - health;
        health += used;
        inout_amount = used;
    }
    static i32 MaxLevel() { return 60; }
    static Vec3 Midpoint(const Vec3& a, const Vec3& b) {
        return Vec3((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f, (a.z + b.z) * 0.5f);
    }
};

// Counts live instances so tests can prove Any constructs and destroys
// exactly once per value, both inline and on the heap.
struct Counted {
    static inline i32 alive = 0;
    i32 value = 0;
    Counted() { ++alive; }
    Counted(const Counted& other) : value(other.value) { ++alive; }
    Counted(Counted&& other) noexcept : value(other.value) { ++alive; }
    Counted& operator=(const Counted&) = default;
    ~Counted() { --alive; }
};

struct BigCounted {
    Counted counted;
    f64 padding[8] = {}; // 64+ bytes: forces heap storage in Any
};

} // namespace game

AETHER_ENUM(game::DamageKind, 1, AETHER_ENUM_VALUE(Physical), AETHER_ENUM_VALUE(Fire))

AETHER_REFLECT(game::Fighter, 1,
    AETHER_FIELD(health, Field_EditAnywhere),
    AETHER_METHOD(ApplyDamage, Fn_BlueprintCallable, {"amount", "kind"}),
    AETHER_FIELD(title),
    AETHER_METHOD(IsAlive, Fn_BlueprintCallable | Fn_Pure),
    AETHER_METHOD(Describe, Fn_None, {"prefix", "level"}),
    AETHER_METHOD(Heal, Fn_None, {"inout_amount"}),
    AETHER_METHOD(MaxLevel),
    AETHER_METHOD(Midpoint, Fn_BlueprintCallable | Fn_Pure, {"a", "b"}),
    AETHER_FIELD(resistant_to)
)

AETHER_REFLECT(game::Counted, 1, AETHER_FIELD(value))
AETHER_REFLECT(game::BigCounted, 1, AETHER_FIELD(counted))

AETHER_TEST(Any_HoldsValuesOfEveryBuiltinKind) {
    Any empty;
    AETHER_CHECK(!empty.HasValue() && empty.Type() == nullptr);

    Any b = true;
    Any i = i32{-7};
    Any f = 2.5f;
    Any s = std::string("hello");
    Any v = Vec3(1, 2, 3);
    Any e = game::DamageKind::Fire;

    AETHER_CHECK(b.Get<bool>() == true);
    AETHER_CHECK(i.Get<i32>() == -7);
    AETHER_CHECK(f.Get<f32>() == 2.5f);
    AETHER_CHECK(s.Get<std::string>() == "hello");
    AETHER_CHECK(v.Get<Vec3>().z == 3.0f);
    AETHER_CHECK(e.Get<game::DamageKind>() == game::DamageKind::Fire);
    AETHER_CHECK(e.Type() == &Reflect<game::DamageKind>());

    // Type mismatches are refused rather than reinterpreting bytes.
    AETHER_CHECK(f.TryGet<i32>() == nullptr);
    AETHER_CHECK(f.TryGet<f64>() == nullptr);
    AETHER_CHECK(empty.TryGet<f32>() == nullptr);

    Any d = Any::DefaultOf(Reflect<game::Fighter>());
    AETHER_CHECK(d.Get<game::Fighter>().title == "Squire");
}

AETHER_TEST(Any_InlineAndHeapStorageCopyMoveAndDestroyExactlyOnce) {
    AETHER_CHECK(game::Counted::alive == 0);
    {
        Any small = game::Counted{};
        AETHER_CHECK(!small.IsHeapAllocated());
        small.Get<game::Counted>().value = 5;

        Any big = game::BigCounted{};
        AETHER_CHECK(big.IsHeapAllocated());
        big.Get<game::BigCounted>().counted.value = 9;
        AETHER_CHECK(game::Counted::alive == 2);

        Any small_copy = small;
        Any big_copy = big;
        AETHER_CHECK(game::Counted::alive == 4);
        small_copy.Get<game::Counted>().value = 6;
        AETHER_CHECK(small.Get<game::Counted>().value == 5); // copies are independent
        AETHER_CHECK(big_copy.Get<game::BigCounted>().counted.value == 9);

        Any moved_small = std::move(small);
        Any moved_big = std::move(big);
        AETHER_CHECK(!small.HasValue() && !big.HasValue());
        AETHER_CHECK(moved_small.Get<game::Counted>().value == 5);
        AETHER_CHECK(moved_big.Get<game::BigCounted>().counted.value == 9);
        AETHER_CHECK(game::Counted::alive == 4);

        moved_small = moved_big; // assign across storage kinds
        AETHER_CHECK(moved_small.IsHeapAllocated());
        AETHER_CHECK(game::Counted::alive == 4);

        moved_small = Any(2.0f);
        AETHER_CHECK(game::Counted::alive == 3);
        Any& alias = moved_small;
        moved_small = alias; // self-assignment is a no-op
        AETHER_CHECK(moved_small.Get<f32>() == 2.0f);

        Mat4 m = Mat4::Identity();
        Any matrix = m;
        AETHER_CHECK(matrix.IsHeapAllocated());
        AETHER_CHECK(matrix.Get<Mat4>().cols[2].z == 1.0f);
    }
    AETHER_CHECK(game::Counted::alive == 0);
}

AETHER_TEST(Reflection_GetSetThroughAny) {
    const TypeInfo& info = Reflect<game::Fighter>();
    game::Fighter fighter;

    Any health = info.FindField("health")->Get(&fighter);
    AETHER_CHECK(health.Get<f32>() == 100.0f);

    AETHER_CHECK(info.FindField("health")->Set(&fighter, Any(12.0f)));
    AETHER_CHECK(fighter.health == 12.0f);
    AETHER_CHECK(info.FindField("title")->Set(&fighter, Any(std::string("Knight"))));
    AETHER_CHECK(fighter.title == "Knight");
    AETHER_CHECK(info.FindField("resistant_to")->Set(&fighter, Any(game::DamageKind::Physical)));
    AETHER_CHECK(fighter.resistant_to == game::DamageKind::Physical);

    // Wrong type (double instead of float) is refused and leaves the field alone.
    AETHER_CHECK(!info.FindField("health")->Set(&fighter, Any(5.0)));
    AETHER_CHECK(!info.FindField("health")->Set(&fighter, Any()));
    AETHER_CHECK(fighter.health == 12.0f);

    // Get returns a copy, not a view.
    Any title = info.FindField("title")->Get(&fighter);
    title.Get<std::string>() = "Changed";
    AETHER_CHECK(fighter.title == "Knight");
}

AETHER_TEST(Reflection_FunctionMetadataIsCaptured) {
    const TypeInfo& info = Reflect<game::Fighter>();
    AETHER_CHECK(info.fields.size() == 3);    // fields and functions interleaved in the macro
    AETHER_CHECK(info.functions.size() == 6);
    AETHER_CHECK(std::string(info.fields[2].name) == "resistant_to");

    const FunctionInfo* apply = info.FindFunction("ApplyDamage");
    AETHER_CHECK(apply != nullptr);
    AETHER_CHECK(apply->return_type == nullptr);
    AETHER_CHECK(apply->params.size() == 2);
    AETHER_CHECK(std::string(apply->params[0].name) == "amount");
    AETHER_CHECK(apply->params[0].type == &Reflect<f32>());
    AETHER_CHECK(apply->params[1].type == &Reflect<game::DamageKind>());
    AETHER_CHECK(apply->HasFlag(Fn_BlueprintCallable) && !apply->HasFlag(Fn_Const));

    const FunctionInfo* alive = info.FindFunction("IsAlive");
    AETHER_CHECK(alive->return_type == &Reflect<bool>());
    AETHER_CHECK(alive->HasFlag(Fn_Const) && alive->HasFlag(Fn_Pure));

    const FunctionInfo* describe = info.FindFunction("Describe");
    AETHER_CHECK(describe->params[0].type == &Reflect<std::string>()); // const std::string& decays
    AETHER_CHECK(describe->return_type == &Reflect<std::string>());

    const FunctionInfo* max_level = info.FindFunction("MaxLevel");
    AETHER_CHECK(max_level->HasFlag(Fn_Static) && max_level->params.empty());
    AETHER_CHECK(std::string(max_level->params.empty() ? "" : max_level->params[0].name).empty());

    AETHER_CHECK(info.FindFunction("NoSuchFunction") == nullptr);
}

AETHER_TEST(Reflection_InvokeCallsRealFunctions) {
    const TypeInfo& info = Reflect<game::Fighter>();
    game::Fighter fighter;

    Any args[] = {Any(30.0f), Any(game::DamageKind::Physical)};
    AETHER_CHECK(info.FindFunction("ApplyDamage")->Invoke(&fighter, args));
    AETHER_CHECK(fighter.health == 70.0f);

    Any resisted[] = {Any(30.0f), Any(game::DamageKind::Fire)};
    AETHER_CHECK(info.FindFunction("ApplyDamage")->Invoke(&fighter, resisted));
    AETHER_CHECK(fighter.health == 70.0f);

    Any alive;
    AETHER_CHECK(info.FindFunction("IsAlive")->Invoke(&fighter, {}, &alive));
    AETHER_CHECK(alive.Get<bool>() == true);

    Any describe_args[] = {Any(std::string("Sir")), Any(i32{12})};
    Any description;
    AETHER_CHECK(info.FindFunction("Describe")->Invoke(&fighter, describe_args, &description));
    AETHER_CHECK(description.Get<std::string>() == "Sir Squire L12");

    // Non-const reference parameters write back into the argument.
    Any heal_args[] = {Any(50.0f)};
    AETHER_CHECK(info.FindFunction("Heal")->Invoke(&fighter, heal_args));
    AETHER_CHECK(fighter.health == 100.0f);
    AETHER_CHECK(heal_args[0].Get<f32>() == 30.0f);

    // Static functions need no instance.
    Any level;
    AETHER_CHECK(info.FindFunction("MaxLevel")->Invoke(nullptr, {}, &level));
    AETHER_CHECK(level.Get<i32>() == 60);
    Any points[] = {Any(Vec3(0, 0, 0)), Any(Vec3(2, 4, 6))};
    Any mid;
    AETHER_CHECK(info.FindFunction("Midpoint")->Invoke(nullptr, points, &mid));
    AETHER_CHECK(mid.Get<Vec3>().y == 2.0f);

    // A discarded result (ret == nullptr) is fine.
    AETHER_CHECK(info.FindFunction("IsAlive")->Invoke(&fighter, {}));
}

AETHER_TEST(Reflection_InvokeRejectsBadCalls) {
    const TypeInfo& info = Reflect<game::Fighter>();
    const FunctionInfo* apply = info.FindFunction("ApplyDamage");
    game::Fighter fighter;

    Any too_few[] = {Any(30.0f)};
    AETHER_CHECK(!apply->Invoke(&fighter, too_few));

    Any wrong_type[] = {Any(30.0), Any(game::DamageKind::Physical)}; // f64, not f32
    AETHER_CHECK(!apply->Invoke(&fighter, wrong_type));

    Any empty_arg[] = {Any(), Any(game::DamageKind::Physical)};
    AETHER_CHECK(!apply->Invoke(&fighter, empty_arg));

    Any ok[] = {Any(30.0f), Any(game::DamageKind::Physical)};
    AETHER_CHECK(!apply->Invoke(nullptr, ok)); // member function without an instance

    AETHER_CHECK(fighter.health == 100.0f); // nothing was called
}
