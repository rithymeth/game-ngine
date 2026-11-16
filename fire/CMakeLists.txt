# Aether Fire: the fire genre kit. A Flammable component makes an entity something that can catch, burn for a while on its fuel, spread to what is near, be put out
# by wetness and burn away: drawn with a particle system and a light that follow it. Nothing here needs physics; the flames use the VFX and Renderer components.
# Optional: AETHER_KIT_FIRE=OFF (or a build without VFX and Renderer) leaves it out of the build and of the player.
add_library(aether_fire STATIC
    src/fire.cpp
    src/fire_system.cpp
    src/fire_kit.cpp
)
target_include_directories(aether_fire PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(aether_fire PUBLIC cxx_std_20)
target_link_libraries(aether_fire PUBLIC Aether::Engine Aether::VFX Aether::Renderer)
aether_warnings_as_errors(aether_fire)
add_library(Aether::Fire ALIAS aether_fire)
