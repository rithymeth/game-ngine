# Aether Destruction: the breakable-things genre kit, built on the physics module. A Destructible component on an entity with a box collider shatters it
# into a grid of physics pieces when it is hit hard enough or takes enough damage; the pieces are debris that fades after a while. Needs physics:
# AETHER_KIT_DESTRUCTION=OFF (or a build without physics) leaves it out of the build and of the player.
add_library(aether_destruction STATIC
    src/destructible.cpp
    src/destruction_system.cpp
    src/destruction_kit.cpp
)
target_include_directories(aether_destruction PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(aether_destruction PUBLIC cxx_std_20)
target_link_libraries(aether_destruction PUBLIC Aether::Physics)
aether_warnings_as_errors(aether_destruction)
add_library(Aether::Destruction ALIAS aether_destruction)
