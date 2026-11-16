# Forest Valley: the integrated showcase (docs/design/FOREST_VALLEY_SHOWCASE.md). This library is the portable, headless part: the
# generated 512 m valley, its five areas, the walkable route, the on-foot controller and the repeatable benchmark camera route. It is
# covered by tests/test_forest_valley.cpp. Drawing it is the player's job and is not in this target.
add_library(forest_valley_core STATIC forest_valley.cpp valley_terrain.cpp)
target_include_directories(forest_valley_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(forest_valley_core PUBLIC Aether::Engine Aether::Renderer Aether::Terrain)
