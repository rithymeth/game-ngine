#include "test_framework.h"

#include "aether/ecs/world.h"
#include "aether/reflection/serialize.h"
#include "aether/save/save_system.h"
#include "aether/save/world_state.h"
#include "aether/scene/components.h"
#include "aether/scene/entity_guid.h"
#include "aether/scene/lifecycle.h"

#include <filesystem>

// Phase 28 step 2 (§28.3): capturing and restoring world state by EntityGuid.

using namespace aether;
using namespace aether::save;

#define CHECK(...) AETHER_CHECK((__VA_ARGS__))

namespace ws_test {
struct Door {
    bool open = false;
    f32 angle = 0.0f;
    i32 locks = 2;
};
struct Pickup {
    bool taken = false;
};
} // namespace ws_test

AETHER_REFLECT(ws_test::Door, 1, AETHER_FIELD(open, Field_EditAnywhere), AETHER_FIELD(angle, Field_EditAnywhere), AETHER_FIELD(locks, Field_EditAnywhere))
AETHER_REFLECT(ws_test::Pickup, 1, AETHER_FIELD(taken, Field_EditAnywhere))

namespace {

struct Level {
    World world;
    GuidIndex guids;
    Entity door, coin, enemy, scenery;
    EntityGuid door_guid, coin_guid, enemy_guid;

    // Builds the level as a scene load would: the same GUIDs each time, new entity handles.
    explicit Level(bool with_enemy = true) {
        (void)GetComponentId<Transform>();
        (void)GetComponentId<ws_test::Door>();
        (void)GetComponentId<ws_test::Pickup>();
        (void)GetComponentId<SaveableEntity>();
        door_guid = EntityGuid{0xD00A, 1};
        coin_guid = EntityGuid{0xC01A, 2};
        enemy_guid = EntityGuid{0xE4E4, 3};
        scenery = world.CreateEntity(Transform{Vec3(), Quaternion::Identity()}); // not saveable
        door = Make(door_guid, "door_a", {"Door.open", "Door.angle"});
        world.AddComponent(door, ws_test::Door{});
        coin = Make(coin_guid, "coin", {"Pickup"});
        world.AddComponent(coin, ws_test::Pickup{});
        if (with_enemy) enemy = Make(enemy_guid, "grunt", {"Transform.position"});
        guids.Rebuild(world);
    }
    Entity Make(const EntityGuid& guid, const char* tag, std::vector<std::string> fields) {
        const Entity e = world.CreateEntity(Transform{Vec3(1, 2, 3), Quaternion::Identity()});
        world.AddComponent(e, IdComponent{guid});
        SaveableEntity s;
        s.tag = tag;
        s.fields = std::move(fields);
        world.AddComponent(e, s);
        return e;
    }
    ws_test::Door* Door() { return world.GetComponent<ws_test::Door>(door); }
};

} // namespace

AETHER_TEST(WorldState_RoundTripsTheListedFields) {
    Level a;
    a.Door()->open = true;
    a.Door()->angle = 1.5f;
    a.Door()->locks = 0; // not listed: not saved
    a.world.GetComponent<ws_test::Pickup>(a.coin)->taken = true;
    a.world.GetComponent<Transform>(a.enemy)->position = Vec3(9, 8, 7);
    CaptureReport cr;
    const WorldSnapshot snapshot = CaptureWorld(a.world, a.guids, nullptr, &cr);
    CHECK(cr.entities == 3 && cr.warnings.empty() && snapshot.entities.size() == 3 && snapshot.destroyed.empty());

    // A fresh scene load: same GUIDs, new entities, default state.
    Level b;
    RestoreReport rr;
    CHECK(RestoreWorld(b.world, b.guids, snapshot, &rr));
    CHECK(rr.applied == 3 && rr.missing.empty() && rr.destroyed == 0);
    CHECK(b.Door()->open && b.Door()->angle == 1.5f);
    CHECK(b.Door()->locks == 2); // the field that wasn't listed keeps the scene's value
    CHECK(b.world.GetComponent<ws_test::Pickup>(b.coin)->taken);
    const Transform* t = b.world.GetComponent<Transform>(b.enemy);
    CHECK(t->position.x == 9.0f && t->position.y == 8.0f && t->position.z == 7.0f);
    CHECK(b.world.GetComponent<Transform>(b.scenery)->position.x == 0.0f); // not saveable: untouched
}

AETHER_TEST(WorldState_ListedFieldsOnlyAndWholeComponents) {
    Level a;
    a.Door()->open = true;
    a.Door()->locks = 7;
    const WorldSnapshot snapshot = CaptureWorld(a.world, a.guids);
    // The door entry has just open and angle; the coin's whole component is there.
    for (const SavedEntity& e : snapshot.entities) {
        if (e.tag == "door_a") {
            CHECK(e.components.size() == 1 && e.components[0].name == "Door");
            const reflect::Json j = reflect::Json::parse(e.components[0].data);
            CHECK(j.contains("open") && j.contains("angle") && !j.contains("locks") && !j.contains("$v"));
        }
        if (e.tag == "coin") {
            const reflect::Json j = reflect::Json::parse(e.components[0].data);
            CHECK(j.contains("taken") && j.contains("$v")); // a whole component keeps its version
        }
    }
    // The same state captures to the same snapshot, whatever order entities were made in.
    Level b;
    b.Door()->open = true;
    b.Door()->locks = 7;
    const WorldSnapshot again = CaptureWorld(b.world, b.guids);
    CHECK(reflect::SaveJsonText(snapshot) == reflect::SaveJsonText(again));
}

AETHER_TEST(WorldState_DestroyedEntitiesStayDestroyed) {
    Level a;
    WorldTracker tracker;
    tracker.Begin(a.world, a.guids);
    CHECK(tracker.Tracking() && tracker.Baseline().size() == 3);
    a.world.DestroyEntity(a.enemy); // killed, with no hook or callback
    const WorldSnapshot snapshot = CaptureWorld(a.world, a.guids, &tracker);
    CHECK(snapshot.entities.size() == 2 && snapshot.destroyed.size() == 1 && snapshot.destroyed[0] == a.enemy_guid);

    Level b;
    tracker.Begin(b.world, b.guids);
    RestoreReport rr;
    CHECK(RestoreWorld(b.world, b.guids, snapshot, &rr) && rr.destroyed == 1);
    CHECK(!b.world.IsAlive(b.enemy) && b.guids.Find(b.world, b.enemy_guid).IsNull());
    CHECK(b.world.IsAlive(b.door) && b.world.IsAlive(b.coin));
    // A tracker begun after the restore no longer has the destroyed entity in its baseline, so it isn't listed again...
    tracker.Begin(b.world, b.guids);
    CHECK(CaptureWorld(b.world, b.guids, &tracker).destroyed.empty());
    // ...which is why Begin belongs on the freshly loaded scene, before the restore: then saving again keeps it dead.
    WorldTracker from_load;
    Level c;
    from_load.Begin(c.world, c.guids);
    CHECK(RestoreWorld(c.world, c.guids, snapshot, nullptr));
    const WorldSnapshot kept = CaptureWorld(c.world, c.guids, &from_load);
    CHECK(kept.destroyed.size() == 1 && kept.destroyed[0] == c.enemy_guid);
    // Restoring twice is harmless.
    RestoreReport again;
    CHECK(RestoreWorld(c.world, c.guids, snapshot, &again) && again.destroyed == 0);
}

AETHER_TEST(WorldState_DestroysThroughLifecycleWhenGiven) {
    Level a;
    WorldTracker tracker;
    tracker.Begin(a.world, a.guids);
    a.world.DestroyEntity(a.coin);
    const WorldSnapshot snapshot = CaptureWorld(a.world, a.guids, &tracker);
    Level b;
    Lifecycle lifecycle(b.world, b.guids);
    int destroyed = 0;
    LifecycleCallbacks cb;
    cb.on_destroy = [&](Entity) { ++destroyed; };
    lifecycle.Register<SaveableEntity>(cb);
    lifecycle.BeginPlay();
    CHECK(RestoreWorld(b.world, b.guids, snapshot, nullptr, &lifecycle));
    CHECK(!b.world.IsAlive(b.coin) && destroyed == 1);
}

AETHER_TEST(WorldState_RuntimeSpawnedEntitiesAreSkipped) {
    Level a;
    WorldTracker tracker;
    tracker.Begin(a.world, a.guids);
    const Entity spawned = a.Make(EntityGuid{0xBEEF, 9}, "spawned", {"Transform"}); // made after the scene loaded
    (void)spawned;
    CaptureReport cr;
    const WorldSnapshot snapshot = CaptureWorld(a.world, a.guids, &tracker, &cr);
    CHECK(cr.entities == 3 && cr.skipped_runtime == 1 && snapshot.entities.size() == 3);
    // Without a tracker everything saveable is captured.
    CHECK(CaptureWorld(a.world, a.guids).entities.size() == 4);
}

AETHER_TEST(WorldState_ReportsWhatCantBeRestored) {
    Level a;
    a.Door()->open = true;
    WorldSnapshot snapshot = CaptureWorld(a.world, a.guids);
    // An entity the new scene doesn't have, a component and a field that no longer exist.
    SavedEntity ghost;
    ghost.guid = EntityGuid{0x6057, 4};
    ghost.tag = "ghost";
    ghost.components.push_back({"Pickup", "{\"taken\":true}"});
    snapshot.entities.push_back(ghost);
    snapshot.entities[0].components.push_back({"Gearbox", "{\"ratio\":3}"});
    SavedEntity door;
    door.guid = a.door_guid;
    door.tag = "door_a";
    door.components.push_back({"Door", "{\"hinge\":1,\"open\":true}"});
    snapshot.entities.push_back(door);
    Level b;
    RestoreReport rr;
    CHECK(!RestoreWorld(b.world, b.guids, snapshot, &rr));
    CHECK(rr.missing.size() == 1 && rr.missing[0] == ghost.guid);
    CHECK(rr.unknown_components == std::vector<std::string>{"Gearbox"});
    CHECK(rr.unknown_fields == std::vector<std::string>{"Door.hinge"});
    CHECK(b.Door()->open); // what could be restored was
    // A saved component on an entity that doesn't have it is reported, not added.
    SavedEntity lacking;
    lacking.guid = b.coin_guid;
    lacking.tag = "coin";
    lacking.components.push_back({"Door", "{\"open\":true}"});
    WorldSnapshot s2;
    s2.entities.push_back(lacking);
    CHECK(!RestoreWorld(b.world, b.guids, s2, &rr) && rr.warnings.size() == 1 && !b.world.HasComponent<ws_test::Door>(b.coin));
    // Unreadable data is a warning too.
    SavedEntity broken;
    broken.guid = b.door_guid;
    broken.components.push_back({"Door", "not json"});
    WorldSnapshot s3;
    s3.entities.push_back(broken);
    CHECK(!RestoreWorld(b.world, b.guids, s3, &rr) && rr.warnings.size() == 1);
}

AETHER_TEST(WorldState_CaptureWarnsAboutBadSpecsAndMissingGuids) {
    Level a;
    SaveableEntity* s = a.world.GetComponent<SaveableEntity>(a.door);
    s->fields = {"Door.open", "Door.nope", "Nope.x", "Pickup", "Transform", ""};
    const Entity no_id = a.world.CreateEntity(Transform{Vec3(), Quaternion::Identity()});
    SaveableEntity empty;
    empty.tag = "no id";
    a.world.AddComponent(no_id, empty);
    const Entity dup = a.Make(a.door_guid, "twin", {"Transform"}); // the same GUID as the door
    (void)dup;
    CaptureReport cr;
    const WorldSnapshot snapshot = CaptureWorld(a.world, a.guids, nullptr, &cr);
    CHECK(cr.warnings.size() >= 4);
    bool no_field = false, no_component = false, no_guid = false, shared = false, lacks = false;
    for (const std::string& w : cr.warnings) {
        no_field = no_field || w.find("no field 'nope'") != std::string::npos;
        no_component = no_component || w.find("no component named 'Nope'") != std::string::npos;
        no_guid = no_guid || w.find("has no GUID") != std::string::npos;
        shared = shared || w.find("share the GUID") != std::string::npos;
        lacks = lacks || w.find("has no 'Pickup'") != std::string::npos;
    }
    CHECK(no_field && no_component && no_guid && shared && lacks);
    for (const SavedEntity& e : snapshot.entities) {
        if (e.tag == "door_a") CHECK(e.components.size() == 2); // Door (open) and Transform: the bad specs are skipped
    }
}

AETHER_TEST(WorldState_IsAnOrdinarySaveGameField) {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "aether_world_state_tests";
    std::filesystem::remove_all(dir);
    Level a;
    a.Door()->open = true;
    WorldTracker tracker;
    tracker.Begin(a.world, a.guids);
    a.world.DestroyEntity(a.enemy);
    const WorldSnapshot snapshot = CaptureWorld(a.world, a.guids, &tracker);
    SaveSystem saves(dir);
    CHECK(saves.Save("world", snapshot).ok);
    WorldSnapshot loaded;
    CHECK(saves.Load("world", loaded).ok && loaded.entities.size() == snapshot.entities.size() && loaded.destroyed == snapshot.destroyed);
    Level b;
    CHECK(RestoreWorld(b.world, b.guids, loaded) && b.Door()->open && !b.world.IsAlive(b.enemy));
}
