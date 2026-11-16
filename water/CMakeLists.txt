# Aether Water: the water-and-boats genre kit, built on the physics module. A WaterSurface is a sea, lake or river with waves and a current; a Floater on a body
# makes it float on the waves, point by point, so it pitches, rolls and rights itself; a Boat adds a propeller and a rudder. Needs physics: AETHER_KIT_WATER=OFF (or
# a build without physics) leaves it out of the build and of the player.
add_library(aether_water STATIC
    src/water.cpp
    src/water_system.cpp
    src/water_kit.cpp
)
target_include_directories(aether_water PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(aether_water PUBLIC cxx_std_20)
target_link_libraries(aether_water PUBLIC Aether::Physics)
aether_warnings_as_errors(aether_water)
add_library(Aether::Water ALIAS aether_water)
