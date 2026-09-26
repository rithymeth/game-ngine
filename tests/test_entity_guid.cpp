#include "aether/platform/filesystem.h"
#include "aether/reflection/serialize.h"
#include "aether/scene/entity_guid.h"
#include "aether/scene/serialization.h"
#include "test_framework.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <unordered_set>

using namespace aether;

namespace {

struct Tag {
    i32 value = 0;
};

std::string ReadText(const std::string& path) {
    std::ifstream file(path);
    return std::string(std::istreambuf_iterator<char>(file), {});
}

} // namespace

AETHER_TEST(EntityGuid_NewGuidsAreUniqueVersion4) {
    std::unordered_set<EntityGuid> seen;
    for (int i = 0; i < 20000; ++i) {
        EntityGuid guid = NewEntityGuid();
        AETHER_CHECK(!guid.IsNull());
        AETHER_CHECK(((guid.hi >> 12) & 0xF) == 4);  // version 4
        AETHER_CHECK(((guid.lo >> 62) & 0x3) == 2);  // RFC variant (binary 10)
        seen.insert(guid);
    }
    AETHER_CHECK(seen.size() == 20000);
}

AETHER_TEST(EntityGuid_StringFormRoundTrips) {
    EntityGuid known{0x5c0a9e2e7b1d4c3aull, 0x9f102a6b8e4d1f77ull};
    AETHER_CHECK(ToString(known) == "5c0a9e2e-7b1d-4c3a-9f10-2a6b8e4d1f77");

    EntityGuid parsed;
    AETHER_CHECK(ParseEntityGuid("5C0A9E2E-7B1D-4C3A-9F10-2A6B8E4D1F77", parsed)); // uppercase accepted
    AETHER_CHECK(parsed == known);

    for (int i = 0; i < 100; ++i) {
        EntityGuid g = NewEntityGuid();
        EntityGuid back;
        AETHER_CHECK(ParseEntityGuid(ToString(g), back) && back == g);
    }

    EntityGuid untouched{1, 2};
    AETHER_CHECK(!ParseEntityGuid("", untouched));
    AETHER_CHECK(!ParseEntityGuid("5c0a9e2e7b1d4c3a9f102a6b8e4d1f77", untouched));      // no dashes
    AETHER_CHECK(!ParseEntityGuid("5c0a9e2e-7b1d-4c3a-9f10-2a6b8e4d1f7", untouched));   // too short
    AETHER_CHECK(!ParseEntityGuid("5c0a9e2e-7b1d-4c3a-9f10-2a6b8e4d1f7g", untouched));  // bad hex
    AETHER_CHECK(!ParseEntityGuid("5c0a9e2e_7b1d-4c3a-9f10-2a6b8e4d1f77", untouched));  // bad separator
    AETHER_CHECK(untouched.hi == 1 && untouched.lo == 2);
}

AETHER_TEST(EntityGuid_ReflectsAsStringThroughConverter) {
    IdComponent id{EntityGuid{0x5c0a9e2e7b1d4c3aull, 0x9f102a6b8e4d1f77ull}};
    reflect::Json json = reflect::ToJson(id);
    AETHER_CHECK(json["guid"] == "5c0a9e2e-7b1d-4c3a-9f10-2a6b8e4d1f77");

    IdComponent loaded;
    reflect::LoadReport report;
    AETHER_CHECK(reflect::FromJson(loaded, json, &report) && report.warnings.empty());
    AETHER_CHECK(loaded.guid == id.guid);

    // Binary archive goes through the same converter.
    IdComponent from_binary;
    AETHER_CHECK(reflect::LoadBinary(from_binary, reflect::SaveBinary(id)) && from_binary.guid == id.guid);

    // Bad data is refused with a warning and leaves the value alone.
    IdComponent bad{EntityGuid{7, 8}};
    AETHER_CHECK(reflect::FromJson(bad, reflect::Json{{"guid", "not-a-guid"}}, &report));
    AETHER_CHECK(bad.guid.hi == 7 && report.warnings.size() == 1);

    // IdComponent is shown read-only in the Inspector.
    AETHER_CHECK(reflect::Reflect<IdComponent>().FindField("guid")->HasFlag(reflect::Field_ReadOnly));
}

AETHER_TEST(GuidIndex_FindsLiveEntitiesAndRejectsStaleOnes) {
    World world;
    GuidIndex index;
    Entity a = world.CreateEntity(Tag{1});
    Entity b = world.CreateEntity(Tag{2});
    EntityGuid ga = EnsureGuid(world, a, &index);
    EntityGuid gb = EnsureGuid(world, b, &index);
    AETHER_CHECK(ga != gb);
    AETHER_CHECK(EnsureGuid(world, a, &index) == ga); // idempotent
    AETHER_CHECK(index.Size() == 2);
    AETHER_CHECK(index.Find(world, ga) == a && index.Find(world, gb) == b);
    AETHER_CHECK(world.GetComponent<Tag>(a)->value == 1); // adding IdComponent kept existing data

    world.DestroyEntity(a);
    AETHER_CHECK(!world.IsAlive(a));
    AETHER_CHECK(index.Find(world, ga) == kNullEntity); // stale: the entity is gone
    Entity recycled = world.CreateEntity(Tag{3});         // likely reuses a's slot
    AETHER_CHECK(index.Find(world, ga) == kNullEntity);  // never resolves to the new occupant
    (void)recycled;

    EntityGuid old_b = gb;
    EntityGuid new_b = RegenerateGuid(world, b, &index);
    AETHER_CHECK(new_b != old_b);
    AETHER_CHECK(index.Find(world, old_b) == kNullEntity);
    AETHER_CHECK(index.Find(world, new_b) == b);
    AETHER_CHECK(index.Find(world, NewEntityGuid()) == kNullEntity);
}

AETHER_TEST(GuidIndex_RebuildDetectsDuplicates) {
    World world;
    EntityGuid shared = NewEntityGuid();
    Entity first = world.CreateEntity(IdComponent{shared}, Tag{1});
    Entity second = world.CreateEntity(IdComponent{shared});   // e.g. a scene loaded twice
    Entity unique = world.CreateEntity(IdComponent{NewEntityGuid()});
    world.CreateEntity(Tag{4});                                  // no identity: ignored

    GuidIndex index;
    std::vector<Entity> duplicates = index.Rebuild(world);
    AETHER_CHECK(index.Size() == 2);
    AETHER_CHECK(duplicates.size() == 1 && duplicates[0] == second);
    AETHER_CHECK(index.Find(world, shared) == first); // the earlier entity keeps the GUID
    AETHER_CHECK(index.Find(world, world.GetComponent<IdComponent>(unique)->guid) == unique);

    RegenerateGuid(world, second, &index);
    AETHER_CHECK(index.Rebuild(world).empty());
    AETHER_CHECK(index.Size() == 3);
}

AETHER_TEST(EntityGuid_SurvivesBothSceneFormats) {
    World world;
    GuidIndex index;
    Entity e = world.CreateEntity(Tag{5});
    EntityGuid guid = EnsureGuid(world, e, &index);
    Entity only_id = world.CreateEntity(IdComponent{NewEntityGuid()}); // identity and nothing else

    std::string binary = (std::filesystem::temp_directory_path() / "aether_test_guid.aesc").string();
    AETHER_CHECK(SaveScene(world, binary));
    World from_binary;
    AETHER_CHECK(LoadScene(from_binary, binary));
    GuidIndex binary_index;
    AETHER_CHECK(binary_index.Rebuild(from_binary).empty());
    AETHER_CHECK(!binary_index.Find(from_binary, guid).IsNull());

    std::string json = (std::filesystem::temp_directory_path() / "aether_test_guid.ascene").string();
    AETHER_CHECK(SaveSceneJson(world, json));
    std::string text = ReadText(json);
    AETHER_CHECK(text.find("\"guid\": \"" + ToString(guid) + "\"") != std::string::npos);
    AETHER_CHECK(text.find("IdComponent") == std::string::npos); // entity-level, not a component entry

    World from_json;
    AETHER_CHECK(LoadSceneJson(from_json, json));
    AETHER_CHECK(from_json.EntityCount() == 2);
    GuidIndex json_index;
    AETHER_CHECK(json_index.Rebuild(from_json).empty());
    Entity found = json_index.Find(from_json, guid);
    AETHER_CHECK(!found.IsNull());
    // Tag isn't reflected, so it doesn't survive JSON — but the identity does.
    AETHER_CHECK(!json_index.Find(from_json, world.GetComponent<IdComponent>(only_id)->guid).IsNull());

    // Re-saving the loaded JSON scene reproduces it byte for byte.
    std::string resaved = (std::filesystem::temp_directory_path() / "aether_test_guid_resaved.ascene").string();
    AETHER_CHECK(SaveSceneJson(from_json, resaved));
    AETHER_CHECK(ReadText(resaved) == text);

    std::filesystem::remove(binary);
    std::filesystem::remove(json);
    std::filesystem::remove(resaved);
}
