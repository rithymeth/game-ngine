# Aether Time of Day: the world clock genre kit. An hour and a day count that advance with the game, a phase of the day (dawn, day, dusk,
# night) and a sunlight level scripts can use to light a scene. Optional: AETHER_KIT_TIMEOFDAY=OFF leaves it out of the build (and of the player).
add_library(aether_timeofday STATIC
    src/world_clock.cpp
    src/timeofday_kit.cpp
)
target_include_directories(aether_timeofday PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(aether_timeofday PUBLIC cxx_std_20)
target_link_libraries(aether_timeofday PUBLIC Aether::Engine)
aether_warnings_as_errors(aether_timeofday)
add_library(Aether::TimeOfDay ALIAS aether_timeofday)
