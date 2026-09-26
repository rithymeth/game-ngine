#include "aether/reflection/reflection.h"
#include "aether/reflection/serialize.h"
#include "test_framework.h"

#include <string>
#include <vector>

using namespace aether;
using namespace aether::reflect;

namespace arrays_test {

struct Waypoint {
    Vec3 position;
    f32 wait = 0.0f;
};

struct Route {
    std::string name = "route";
    std::vector<Waypoint> points;
    std::vector<std::string> tags;
    std::vector<std::vector<i32>> grid;
    std::vector<u8> flags;
};

} // namespace arrays_test

AETHER_REFLECT(arrays_test::Waypoint, 1, AETHER_FIELD(position), AETHER_FIELD(wait))
AETHER_REFLECT(arrays_test::Route, 1,
    AETHER_FIELD(name, Field_EditAnywhere), AETHER_FIELD(points, Field_EditAnywhere),
    AETHER_FIELD(tags, Field_EditAnywhere), AETHER_FIELD(grid, Field_EditAnywhere),
    AETHER_FIELD(flags, Field_EditAnywhere))

AETHER_TEST(ReflectionArrays_DescribeElementType) {
    const TypeInfo& tags = Reflect<std::vector<std::string>>();
    AETHER_CHECK(tags.kind == TypeKind::Array);
    AETHER_CHECK(std::string(tags.name) == "Array<string>");
    AETHER_CHECK(tags.element == &Reflect<std::string>());
    AETHER_CHECK(std::string(Reflect<std::vector<std::vector<i32>>>().name) == "Array<Array<i32>>");
    AETHER_CHECK(Reflect<arrays_test::Route>().FindField("points")->type->element == &Reflect<arrays_test::Waypoint>());
    AETHER_CHECK(TypeRegistry::Find("Array<string>") == &tags);

    std::vector<std::string> values{"a", "b"};
    AETHER_CHECK(tags.array_size(&values) == 2);
    tags.array_resize(&values, 3);
    AETHER_CHECK(values.size() == 3 && values[2].empty());
    *static_cast<std::string*>(tags.array_element(&values, 2)) = "c";
    AETHER_CHECK(values[2] == "c");
}

AETHER_TEST(ReflectionArrays_RoundTripThroughBothArchives) {
    arrays_test::Route route;
    route.name = "patrol";
    route.points = {{Vec3(1, 2, 3), 0.5f}, {Vec3(4, 5, 6), 0.0f}};
    route.tags = {"night", "guard"};
    route.grid = {{1, 2}, {}, {3}};
    route.flags = {0, 255, 7};

    Json json = ToJson(route);
    AETHER_CHECK(json["tags"] == Json::array({"night", "guard"}));
    AETHER_CHECK(json["points"][1]["position"] == Json::array({4.0, 5.0, 6.0}));
    AETHER_CHECK(json["grid"][1].is_array() && json["grid"][1].empty());

    for (int binary = 0; binary < 2; ++binary) {
        arrays_test::Route loaded;
        loaded.tags = {"stale", "data", "to", "replace"};
        LoadReport report;
        bool ok = binary ? LoadBinary(loaded, SaveBinary(route), &report) : LoadJsonText(loaded, SaveJsonText(route), &report);
        AETHER_CHECK(ok && report.warnings.empty());
        AETHER_CHECK(loaded.tags == route.tags); // the data decides the length
        AETHER_CHECK(loaded.points.size() == 2 && loaded.points[0].wait == 0.5f && loaded.points[1].position.z == 6.0f);
        AETHER_CHECK(loaded.grid == route.grid);
        AETHER_CHECK(loaded.flags == route.flags);
    }
}

AETHER_TEST(ReflectionArrays_BadElementsWarnAndKeepDefaults) {
    arrays_test::Route loaded;
    LoadReport report;
    AETHER_CHECK(LoadJsonText(loaded, R"({"tags": ["ok", 5, "fine"], "points": "nope", "flags": [1, 300]})", &report));
    AETHER_CHECK(loaded.tags.size() == 3 && loaded.tags[0] == "ok" && loaded.tags[1].empty() && loaded.tags[2] == "fine");
    AETHER_CHECK(loaded.points.empty());
    AETHER_CHECK(loaded.flags.size() == 2 && loaded.flags[0] == 1 && loaded.flags[1] == 0);
    AETHER_CHECK(report.warnings.size() == 3);
    bool indexed = false;
    for (const auto& w : report.warnings) {
        indexed = indexed || w.find("Route.tags[1]") != std::string::npos;
    }
    AETHER_CHECK(indexed);
}

AETHER_TEST(ReflectionArrays_CopyThroughAnyAndFieldAccess) {
    arrays_test::Route route;
    route.tags = {"x"};
    const FieldInfo* tags = Reflect<arrays_test::Route>().FindField("tags");
    Any copy = tags->Get(&route);
    AETHER_CHECK(copy.Get<std::vector<std::string>>().size() == 1);
    copy.Get<std::vector<std::string>>().push_back("y");
    AETHER_CHECK(route.tags.size() == 1); // a copy
    AETHER_CHECK(tags->Set(&route, copy) && route.tags.size() == 2);
}
