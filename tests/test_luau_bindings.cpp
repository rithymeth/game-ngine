#include "aether/scene/components.h"
#include "aether/scene/entity_guid.h"
#include "aether/script/luau_host.h"
#include "test_framework.h"

#include <algorithm>

using namespace aether;
using namespace aether::script;

namespace luau_test {
enum class Team { Red, Blue };
struct Health {
    f32 current = 100.0f;
    i32 max = 100;
    std::string owner = "nobody";
    Team team = Team::Red;
    bool alive = true;
    Vec3 respawn{0.0f, 0.0f, 0.0f};
    std::vector<i32> hits;
    char label[8] = "hp";
    u8 armor = 0;

    f32 Heal(f32 amount) {
        current = std::min(static_cast<f32>(max), current + amount);
        return current;
    }
    bool IsFull() const { return current >= static_cast<f32>(max); }
};
struct Hidden { // not reflected
    i32 x = 0;
};
} // namespace luau_test

AETHER_ENUM(luau_test::Team, 1, AETHER_ENUM_VALUE(Red), AETHER_ENUM_VALUE(Blue))
AETHER_REFLECT(luau_test::Health, 1,
    AETHER_FIELD(current), AETHER_FIELD(max), AETHER_FIELD(owner), AETHER_FIELD(team), AETHER_FIELD(alive),
    AETHER_FIELD(respawn), AETHER_FIELD(hits), AETHER_FIELD(label), AETHER_FIELD(armor),
    AETHER_METHOD(Heal, Fn_BlueprintCallable, {"amount"}),
    AETHER_METHOD(IsFull, Fn_BlueprintCallable | Fn_Pure)
)

namespace {

using luau_test::Health;

f64 Number(const ScriptResult& r, usize i = 0) {
    return r.values.size() > i && std::holds_alternative<f64>(r.values[i]) ? std::get<f64>(r.values[i]) : -12345.0;
}
std::string Text(const ScriptResult& r, usize i = 0) {
    return r.values.size() > i && std::holds_alternative<std::string>(r.values[i]) ? std::get<std::string>(r.values[i]) : "";
}

struct Scene {
    World world;
    GuidIndex guids;
    LuauHost host;
    Entity player;

    Scene() {
        (void)GetComponentId<Health>();
        (void)GetComponentId<luau_test::Hidden>();
        (void)GetComponentId<Transform>();
        player = world.CreateEntity(IdComponent{NewEntityGuid()}, Health{}, luau_test::Hidden{});
        guids.Add(world.GetComponent<IdComponent>(player)->guid, player);
        host.BindWorld(&world, &guids);
        host.SetGlobal("player", EntityRef{player});
    }
};

} // namespace

AETHER_TEST(LuauBindings_FieldsReadAndWrite) {
    Scene s;
    ScriptResult r = s.host.Run(R"(
        local h = player:Get("Health")
        return h.current, h.max, h.owner, h.team, h.alive, h.label, h.armor
    )");
    AETHER_CHECK(r.ok && Number(r, 0) == 100.0 && Number(r, 1) == 100.0 && Text(r, 2) == "nobody");
    AETHER_CHECK(Text(r, 3) == "Red" && std::get<bool>(r.values[4]) && Text(r, 5) == "hp" && Number(r, 6) == 0.0);

    r = s.host.Run(R"(
        local h = player:Get("Health")
        h.current -= 25.5
        h.max = 150
        h.owner = "Ada"
        h.team = "Blue"
        h.alive = false
        h.label = "a long label"   -- truncated to fit char[8]
        h.armor = 3
        h.respawn = vector.create(1, 2, 3) * 2 + vector.create(0, 0, 1)  -- native vector math
        h.hits = {10, 20, 30}
        return h.respawn.z, #h.hits, h.hits[2]
    )");
    AETHER_CHECK(r.ok && Number(r, 0) == 7.0 && Number(r, 1) == 3.0 && Number(r, 2) == 20.0);
    const Health& health = *s.world.GetComponent<Health>(s.player);
    AETHER_CHECK(health.current == 74.5f && health.max == 150 && health.owner == "Ada" && health.team == luau_test::Team::Blue);
    AETHER_CHECK(!health.alive && std::string(health.label) == "a long " && health.armor == 3);
    AETHER_CHECK(health.respawn.x == 2.0f && health.respawn.z == 7.0f && health.hits.size() == 3);

    // Type mismatches are script errors naming the field; nothing is written.
    r = s.host.Run("player:Get('Health').max = 'lots'", "typo");
    AETHER_CHECK(!r.ok && r.error.find("Health.max expects a number, got string") != std::string::npos);
    AETHER_CHECK(!s.host.Run("player:Get('Health').max = 1.5").ok);   // not a whole number
    AETHER_CHECK(!s.host.Run("player:Get('Health').armor = -1").ok);  // unsigned
    AETHER_CHECK(!s.host.Run("player:Get('Health').team = 'Green'").ok);
    AETHER_CHECK(!s.host.Run("player:Get('Health').nope = 1").ok);
    r = s.host.Run("return player:Get('Health').nope");
    AETHER_CHECK(!r.ok && r.error.find("no field or function named 'nope'") != std::string::npos);
    AETHER_CHECK(health.max == 150);
}

AETHER_TEST(LuauBindings_MethodsEntitiesAndTheWorld) {
    Scene s;
    s.world.GetComponent<Health>(s.player)->current = 40.0f;
    ScriptResult r = s.host.Run(R"(
        local h = player:Get("Health")
        local healed = h:Heal(25)
        return healed, h:IsFull(), h:Heal(1000), h:IsFull()
    )");
    AETHER_CHECK(r.ok && Number(r, 0) == 65.0 && !std::get<bool>(r.values[1]) && Number(r, 2) == 100.0);
    AETHER_CHECK(std::get<bool>(r.values[3]));
    r = s.host.Run("return player:Get('Health'):Heal('x')");
    AETHER_CHECK(!r.ok && r.error.find("argument 'amount'") != std::string::npos);
    AETHER_CHECK(!s.host.Run("return player:Get('Health'):Heal()").ok); // wrong count

    // Spawning, finding, adding, removing.
    r = s.host.Run(R"(
        local e = world:Spawn()
        local t = e:Add("Transform")
        t.position = vector.create(4, 5, 6)
        local again = world:Find(e:Guid())
        assert(again == e)
        assert(e:Has("Transform") and not e:Has("Health"))
        assert(e:Get("Health") == nil)
        return #world:EntitiesWith("Health"), #world:EntitiesWith("Transform"), e
    )");
    AETHER_CHECK(r.ok && Number(r, 0) == 1.0 && Number(r, 1) == 1.0);
    const Entity spawned = std::get<EntityRef>(r.values[2]).entity; // entities come back to C++
    AETHER_CHECK(s.world.IsAlive(spawned) && s.world.GetComponent<Transform>(spawned)->position.y == 5.0f);
    // An unreflected component goes by its registered (compiler-derived) name:
    // scripts can test for it and remove it, but not read it.
    s.host.SetGlobal("hidden", std::string(GetComponentInfo(GetComponentId<luau_test::Hidden>()).name));
    r = s.host.Run("return player:Has(hidden), player:Remove(hidden), player:Remove(hidden)");
    AETHER_CHECK(r.ok && std::get<bool>(r.values[0]) && std::get<bool>(r.values[1]) && !std::get<bool>(r.values[2]));
    AETHER_CHECK(!s.world.HasComponent<luau_test::Hidden>(s.player));
    s.world.AddComponent<luau_test::Hidden>(s.player);
    r = s.host.Run("return player:Get(hidden)");
    AETHER_CHECK(!r.ok && r.error.find("isn't reflected") != std::string::npos);
    AETHER_CHECK(!s.host.Run("return player:Get('NotAComponent')").ok);
    AETHER_CHECK(!s.host.Run("player:Remove('IdComponent')").ok);
    AETHER_CHECK(Text(s.host.Run("return tostring(player:Get('Health'))")).find("Health") == 0);

    // C++ passes entities in as arguments too.
    AETHER_CHECK(s.host.Run("function HealthOf(e) return e:Get('Health').current end").ok);
    AETHER_CHECK(Number(s.host.Call("HealthOf", {EntityRef{s.player}})) == 100.0);
}

AETHER_TEST(LuauBindings_HandlesAreSafe) {
    Scene s;
    AETHER_CHECK(s.host.Run("saved = player:Get('Health'); saved_entity = player").ok);

    // Component removed: a clear error, not a read of freed memory.
    s.world.RemoveComponent<Health>(s.player);
    ScriptResult r = s.host.Run("return saved.current", "stale");
    AETHER_CHECK(!r.ok && r.error.find("no longer has a Health") != std::string::npos);
    s.world.AddComponent<Health>(s.player, Health{});
    AETHER_CHECK(s.host.Run("return saved.current").ok); // valid again

    // Entity destroyed (and its slot reused): errors, and IsValid says so.
    s.world.DestroyEntity(s.player);
    const Entity reused = s.world.CreateEntity(Health{});
    AETHER_CHECK(reused.index == s.player.index); // same slot, new generation
    r = s.host.Run("return saved.current");
    AETHER_CHECK(!r.ok && r.error.find("destroyed") != std::string::npos);
    r = s.host.Run("return saved_entity:IsValid()");
    AETHER_CHECK(r.ok && !std::get<bool>(r.values[0]));
    AETHER_CHECK(!s.host.Run("return saved_entity:Get('Health')").ok);
    AETHER_CHECK(!s.host.Run("world:Destroy(saved_entity)").ok);

    // Re-binding the world invalidates every old handle.
    AETHER_CHECK(s.host.Run("fresh = world:Spawn(); fresh:Add('Health')").ok);
    World other;
    GuidIndex other_guids;
    s.host.BindWorld(&other, &other_guids);
    r = s.host.Run("return fresh:Get('Health')");
    AETHER_CHECK(!r.ok && r.error.find("no longer bound") != std::string::npos);
    s.host.BindWorld(nullptr, nullptr);
    r = s.host.Run("return world:Spawn()");
    AETHER_CHECK(!r.ok && r.error.find("no world is bound") != std::string::npos);
    s.host.SetGlobal("nobody", EntityRef{reused}); // no world: arrives as nil
    AETHER_CHECK(s.host.Run("assert(nobody == nil)").ok);
}
