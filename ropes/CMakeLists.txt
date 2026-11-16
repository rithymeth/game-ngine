# Aether Ropes: the rope-and-chain genre kit, built on the physics module's joints. A Rope component on an entity becomes a chain of small bodies joined end to
# end by ball-and-socket joints, pinned to the world or to another body at one end, optionally carrying a body at the other, and cut by a script. Needs
# physics: AETHER_KIT_ROPES=OFF (or a build without physics) leaves it out of the build and of the player.
add_library(aether_ropes STATIC
    src/rope.cpp
    src/rope_system.cpp
    src/ropes_kit.cpp
)
target_include_directories(aether_ropes PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(aether_ropes PUBLIC cxx_std_20)
target_link_libraries(aether_ropes PUBLIC Aether::Physics)
aether_warnings_as_errors(aether_ropes)
add_library(Aether::Ropes ALIAS aether_ropes)
