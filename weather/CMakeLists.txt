# Aether Weather: the weather genre kit. The sky's mood as numbers a game can use: clouds, rain, snow, fog, wind with gusts, lightning with a thunder delay and
# how wet the ground is, blending smoothly from one kind of weather to the next, by hand or on its own from a seed. Optional: AETHER_KIT_WEATHER=OFF leaves it
# out of the build (and of the player).
add_library(aether_weather STATIC
    src/weather_system.cpp
    src/weather_kit.cpp
)
target_include_directories(aether_weather PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(aether_weather PUBLIC cxx_std_20)
target_link_libraries(aether_weather PUBLIC Aether::Engine)
aether_warnings_as_errors(aether_weather)
add_library(Aether::Weather ALIAS aether_weather)
